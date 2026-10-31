"""Tests for tools/analyze.py.  Run: python -m unittest discover tools"""

import csv
import contextlib
from contextlib import redirect_stdout
import io
import os
import shutil
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, os.path.dirname(__file__))
import analyze  # noqa: E402
import cmp_runs  # noqa: E402


class TestDirectory:
    """Use inherited Windows permissions so restricted runners retain access."""

    def __init__(self):
        self.name = os.path.join(tempfile.gettempdir(), "alamo-test-" + uuid.uuid4().hex)
        os.mkdir(self.name, 0o777 if os.name == "nt" else 0o700)

    def cleanup(self):
        shutil.rmtree(self.name)

    def __enter__(self):
        return self.name

    def __exit__(self, *args):
        self.cleanup()

FIELDS = ["frame", "t_ms", "frame_ms", "present_ms", "draw_ms", "game_cpu_ms", "gpu_ms",
          "draws", "prims", "set_texture", "set_render_state", "set_shader", "set_stream",
          "set_const", "set_rt", "clears", "scenes", "marks", "zone_ai"]


def synth(n=1000):
    """Steady 20 ms frames; every 20th frame is a 60 ms hitch caused by game CPU.
    Draw cost is a flat 2 us per draw. A mark lands at frame 500."""
    rows, t = [], 0.0
    for i in range(1, n + 1):
        hitch = i % 20 == 0
        draws = 1500
        draw_ms = draws * 0.002
        present = 5.0
        game = 12.0 + (40.0 if hitch else 0.0)
        frame = game + draw_ms + present
        t += frame
        rows.append({
            "frame": i, "t_ms": f"{t:.3f}", "frame_ms": f"{frame:.3f}", "present_ms": present,
            "draw_ms": f"{draw_ms:.3f}", "game_cpu_ms": f"{game:.3f}",
            "gpu_ms": "" if i % 2 else "9.000", "draws": draws, "prims": draws * 100,
            "set_texture": 800, "set_render_state": 3000, "set_shader": 400, "set_stream": 900,
            "set_const": 2500, "set_rt": 6, "clears": 2, "scenes": 1,
            "marks": 1 if i == 500 else 0, "zone_ai": 30.0 if hitch else 2.0,
        })
    return rows


class AnalyzeTest(unittest.TestCase):
    def setUp(self):
        self.dir = TestDirectory()
        self.csv = os.path.join(self.dir.name, "x_frames.csv")
        with open(self.csv, "w", newline="") as f:
            w = csv.DictWriter(f, FIELDS)
            w.writeheader()
            w.writerows(synth())

    def tearDown(self):
        self.dir.cleanup()

    def test_summary_attributes_hitches_to_game_cpu(self):
        rows, fields = analyze.load(self.csv)
        text = analyze.summarize(rows, fields)
        self.assertIn("frames 1000", text)
        self.assertIn("p50 20.00", text)
        self.assertIn("hitches (>2x median and >33.3 ms): 50", text)
        game_line = next(l for l in text.splitlines() if l.strip().startswith("game CPU"))
        self.assertIn("+40.00", game_line)
        self.assertIn("100%", game_line)
        self.assertIn("2.0 us/call", text)
        self.assertIn("ai", text)
        self.assertIn("seg 1", text)
        self.assertIn("500 of 1000 frames timed", text)

    def test_blank_gpu_is_none(self):
        rows, _ = analyze.load(self.csv)
        self.assertIsNone(rows[0]["gpu_ms"])
        self.assertEqual(rows[1]["gpu_ms"], 9.0)

    def test_session_origin_in_summary(self):
        stem = self.csv[:-len("_frames.csv")]
        for content, expected in (("[ 10.0] no origin\n", False),
                                  ("[ 5000.0] csv clock origin: first Present at profiler 5000.000 ms\n", True)):
            with self.subTest(content=content):
                with open(stem + "_session.txt", "w") as f:
                    f.write(content)
                with redirect_stdout(io.StringIO()):
                    analyze.main([self.csv, "--no-chart"])
                with open(stem + "_summary.txt") as f:
                    text = f.read()
                line = ("csv clock origin: profiler 5000.000 ms "
                        "(subtract from session log times to get CSV t_ms)")
                self.assertEqual(line in text, expected)

    def test_classify(self):
        self.assertEqual(analyze.classify({"frame_ms": 20, "present_ms": 1, "gpu_ms": 19}), "gpu")
        self.assertEqual(analyze.classify({"frame_ms": 20, "present_ms": 8, "gpu_ms": None}), "present")
        self.assertEqual(analyze.classify({"frame_ms": 20, "present_ms": 1, "gpu_ms": 5}), "cpu")

    def test_main_writes_summary_and_chart(self):
        analyze.main([self.csv])
        stem = self.csv[:-len("_frames.csv")]
        self.assertTrue(os.path.exists(stem + "_summary.txt"))
        try:
            import matplotlib  # noqa: F401
        except ImportError:
            return
        self.assertGreater(os.path.getsize(stem + "_chart.png"), 10000)


