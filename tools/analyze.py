#!/usr/bin/env python3
"""Summarize an alamo-profiler frames CSV and (optionally) chart it.

    python tools/analyze.py perf_logs/20261018_201500_frames.csv [--no-chart]

Writes <stem>_summary.txt (and <stem>_chart.png when matplotlib is installed)
next to the CSV, and prints the summary.

The breakdown it reports per frame:
  present_ms   time blocked in Present: GPU still busy, or vsync
  draw_ms      CPU time inside Draw* calls (D3D runtime + driver overhead)
  game_cpu_ms  everything else in the frame: simulation, AI, scripts, scene
               traversal, state setting
zone_* columns are additional wall times and may
overlap the buckets above, so they are reported separately, never summed in.
"""

import argparse
import csv
from dataclasses import dataclass
import math
import os
import re
import sys

BUCKETS = ["game_cpu_ms", "draw_ms", "present_ms"]
LABELS = {"game_cpu_ms": "game CPU", "draw_ms": "draw calls (CPU)", "present_ms": "Present wait"}
MIN_PERIODICITY_EVENTS = 20
RUN_METRICS = ("p50", "p95", "hitches_per_min")
RUN_TOLERANCE = 0.10


def load(path):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        rows = []
        for raw in reader:
            row = {}
            for k, v in raw.items():
                if k is None:
                    continue
                row[k] = float(v) if v not in ("", None) else None
            rows.append(row)
    if not rows:
        sys.exit(f"{path}: no frames")
    return rows, reader.fieldnames


