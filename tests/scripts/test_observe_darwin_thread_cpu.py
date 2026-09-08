#!/usr/bin/env python3
"""Deterministic receipt/log/delta tests; not macOS permission or unit proof."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("observer", ROOT / "scripts/tools/observe_darwin_thread_cpu.py")
observer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(observer)


def snapshot(index=0, **changes):
    value = dict(status="ok", schema_version=1, units="nanoseconds", pid=42, native_tid=2**40 + 7,
        start_sec=100, start_usec=999, monotonic_begin_ns=1_000_000_000 + index*2_000_000_000,
        monotonic_end_ns=1_001_000_000 + index*2_000_000_000,
        user_ns=10_000_000 + index*1_000_000_000,
        system_ns=1_000_000 + index*10_000_000, thread_state=1)
    value.update(changes)
    return value


def row(index=0):
    frame = dict(frame=100 + index*50, wall_ms=30.0, draws=1000)
    return dict(status="sample", snapshot=snapshot(index), frame_before=frame, frame_after=frame.copy())


def qualify_envelope_rows(rows):
    for value in rows:
        snap = value['snapshot']
        value.setdefault('selector', dict(device=1, native_tid=2**40+7, transport=1))
        value.setdefault('log_metadata', dict(device=4, inode=9, modified_unix_ns=100))
        value.setdefault('observer_begin_ns', snap['monotonic_begin_ns'])
        value.setdefault('observer_end_ns', snap['monotonic_end_ns'])
    return rows


class ReceiptTests(unittest.TestCase):
    def test_large_native_id_preserved(self):
        value = snapshot()
        self.assertEqual(observer.qualified_snapshot(value, 42, 2**40+7), value)

    def test_bad_snapshots_fail_closed(self):
        changes = [dict(status="error"), dict(schema_version=2), dict(schema_version=True), dict(units="ticks"), dict(pid=43),
            dict(native_tid=7), dict(user_ns=-1), dict(system_ns=True), dict(start_usec=1_000_000),
            dict(start_sec=0), dict(monotonic_end_ns=999_999_999),
            dict(monotonic_end_ns=1_200_000_000), dict(user_ns=None)]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                observer.qualified_snapshot(snapshot(**change), 42, 2**40+7)

    def test_exact_delta_population(self):
        report = observer.summarize([row(0), row(1), row(2)], 100, 300)
        self.assertEqual(report['frames'], 100)
        self.assertEqual(report['user_ns'], 2_000_000_000)
        self.assertEqual(report['system_ns'], 20_000_000)
        self.assertAlmostEqual(report['cpu_ms_per_frame'], 20.2)

    def test_error_breaks_adjacency_not_zero_cpu(self):
        report = observer.summarize([row(0), dict(status="query_error"), row(2), row(3)], 100, 300)
        self.assertEqual(report['frames'], 50)
        self.assertEqual(report['rejected']['query_error'], 1)
        self.assertEqual(report['intervals'][0]['first_frame'], 200)

    def test_unstable_endpoint_breaks_adjacency(self):
        unstable = row(1)
        unstable['frame_after']['frame'] += 1
        report = observer.summarize([row(0), unstable, row(2), row(3)], 100, 300)
        self.assertEqual(report['frames'], 50)

    def test_loading_static_and_terminal_endpoints(self):
        loading = row(0)
        loading['frame_before']['draws'] = loading['frame_after']['draws'] = 2
        report = observer.summarize([loading, row(1), row(2), row(3)], 100, 250)
        self.assertEqual(report['frames'], 50)
        static = row(1)
        static['frame_before']['frame'] = static['frame_after']['frame'] = 100
        report = observer.summarize([row(0), static], 100, 300)
        self.assertIsNone(report['cpu_ms_per_frame'])
        self.assertEqual(report['status'], 'unmeasured')

    def test_identity_changes_rejected(self):
        for key in ('pid', 'native_tid', 'start_sec', 'start_usec'):
            changed = row(1)
            changed['snapshot'][key] += 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                observer.summarize([row(0), changed], 100, 300)

    def test_cpu_and_time_regressions_rejected(self):
        for change in (dict(user_ns=1), dict(system_ns=1),
                       dict(monotonic_begin_ns=1, monotonic_end_ns=2)):
            changed = row(1)
            changed['snapshot'].update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                observer.summarize([row(0), changed], 100, 300)

    def test_device_selector_mixing_rejected(self):
        first, second = row(0), row(1)
        first['selector'] = dict(device=1, native_tid=2**40+7, transport=1)
        second['selector'] = dict(device=2, native_tid=2**40+7, transport=1)
        with self.assertRaises(ValueError):
            observer.summarize([first, second], 100, 300)
        report = observer.summarize([first, row(1)], 100, 300)
        self.assertIsNone(report['selector'])

    def test_envelope_includes_unstable_interior_and_reports_exclusions(self):
        unstable = row(1)
        unstable['frame_after']['frame'] += 1
        prefix = row(0)
        prefix['frame_before']['draws'] = prefix['frame_after']['draws'] = 2
        samples = qualify_envelope_rows([row(0), unstable, row(2)])
        suffix = row(3)
        suffix['frame_before']['frame'] = suffix['frame_after']['frame'] = 300
        report = observer.summarize_envelope(
            [prefix, *samples, suffix], 100, 300)
        self.assertEqual(report['status'], 'partial')
        self.assertEqual(report['frames'], 100)
        self.assertEqual(report['frame_span'], 100)
        self.assertEqual(report['user_ns'], 2_000_000_000)
        self.assertEqual(report['system_ns'], 20_000_000)
        self.assertEqual(report['excluded_prefix']['rows'], 1)
        self.assertEqual(report['excluded_suffix']['rows'], 1)
        self.assertIn('not exact Present timestamps', report['endpoint_approximation'])
        self.assertIn('game/Wine/synchronous Unix', report['qualification'])

    def test_envelope_verifies_selector_and_log_provenance(self):
        rows = [row(0), row(1), row(2)]
        qualify_envelope_rows(rows)
        report = observer.summarize_envelope(rows, 100, 300)
        self.assertEqual(report['selector_provenance'], 'stable')
        self.assertEqual(report['log_provenance'], 'stable-file-identity')

    def test_envelope_rejects_internal_identity_counter_query_and_selector_changes(self):
        cases = []
        changed_identity = row(1)
        changed_identity['frame_after']['frame'] += 1
        changed_identity['snapshot']['pid'] += 1
        cases.append(changed_identity)
        changed_counter = row(1)
        changed_counter['frame_after']['frame'] += 1
        changed_counter['snapshot']['user_ns'] = 1
        cases.append(changed_counter)
        bad_query = row(1)
        bad_query['frame_after']['frame'] += 1
        bad_query['observer_begin_ns'] = 0
        bad_query['observer_end_ns'] = 1
        cases.append(bad_query)
        changed_selector = row(1)
        changed_selector['frame_after']['frame'] += 1
        changed_selector['selector'] = dict(device=2, native_tid=2**40+7, transport=1)
        cases.append(changed_selector)
        for interior in cases:
            first, last = row(0), row(2)
            qualify_envelope_rows([first, interior, last])
            with self.subTest(interior=interior), self.assertRaises(ValueError):
                observer.summarize_envelope([first, interior, last], 100, 300)

    def test_envelope_does_not_bridge_internal_query_error(self):
        endpoints = qualify_envelope_rows([row(0), row(2)])
        with self.assertRaisesRegex(ValueError, 'non-sample row'):
            observer.summarize_envelope(
                [endpoints[0], dict(status='query_error'), endpoints[1]], 100, 300)

    def test_envelope_rejects_missing_selector_metadata_and_query_bounds(self):
        for field in ('selector', 'log_metadata', 'observer_begin_ns', 'observer_end_ns'):
            rows = qualify_envelope_rows([row(0), row(1), row(2)])
            rows[1].pop(field)
            with self.subTest(field=field), self.assertRaises(ValueError):
                observer.summarize_envelope(rows, 100, 300)

    def test_envelope_rejects_provenance_frame_and_typed_selector_changes(self):
        cases = []
        rows = qualify_envelope_rows([row(0), row(1), row(2)])
        rows[1]['log_metadata']['inode'] += 1
        cases.append(('log inode', rows))
        rows = qualify_envelope_rows([row(0), row(1), row(2)])
        rows[1]['frame_before']['wall_ms'] = float('nan')
        cases.append(('nonfinite wall', rows))
        rows = qualify_envelope_rows([row(0), row(1), row(2)])
        rows[1]['selector']['transport'] = True
        cases.append(('typed transport', rows))
        for name, values in cases:
            with self.subTest(name=name), self.assertRaises(ValueError):
                observer.summarize_envelope(values, 100, 300)

    def test_envelope_rejects_monotonic_and_sample_regressions(self):
        rows = qualify_envelope_rows([row(0), row(1), row(2)])
        rows[1]['snapshot']['monotonic_begin_ns'] = 1
        rows[1]['snapshot']['monotonic_end_ns'] = 2
        with self.assertRaises(ValueError):
            observer.summarize_envelope(rows, 100, 300)
        rows = qualify_envelope_rows([row(0), row(1), row(2)])
        rows[1]['frame_before']['frame'] = 90
        with self.assertRaises(ValueError):
            observer.summarize_envelope(rows, 100, 300)

    def test_envelope_no_frame_progress_is_unmeasured(self):
        static = row(1)
        static['frame_before']['frame'] = static['frame_after']['frame'] = 100
        report = observer.summarize_envelope(qualify_envelope_rows([row(0), static]), 100, 300)
        self.assertEqual(report['status'], 'unmeasured')
        self.assertEqual(report['frames'], 0)
        self.assertEqual(report['rejected']['no_frame_progress'], 1)


class LogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'dxmt9.log'
        self.path.write_bytes(b'')
        self.log = observer.LiveLog(self.path)

    def append(self, text):
        with self.path.open('a') as stream:
            stream.write(text)

    def test_incremental_marker_frame_and_partial_line(self):
        self.append('[dxmt9-device] info: unix_commit_chunk_entry device=0x123 native_tid=0x10000000007')
        self.log.read()
        self.assertIsNone(self.log.native_tid)
        self.append('\n[dxmt9-perf-frame frame=100 wall_ms=30.00 command_buffers=4 draw_calls=1000 extra=0]\n')
        self.log.read()
        self.assertEqual(self.log.native_tid, 2**40+7)
        self.assertEqual(self.log.frame['frame'], 100)
        self.log.read()
        self.assertEqual(self.log.frame['frame'], 100)

    def test_other_tid_namespace_is_not_selector(self):
        self.append('[pe] thread_id=0x777\n[queue] native_tid=0x888\n')
        self.log.read()
        self.assertIsNone(self.log.native_tid)

    def test_second_device_on_same_thread_rejected(self):
        self.append('command chunk negotiation selected canonical transport=0x1\n'
                    'unix_commit_chunk_entry device=0x123 native_tid=0x77\n'
                    'command chunk negotiation selected canonical transport=0x1\n')
        with self.assertRaises(ValueError):
            self.log.read()

    def test_ambiguous_tid_or_device_rejected(self):
        for device, tid in [('0x124', '0x77'), ('0x123', '0x78')]:
            self.path.write_text('unix_commit_chunk_entry device=0x123 native_tid=0x77\n'
                                 f'unix_commit_chunk_entry device={device} native_tid={tid}\n')
            with self.subTest(device=device, tid=tid), self.assertRaises(ValueError):
                observer.LiveLog(self.path).read()

    def test_truncated_or_replaced_log_rejected(self):
        self.append('a long original log\n')
        self.log.read()
        self.path.write_text('x')
        with self.assertRaises(ValueError):
            self.log.read()
        replacement = self.path.with_suffix('.new')
        replacement.write_text('a much longer replacement log\n')
        replacement.replace(self.path)
        with self.assertRaises(ValueError):
            self.log.read()

    def test_duplicate_frame_and_bounded_read_rejected(self):
        line = '[dxmt9-perf-frame frame=100 wall_ms=30.0 draw_calls=1000 tail=0]\n'
        self.append(line + line)
        with self.assertRaises(ValueError):
            self.log.read()
        with mock.patch.object(observer, 'LIMIT', 8), self.assertRaises(ValueError):
            observer.LiveLog(self.path).read()


if __name__ == '__main__':
    unittest.main()