class MemoryAnalysisTest(unittest.TestCase):
    def setUp(self):
        self.dir = TestDirectory()
        self.csv = os.path.join(self.dir.name, "x_frames.csv")
        self.memory = os.path.join(self.dir.name, "x_memory.csv")
        with open(self.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, FIELDS)
            writer.writeheader()
            writer.writerows(synth(180))

    def tearDown(self):
        self.dir.cleanup()

    def write_memory(self, path=None):
        with open(path or self.memory, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["frame", "t_ms", "private_bytes", "largest_free_bytes",
                             "regions", "truncated", "sample_ms"])
            writer.writerows([
                [0, 0, 1024 ** 4, 1, 100, 1, 100],
                [60, 1200, 512 * 1024 ** 2, 3 * 1024 ** 4, 100, 0, 1],
                [120, 2400, 2 * 1024 ** 3, 1024 ** 3, 120, 1, 3],
                [180, 3600, 1024 ** 3, 512 * 1024 ** 2, 140, 0, 5],
                [181, 3620, 1024 ** 4, 1, 100, 1, 100],
            ])

    def run_main(self, *args):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            analyze.main([self.csv, "--no-chart", *args])
        with open(os.path.join(self.dir.name, "x_summary.txt")) as f:
            self.assertEqual(f.read(), output.getvalue())
        return output.getvalue()

    def test_extrema_with_frames_and_costs(self):
        self.write_memory()
        text = self.run_main()
        self.assertIn("memory samples: 3", text)
        self.assertIn("max private bytes: 2.00 GiB at frame 120", text)
        self.assertIn("min largest free block: 512.00 MiB at frame 180", text)
        self.assertIn("sample_ms p50 3.000  max 5.000", text)
        self.assertIn("truncated walks: 1", text)

    def test_inclusive_window(self):
        self.write_memory()
        text = self.run_main("--window", "60", "120")
        self.assertIn("memory samples: 2", text)
        self.assertIn("min largest free block: 3.00 TiB at frame 60", text)
        self.assertIn("sample_ms p50 2.000  max 3.000", text)
        self.assertNotIn("memory samples", self.run_main("--window", "61", "119"))

    def test_missing_sidecar_preserves_summary(self):
        rows, fields = analyze.load(self.csv)
        self.assertEqual(self.run_main(), analyze.summarize(rows, fields))

    def test_truncated_walk_excluded_only_from_free_minimum(self):
        self.write_memory()
        with open(self.memory, "a", newline="") as f:
            csv.writer(f).writerow([61, 1220, 4 * 1024 ** 3, 0, 1, 1, 9])
        text = self.run_main("--window", "60", "61")
        self.assertIn("memory samples: 2", text)
        self.assertIn("max private bytes: 4.00 GiB at frame 61", text)
        self.assertIn("min largest free block: 3.00 TiB at frame 60", text)
        self.assertIn("sample_ms p50 5.000  max 9.000", text)
        self.assertIn("truncated walks: 1", text)

    def test_all_walks_truncated(self):
        self.write_memory()
        with open(self.memory, "a", newline="") as f:
            csv.writer(f).writerow([121, 2420, 4 * 1024 ** 3, 0, 1, 1, 9])
        text = self.run_main("--window", "120", "121")
        self.assertIn("memory samples: 2", text)
        self.assertIn("max private bytes: 4.00 GiB at frame 121", text)
        self.assertIn("min largest free block: unavailable (all walks truncated)", text)
        self.assertIn("sample_ms p50 6.000  max 9.000", text)
        self.assertIn("truncated walks: 2", text)

    def test_memory_override(self):
        override = os.path.join(self.dir.name, "other.csv")
        self.write_memory(override)
        self.assertIn("memory samples: 3", self.run_main("--memory", override))
        # An explicit empty sidecar takes precedence over the default sidecar.
        self.write_memory()
        with open(override, "w") as f:
            f.write("frame,t_ms,private_bytes,largest_free_bytes,regions,truncated,sample_ms\n")
        self.assertNotIn("memory samples", self.run_main("--memory", override))

    def test_only_frames_present_are_joined(self):
        self.write_memory()
        samples = analyze.load_memory(self.memory, [{"frame": 60}, {"frame": 180}])
        self.assertEqual([sample["frame"] for sample in samples], [60, 180])
        self.assertNotIn("truncated walks", analyze.summarize_memory(samples))

    def test_partial_last_row_is_skipped(self):
        self.write_memory()
        with open(self.memory, "a") as f:
            f.write("120,2400,99")
        samples = analyze.load_memory(self.memory, [{"frame": 120}])
        self.assertEqual([sample["private_bytes"] for sample in samples], [2 * 1024 ** 3])

    def test_byte_units(self):
        for value, expected in [(0, "0.00 MiB"), (1024 ** 3 - 1, "1024.00 MiB"),
                                (1024 ** 3, "1.00 GiB"), (1024 ** 4, "1.00 TiB"),
                                (3 * 1024 ** 4, "3.00 TiB")]:
            self.assertEqual(analyze.format_bytes(value), expected)