def pct(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def mean(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else float("nan")


def load_memory(path, rows):
    """Join sidecar samples to the analyzed frame rows, including window bounds."""
    if not os.path.exists(path):
        return []
    frames = {row["frame"] for row in rows}
    ints = ("frame", "private_bytes", "largest_free_bytes", "regions", "truncated")
    samples = []
    with open(path, newline="") as f:
        for raw in csv.DictReader(f):
            try:
                sample = {key: int(raw[key]) if key in ints else float(raw[key])
                          for key in ints + ("t_ms", "sample_ms")}
            except (KeyError, TypeError, ValueError):
                continue  # a row cut short when the game died mid-write
            if sample["frame"] in frames:
                samples.append(sample)
    return samples


def format_bytes(value):
    if value < 1024 ** 3:
        return f"{value / 1024 ** 2:.2f} MiB"
    if value < 1024 ** 4:
        return f"{value / 1024 ** 3:.2f} GiB"
    return f"{value / 1024 ** 4:.2f} TiB"


def summarize_memory(samples):
    if not samples:
        return ""
    private = max(samples, key=lambda sample: sample["private_bytes"])
    complete = [sample for sample in samples if not sample["truncated"]]
    free = min(complete, key=lambda sample: sample["largest_free_bytes"]) if complete else None
    free_line = (f"  min largest free block: {format_bytes(free['largest_free_bytes'])} at frame {free['frame']}"
                 if free else "  min largest free block: unavailable (all walks truncated)")
    costs = sorted(sample["sample_ms"] for sample in samples)
    lines = [f"memory samples: {len(samples)}",
             f"  max private bytes: {format_bytes(private['private_bytes'])} at frame {private['frame']}",
             free_line,
             f"  sample_ms p50 {pct(costs, 50):.3f}  max {costs[-1]:.3f}"]
    truncated = sum(bool(sample["truncated"]) for sample in samples)
    if truncated:
        lines.append(f"  truncated walks: {truncated}")
    return "\n".join(lines) + "\n"


def select_window(rows, start_frame: int, end_frame: int) -> list[dict]:
    """Select inclusive frame bounds without changing the CSV clock or order."""
    if start_frame > end_frame:
        raise ValueError("window start frame must not exceed end frame")
    return [row for row in rows if start_frame <= row["frame"] <= end_frame]


@dataclass(frozen=True)
class HitchRule:
    mult: float = 2.0
    floor_ms: float = 1000 / 30


def periodicity(hitch_times_ms):
    """Describe consecutive hitch intervals without changing the window."""
    n = len(hitch_times_ms)
    result = {"status": "insufficient events", "events": n,
              "need": MIN_PERIODICITY_EVENTS, "median_interval_ms": None,
              "interval_spread": None}
    if n >= MIN_PERIODICITY_EVENTS:
        intervals = sorted(b - a for a, b in zip(hitch_times_ms, hitch_times_ms[1:]))
        median = pct(intervals, 50)
        result.update(status="ok", median_interval_ms=median,
                      interval_spread=(pct(intervals, 75) - pct(intervals, 25)) / median
                      if median else None)
    return result


def parse_session(text):
    entries = []
    for line in text.splitlines():
        match = re.match(r"^\[\s*([+-]?\d+(?:\.\d+)?)\]\s*(.*)$", line)
        if match:
            entries.append((float(match[1]), match[2]))
    return entries


def clock_alignment_ms(entries):
    for _, text in entries:
        match = re.fullmatch(r"csv clock origin: first Present at profiler ([+-]?\d+(?:\.\d+)?) ms", text)
        if match:
            return float(match[1])
    return None


def log_to_csv_ms(profiler_ms, alignment_ms):
    return profiler_ms - alignment_ms


def frame_at(rows, csv_ms):
    return next((row for row in rows
                 if row["t_ms"] - row["frame_ms"] < csv_ms <= row["t_ms"]), None)


def compare_runs(stats_list, tolerance=RUN_TOLERANCE):
    if len(stats_list) < 2:
        raise ValueError("at least two runs required")
    rule = stats_list[0]["rule"]
    if any(stats["rule"] != rule for stats in stats_list):
        raise ValueError("runs must use the same hitch rule")
    metrics = {}
    for metric in RUN_METRICS:
        values = [stats[metric] for stats in stats_list]
        average = mean(values)
        spread = (max(values) - min(values)) / average if average else (
            0.0 if all(value == 0 for value in values) else float("inf"))
        metrics[metric] = {"values": values, "mean": average, "spread": spread,
                           "ok": spread <= tolerance}
    return {"metrics": metrics, "rule": dict(rule),
            "ok": all(metric["ok"] for metric in metrics.values())}


def hitch_rule_text(rule):
    return f"hitch rule: frame_ms > {rule.mult:g}x median and > {rule.floor_ms:.1f} ms"


def window_stats(rows, rule: HitchRule) -> dict:
    """Measure a nonempty window on its original CSV clock."""
    if not rows:
        raise ValueError("no frames in selected window")
    ft = sorted(row["frame_ms"] for row in rows)
    duration_s = (rows[-1]["t_ms"] - rows[0]["t_ms"] + rows[0]["frame_ms"]) / 1000
    median = pct(ft, 50)
    threshold = max(rule.mult * median, rule.floor_ms)
    hitch_rows = [row for row in rows if row["frame_ms"] > threshold]
    hitch_ms = sorted(row["frame_ms"] for row in hitch_rows)
    hitches = len(hitch_rows)
    return {
        "frames": len(rows),
        "duration_s": duration_s,
        "fps": len(rows) / duration_s if duration_s else 0.0,
        "p50": median,
        "p95": pct(ft, 95),
        "p99": pct(ft, 99),
        "max": ft[-1],
        "hitches": hitches,
        "hitches_per_min": hitches * 60 / duration_s if duration_s else 0.0,
        "hitch_threshold_ms": threshold,
        "hitch_ms_p50": pct(hitch_ms, 50) if hitch_ms else None,
        "hitch_ms_max": hitch_ms[-1] if hitch_ms else None,
        "periodicity": periodicity([row["t_ms"] for row in hitch_rows]),
        "rule": {"mult": rule.mult, "floor_ms": rule.floor_ms},
        "first_t_ms": rows[0]["t_ms"],
    }


def classify(row):
    """Where a frame most likely spent its time, using a timing heuristic."""
    ft = row["frame_ms"]
    gpu = row.get("gpu_ms")
    if gpu is not None and gpu >= 0.9 * ft:
        return "gpu"
    if row["present_ms"] >= 0.3 * ft:
        return "present"  # GPU or vsync; the session log says whether vsync is on
    return "cpu"


def regress(xs, ys):
    n = len(xs)
    if n < 3:
        return float("nan"), float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0 or syy == 0:
        return float("nan"), float("nan")
    return sxy / sxx, sxy / math.sqrt(sxx * syy)


def compare(rows, cols):
    """Mean of each column on typical frames (p40-p60) vs the slowest 5%."""
    ordered = sorted(rows, key=lambda r: r["frame_ms"])
    n = len(ordered)
    typical = ordered[int(n * 0.4): max(int(n * 0.6), int(n * 0.4) + 1)]
    slow = ordered[min(int(n * 0.95), n - 1):]
    return {c: (mean(r[c] for r in typical), mean(r[c] for r in slow)) for c in cols}, typical, slow


def summarize(rows, fields, rule: HitchRule = HitchRule()):
    lines = []
    out = lines.append
    zones = [f for f in fields if f.startswith("zone_")]
    stats = window_stats(rows, rule)

    out(f"frames {stats['frames']}  duration {stats['duration_s']:.1f} s  avg {stats['fps']:.1f} fps")
    out(f"frame ms  p50 {stats['p50']:.2f}  p95 {stats['p95']:.2f}  "
        f"p99 {stats['p99']:.2f}  max {stats['max']:.2f}")
    slow30 = sum(1 for r in rows if r["frame_ms"] > 1000 / 30)
    out(f"frames over 33.3 ms: {slow30} ({100 * slow30 / stats['frames']:.1f}%)   "
        f"hitches (>{rule.mult:g}x median and >{rule.floor_ms:.1f} ms): {stats['hitches']}")
    out(hitch_rule_text(rule))
    hitch_p50 = f"{stats['hitch_ms_p50']:.2f}" if stats['hitch_ms_p50'] is not None else "n/a"
    hitch_max = f"{stats['hitch_ms_max']:.2f}" if stats['hitch_ms_max'] is not None else "n/a"
    out(f"hitch rate {stats['hitches_per_min']:.1f}/min  duration p50 {hitch_p50} ms  max {hitch_max} ms")
    period = stats["periodicity"]
    if period["status"] == "ok":
        spread = f"{period['interval_spread']:.2f}" if period['interval_spread'] is not None else "n/a"
        out(f"hitch periodicity: median interval {period['median_interval_ms']:.1f} ms  "
            f"spread (IQR/median) {spread}")
    else:
        out(f"hitch periodicity: insufficient events ({period['events']} hitches, need {period['need']})")
    out("")

    cmp, typical, slow = compare(rows, BUCKETS + zones)
    t_total = mean(r["frame_ms"] for r in typical)
    s_total = mean(r["frame_ms"] for r in slow)
    extra = s_total - t_total
    out(f"where the time goes: typical frames (p40-p60, {t_total:.2f} ms) vs slowest 5% ({s_total:.2f} ms)")
    out(f"  {'bucket':<20}{'typical':>10}{'slow':>10}{'extra':>10}{'of extra':>10}")
    for b in BUCKETS:
        t, s = cmp[b]
        share = 100 * (s - t) / extra if extra > 0 else float("nan")
        out(f"  {LABELS[b]:<20}{t:>10.2f}{s:>10.2f}{s - t:>+10.2f}{share:>9.0f}%")
    if zones:
        out("  zones (inclusive, overlap the buckets above):")
        for z in zones:
            t, s = cmp[z]
            out(f"  {z[5:]:<20}{t:>10.2f}{s:>10.2f}{s - t:>+10.2f}")
    out("")

    kinds = {"cpu": 0, "present": 0, "gpu": 0}
    for r in rows:
        kinds[classify(r)] += 1
    n = len(rows)
    out(f"frame limiter (heuristic): CPU {100 * kinds['cpu'] / n:.0f}%  "
        f"Present wait {100 * kinds['present'] / n:.0f}%  GPU {100 * kinds['gpu'] / n:.0f}%")
    has_gpu = sum(1 for r in rows if r.get("gpu_ms") is not None)
    if has_gpu:
        out(f"gpu span ms  p50 {pct(sorted(r['gpu_ms'] for r in rows if r.get('gpu_ms') is not None), 50):.2f}"
            f"  ({has_gpu} of {n} frames timed)")
    else:
        out("gpu span: not available (GpuTiming off or driver lacks timestamp queries)")
    out("")

    draws = sum(r["draws"] for r in rows)
    draw_ms = sum(r["draw_ms"] for r in rows)
    if draws:
        out(f"draw calls  mean {draws / n:.0f}/frame  max {max(r['draws'] for r in rows):.0f}"
            f"  CPU cost {1000 * draw_ms / draws:.1f} us/call")
        slope, r = regress([r["draws"] for r in rows], [r["frame_ms"] for r in rows])
        if math.isnan(slope):
            out("  frame_ms vs draws: n/a (draw count or frame time did not vary)")
        else:
            out(f"  frame_ms vs draws: {1000 * slope:+.2f} ms per 1000 draws"
                f" (r = {r:.2f}; correlation, not cause)")
    for name in ("set_texture", "set_render_state", "set_shader", "set_const"):
        if name in fields:
            out(f"  {name:<17} mean {mean(r[name] for r in rows):.0f}/frame")
    out("")

    marks = [r.get("marks") or 0 for r in rows]
    if any(marks):
        out("segments between F11 marks:")
        seg, start = 0, 0
        for i in range(len(rows) + 1):
            if i == len(rows) or (i > start and marks[i]):
                part = rows[start:i]
                if part:
                    pft = sorted(r["frame_ms"] for r in part)
                    m = {b: mean(r[b] for r in part) for b in BUCKETS}
                    out(f"  seg {seg} t={part[0]['t_ms'] / 1000:.1f}s frames {len(part)}  "
                        f"p50 {pct(pft, 50):.2f}  p95 {pct(pft, 95):.2f}  "
                        f"game {m['game_cpu_ms']:.2f} draw {m['draw_ms']:.2f} present {m['present_ms']:.2f}")
                seg += 1
                start = i
    return "\n".join(lines).rstrip() + "\n"


# Reference palette (validated light mode: slots 1-3 + neutrals).
SURFACE, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9"
SERIES = {"game_cpu_ms": "#2a78d6", "draw_ms": "#eb6834", "present_ms": "#1baf7a"}
ZONE_COLORS = ["#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]


def binned(rows, cols, max_bins=1200):
    size = max(1, math.ceil(len(rows) / max_bins))
    out = []
    for i in range(0, len(rows), size):
        part = rows[i:i + size]
        b = {"t": part[0]["t_ms"] / 1000.0, "worst": max(r["frame_ms"] for r in part)}
        for c in cols:
            b[c] = mean(r.get(c) for r in part)
        b["marks"] = sum(r.get("marks") or 0 for r in part)
        out.append(b)
    return out


def chart(rows, fields, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    zones = [f for f in fields if f.startswith("zone_")][:len(ZONE_COLORS)]
    bins = binned(rows, BUCKETS + ["gpu_ms", "draws"] + zones)
    t = [b["t"] for b in bins]
    panels = 3 if zones else 2
    fig, axes = plt.subplots(panels, 1, figsize=(12, 3.2 * panels + 1), sharex=True,
                             gridspec_kw={"height_ratios": [3] + [1.4] * (panels - 1)},
                             facecolor=SURFACE)
    for ax in axes:
        ax.set_facecolor(SURFACE)
        ax.grid(axis="y", color=GRID, linewidth=0.6)
        ax.tick_params(colors=MUTED, labelsize=9)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(GRID)
        for b in bins:
            if b["marks"]:
                ax.axvline(b["t"], color=MUTED, linewidth=0.8, linestyle=":")

    ax = axes[0]
    base = [0.0] * len(bins)
    for c in BUCKETS:
        top = [lo + (b[c] if not math.isnan(b[c]) else 0.0) for lo, b in zip(base, bins)]
        ax.fill_between(t, base, top, color=SERIES[c], linewidth=0.8, edgecolor=SURFACE,
                        label=LABELS[c])
        base = top
    ax.plot(t, [b["worst"] for b in bins], color=MUTED, linewidth=0.8, label="worst frame in bin")
    gpu = [b["gpu_ms"] for b in bins]
    if any(not math.isnan(g) for g in gpu):
        ax.plot(t, gpu, color=INK2, linewidth=1.2, linestyle="--", label="GPU span")
    ax.set_ylabel("ms per frame", color=INK2)
    ax.set_title("Frame time breakdown (bin means)", color=INK, loc="left", fontsize=12, pad=26)
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), frameon=False, fontsize=9, ncol=5,
              labelcolor=INK2, borderaxespad=0.2)
    ax.set_ylim(bottom=0)

    ax = axes[1]
    ax.plot(t, [b["draws"] for b in bins], color=INK2, linewidth=1.2)
    ax.set_title("Draw calls per frame", color=INK, loc="left", fontsize=11)
    ax.set_ylim(bottom=0)

    if zones:
        ax = axes[2]
        for z, color in zip(zones, ZONE_COLORS):
            ax.plot(t, [b[z] for b in bins], color=color, linewidth=1.5, label=z[5:])
        ax.set_title("Hooked zones, ms per frame (inclusive)", color=INK, loc="left", fontsize=11,
                     pad=22)
        ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), frameon=False, fontsize=9, ncol=5,
                  labelcolor=INK2, borderaxespad=0.2)
        ax.set_ylim(bottom=0)

    axes[-1].set_xlabel("session time (s)", color=INK2)
    fig.tight_layout()
    fig.savefig(path, dpi=110, facecolor=SURFACE)
    plt.close(fig)
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv")
    ap.add_argument("--no-chart", action="store_true")
    ap.add_argument("--memory", metavar="PATH", help="memory CSV sidecar (default: <stem>_memory.csv)")
    ap.add_argument("--window", nargs=2, type=int, metavar=("START", "END"),
                    help="inclusive frame range to analyze and chart")
    ap.add_argument("--hitch-mult", type=float, default=2.0,
                    help="hitch threshold as a multiple of median frame time (default: 2)")
    ap.add_argument("--hitch-floor-ms", type=float, default=1000 / 30,
                    help="minimum hitch frame time in ms (default: 33.3)")
    args = ap.parse_args(argv)
    rows, fields = load(args.csv)
    try:
        if args.window is not None:
            rows = select_window(rows, *args.window)
        text = summarize(rows, fields, HitchRule(args.hitch_mult, args.hitch_floor_ms))
    except ValueError as exc:
        ap.error(str(exc))
    stem = args.csv[:-len("_frames.csv")] if args.csv.endswith("_frames.csv") else os.path.splitext(args.csv)[0]
    try:
        with open(stem + "_session.txt", encoding="utf-8", errors="replace") as f:
            alignment = clock_alignment_ms(parse_session(f.read()))
    except FileNotFoundError:
        alignment = None
    if alignment is not None:
        text += (f"csv clock origin: profiler {alignment:.3f} ms "
                 "(subtract from session log times to get CSV t_ms)\n")
    memory = summarize_memory(load_memory(args.memory or stem + "_memory.csv", rows))
    if memory:
        text += "\n" + memory
    with open(stem + "_summary.txt", "w") as f:
        f.write(text)
    print(text, end="")
    if not args.no_chart:
        if chart(rows, fields, stem + "_chart.png"):
            print(f"chart: {stem}_chart.png")
        else:
            print("chart skipped: pip install matplotlib")


if __name__ == "__main__":
    main()
