"""Steady-state frame-time stats for profiler CSVs, skipping the load.

usage: cmp_runs.py SKIP_S WINDOW_S label=path.csv ...
       cmp_runs.py repeat [--hitch-mult M] [--hitch-floor-ms F] label=path.csv[@START-END] ...

The repeat form scores inclusive frame windows of repeated scenario runs and
exits 1 unless p50, p95 and hitch rate each spread at most 10% across runs.
"""
import argparse
import csv
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(__file__))
import analyze  # noqa: E402


def repeat(argv):
    ap = argparse.ArgumentParser(description="Compare repeatability of selected frame windows")
    ap.add_argument("--hitch-mult", type=float, default=2.0)
    ap.add_argument("--hitch-floor-ms", type=float, default=1000 / 30)
    ap.add_argument("runs", nargs="+")
    args = ap.parse_args(argv)
    rule = analyze.HitchRule(args.hitch_mult, args.hitch_floor_ms)
    runs = []
    try:
        for arg in args.runs:
            label, path = arg.split("=", 1)
            bounds = None
            if re.search(r"@\d+-\d+$", path):
                path, bounds = path.rsplit("@", 1)
            rows, _ = analyze.load(path)
            if bounds is not None:
                start, end = bounds.split("-", 1)
                rows = analyze.select_window(rows, int(start), int(end))
            runs.append((label, analyze.window_stats(rows, rule)))
        comparison = analyze.compare_runs([stats for _, stats in runs])
    except ValueError as exc:
        ap.error(str(exc))
    print(analyze.hitch_rule_text(rule))
    print(f"{'run':<18} {'frames':>7} {'duration s':>10} {'p50':>8} {'p95':>8} {'p99':>8} "
          f"{'hitches':>7} {'hitches/min':>11} {'hitch p50':>10}  periodicity")
    for label, stats in runs:
        period = stats['periodicity']
        period_text = (f"{period['median_interval_ms']:.1f} ms" if period['status'] == 'ok'
                       else 'insufficient events')
        hitch_p50 = f"{stats['hitch_ms_p50']:.2f}" if stats['hitch_ms_p50'] is not None else "n/a"
        print(f"{label:<18} {stats['frames']:>7} {stats['duration_s']:>10.1f} "
              f"{stats['p50']:>8.2f} {stats['p95']:>8.2f} {stats['p99']:>8.2f} "
              f"{stats['hitches']:>7} {stats['hitches_per_min']:>11.1f} {hitch_p50:>10}  {period_text}")
    for metric in analyze.RUN_METRICS:
        check = comparison['metrics'][metric]
        print(f"check {metric}: mean {check['mean']:.2f}  spread {check['spread']:.1%}  "
              f"{'PASS' if check['ok'] else 'FAIL'} (at most {analyze.RUN_TOLERANCE:.0%})")
    print(f"overall {'PASS' if comparison['ok'] else 'FAIL'}")
    return 0 if comparison['ok'] else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "repeat":
        return repeat(argv[1:])
    skip, window = float(argv[0]), float(argv[1])
    print(f"{'run':<18}{'frames':>7}{'fps':>8}{'p50 ms':>8}{'p95 ms':>8}{'p99 ms':>8}"
          f"{'draws':>7}{'draw_ms':>8}{'present':>8}{'gpu_ms':>8}")
    for arg in argv[2:]:
        label, path = arg.split("=", 1)
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        t0 = float(rows[0]["t_ms"])
        sel = [r for r in rows if skip * 1000 <= float(r["t_ms"]) - t0 < (skip + window) * 1000]
        ft = sorted(float(r["frame_ms"]) for r in sel)
        q = lambda p: ft[min(len(ft) - 1, int(p * len(ft)))]
        mean = lambda k: statistics.fmean(float(r[k]) for r in sel if r[k] != "")
        print(f"{label:<18}{len(sel):>7}{1000 * len(ft) / sum(ft):>8.1f}{q(.5):>8.3f}{q(.95):>8.3f}"
              f"{q(.99):>8.3f}{mean('draws'):>7.0f}{mean('draw_ms'):>8.3f}{mean('present_ms'):>8.3f}"
              f"{mean('gpu_ms'):>8.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