class WindowAnalysisTest(unittest.TestCase):
    @staticmethod
    def rows(frame_times, start_frame=1, start_t_ms=0.0):
        rows, t_ms = [], start_t_ms
        for frame, frame_ms in enumerate(frame_times, start_frame):
            t_ms += frame_ms
            rows.append({
                "frame": frame, "t_ms": t_ms, "frame_ms": frame_ms,
                "present_ms": 0.0, "draw_ms": 0.0,
                "game_cpu_ms": frame_ms, "draws": 0,
            })
        return rows

    def test_window_late_in_session(self):
        rows = self.rows([10.0] * 36001)
        window = analyze.select_window(rows, 30001, 36000)
        stats = analyze.window_stats(window, analyze.HitchRule())
        self.assertEqual(stats["frames"], 6000)
        self.assertEqual(window[0]["frame"], 30001)
        self.assertEqual(window[-1]["frame"], 36000)
        self.assertAlmostEqual(stats["duration_s"], 60.0, delta=0.01)
        self.assertAlmostEqual(stats["fps"], 100.0, delta=0.5)
        self.assertIn("frames 6000  duration 60.0 s  avg 100.0 fps",
                      analyze.summarize(window, list(window[0])))

    def test_frames_outside_window_ignored(self):
        rows = self.rows([500.0, 10.0, 10.0, 10.0, 500.0])
        window = analyze.select_window(rows, 2, 4)
        stats = analyze.window_stats(window, analyze.HitchRule())
        self.assertEqual(stats["max"], 10.0)
        self.assertEqual(stats["hitches"], 0)
        self.assertEqual(stats["frames"], 3)

    def test_hitch_needs_both_thresholds(self):
        rows = self.rows([16.7] * 9 + [30.0])
        stats = analyze.window_stats(rows, analyze.HitchRule(1.5, 25))
        self.assertEqual(stats["hitches"], 1)
        self.assertAlmostEqual(stats["hitches_per_min"], 60 / stats["duration_s"])
        self.assertEqual(analyze.window_stats(rows, analyze.HitchRule())["hitches"], 0)
        # Each threshold independently vetoes a hitch, including equality.
        for rule in (analyze.HitchRule(2, 25), analyze.HitchRule(1.5, 30),
                     analyze.HitchRule(30 / 16.7, 25)):
            with self.subTest(rule=rule):
                self.assertEqual(analyze.window_stats(rows, rule)["hitches"], 0)

    def test_rule_recorded(self):
        rows = self.rows([10.0, 20.0, 30.0])
        stats = analyze.window_stats(rows, analyze.HitchRule(1.5, 25))
        self.assertEqual(set(stats), {
            "frames", "duration_s", "fps", "p50", "p95", "p99", "max",
            "hitches", "hitches_per_min", "rule", "first_t_ms",
            "hitch_ms_p50", "hitch_ms_max", "periodicity", "hitch_threshold_ms",
        })
        self.assertEqual(stats["rule"], {"mult": 1.5, "floor_ms": 25})
        self.assertEqual(stats["p50"], 20.0)
        self.assertAlmostEqual(stats["p95"], 29.0)
        self.assertAlmostEqual(stats["p99"], 29.8)
        self.assertAlmostEqual(stats["duration_s"], 0.06)
        defaults = analyze.window_stats(rows, analyze.HitchRule())
        self.assertEqual(defaults["rule"], {"mult": 2.0, "floor_ms": 1000 / 30})

    def test_original_timestamps_kept(self):
        rows = self.rows([10.0] * 5, start_frame=30000, start_t_ms=300000.0)
        original = [row.copy() for row in rows]
        window = analyze.select_window(rows, 30001, 30003)
        stats = analyze.window_stats(window, analyze.HitchRule())
        self.assertEqual(stats["first_t_ms"], 300020.0)
        self.assertEqual([row["t_ms"] for row in window], [300020.0, 300030.0, 300040.0])
        self.assertEqual(rows, original)

    def test_late_marked_window_hitch_metrics(self):
        times = [10.0] * 36000
        for index in range(30099, 36000, 100):
            times[index] = 60.0
        rows = self.rows(times)
        selected = analyze.select_window(rows, 30001, 36000)
        stats = analyze.window_stats(selected, analyze.HitchRule())
        self.assertEqual(stats['frames'], 6000)
        self.assertEqual(stats['duration_s'], 63.0)
        self.assertEqual(stats['p50'], 10.0)
        self.assertEqual(stats['p95'], 10.0)
        self.assertAlmostEqual(stats['hitch_threshold_ms'], 1000 / 30)
        self.assertEqual(stats['hitches'], 60)
        self.assertAlmostEqual(stats['hitches_per_min'], 60 * 60 / 63)
        self.assertEqual(stats['hitch_ms_p50'], 60.0)
        self.assertEqual(stats['hitch_ms_max'], 60.0)
        self.assertEqual(stats['periodicity']['median_interval_ms'], 1050.0)
        self.assertEqual(stats['periodicity']['interval_spread'], 0.0)
        # Change only outside frames, preserving the selected CSV clock.
        for row in rows[:30000]:
            row['frame_ms'] = 10000.0
        rows += self.rows([50000.0], start_frame=36001, start_t_ms=rows[-1]['t_ms'])
        self.assertEqual(analyze.window_stats(analyze.select_window(rows, 30001, 36000),
                                             analyze.HitchRule()), stats)

    def test_periodicity(self):
        fixed = analyze.periodicity([i * 1234.0 for i in range(20)])
        self.assertEqual(fixed, {'status': 'ok', 'events': 20, 'need': 20,
                                 'median_interval_ms': 1234.0, 'interval_spread': 0.0})
        irregular = [0.0]
        for interval in [100.0, 1000.0, 5000.0] * 7:
            irregular.append(irregular[-1] + interval)
        self.assertGreater(analyze.periodicity(irregular)['interval_spread'],
                           fixed['interval_spread'])
        self.assertEqual(analyze.periodicity(list(range(19))), {
            'status': 'insufficient events', 'events': 19, 'need': 20,
            'median_interval_ms': None, 'interval_spread': None})
        self.assertIsNone(analyze.periodicity([1.0] * 20)['interval_spread'])

    def test_no_hitches(self):
        rows = self.rows([10.0] * 20)
        stats = analyze.window_stats(rows, analyze.HitchRule())
        self.assertIsNone(stats['hitch_ms_p50'])
        self.assertIsNone(stats['hitch_ms_max'])
        text = analyze.summarize(rows, list(rows[0]))
        self.assertIn('hitch rate 0.0/min  duration p50 n/a ms  max n/a ms', text)
        self.assertIn('hitch periodicity: insufficient events (0 hitches, need 20)', text)

    def test_session_clock_and_frame_spans(self):
        entries = analyze.parse_session(
            'unbracketed\n[    5000.000] csv clock origin: first Present at profiler 5000.000 ms\n'
            '[    5123.400] event\n[bad] ignored\n')
        self.assertEqual(entries, [(5000.0, 'csv clock origin: first Present at profiler 5000.000 ms'),
                                   (5123.4, 'event')])
        alignment = analyze.clock_alignment_ms(entries)
        self.assertEqual(alignment, 5000.0)
        csv_ms = analyze.log_to_csv_ms(entries[1][0], alignment)
        self.assertAlmostEqual(csv_ms, 123.4)
        rows = self.rows([10.0] * 20)
        window = analyze.select_window(rows, 11, 15)
        self.assertIs(analyze.frame_at(window, csv_ms), rows[12])
        self.assertIs(analyze.frame_at(window, 120.0), rows[11])
        self.assertIsNone(analyze.frame_at(window, 100.0))
        self.assertIsNone(analyze.frame_at(window, 151.0))
        self.assertIsNone(analyze.clock_alignment_ms([(1.0, 'other')]))

    def test_compare_runs(self):
        base = analyze.window_stats(self.rows([10.0] * 100 + [60.0]), analyze.HitchRule())
        nearby = dict(base, p50=10.5, p95=10.5,
                      hitches_per_min=base['hitches_per_min'] * 1.05)
        result = analyze.compare_runs([base, nearby, base])
        self.assertTrue(result['ok'])
        self.assertEqual(result['rule'], base['rule'])
        for metric in analyze.RUN_METRICS:
            with self.subTest(metric=metric):
                changed = dict(base, **{metric: base[metric] * 1.3})
                result = analyze.compare_runs([base, changed, base])
                self.assertFalse(result['ok'])
                self.assertFalse(result['metrics'][metric]['ok'])
                self.assertEqual(result['metrics'][metric]['values'],
                                 [base[metric], changed[metric], base[metric]])
        zeros = dict(base, hitches=0, hitches_per_min=0.0)
        check = analyze.compare_runs([zeros, zeros, zeros])['metrics']['hitches_per_min']
        self.assertEqual(check['spread'], 0.0)
        self.assertTrue(check['ok'])
        self.assertFalse(analyze.compare_runs([base, zeros, zeros])['ok'])
        with self.assertRaises(ValueError):
            analyze.compare_runs([base])
        with self.assertRaises(ValueError):
            analyze.compare_runs([base, dict(base, rule={'mult': 3, 'floor_ms': 25})])


class CompareRunsCliTest(unittest.TestCase):
    def test_repeat_and_legacy(self):
        with TestDirectory() as directory:
            paths = []
            for name, frame_ms in [('a', 10.0), ('b', 10.5), ('c', 30.0)]:
                path = os.path.join(directory, name + '@capture.csv')
                rows = WindowAnalysisTest.rows([frame_ms] * 100)
                for row in rows:
                    row['gpu_ms'] = 1.0
                with open(path, 'w', newline='') as f:
                    writer = csv.DictWriter(f, list(rows[0]))
                    writer.writeheader()
                    writer.writerows(rows)
                paths.append(path)
            for last, code in ((1, 0), (2, 1)):
                with self.subTest(code=code), redirect_stdout(io.StringIO()) as output:
                    result = cmp_runs.main(['repeat', '--hitch-mult', '1.5',
                                            '--hitch-floor-ms', '25',
                                            'a=' + paths[0] + '@10-90', 'b=' + paths[last]])
                self.assertEqual(result, code)
                text = output.getvalue()
                self.assertIn('hitch rule: frame_ms > 1.5x median and > 25.0 ms', text)
                self.assertIn('overall ' + ('PASS' if code == 0 else 'FAIL'), text)
                self.assertIn('insufficient events', text)
                self.assertIn('81', text)
            with redirect_stdout(io.StringIO()) as output:
                self.assertEqual(cmp_runs.main(['0', '10', 'a=' + paths[0]]), 0)
            self.assertEqual(output.getvalue().splitlines()[0],
                             f"{'run':<18}{'frames':>7}{'fps':>8}{'p50 ms':>8}{'p95 ms':>8}{'p99 ms':>8}"
                             f"{'draws':>7}{'draw_ms':>8}{'present':>8}{'gpu_ms':>8}")


if __name__ == "__main__":
    unittest.main()
