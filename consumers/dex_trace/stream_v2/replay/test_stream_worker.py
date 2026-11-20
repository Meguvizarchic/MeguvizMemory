#!/usr/bin/env python3
"""Bounded synthetic contracts for MDXTRC2; no Android or product input."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import time
import tracemalloc
import unittest
from unittest import mock

import stream_parser as wire
import stream_worker as worker


class StreamContracts(unittest.TestCase):
    def setUp(self):
        self.root = wire.safe.build_root() / ('stream-v2-worker-contract-' + secrets.token_hex(12))
        self.root.mkdir()
        self.payload = b'dex\nABCDEFGH'
        self.input = self.root / 'input.dex'
        self.input.write_bytes(self.payload)
        self.core = self.root / 'core.bin'
        self.core.write_bytes(b'not-a-library')
        self.trace = self.root / 'events.bin'
        self.limit = 1_000_001
        self.file_hash = hashlib.sha256(self.payload).hexdigest()

    def value(self, variant='D-C1', case='normal'):
        value = {key: 0 for key in wire.paired.STABLE}
        accepted = case in ('normal', 'cross-thread')
        value.update(schema_version=2, workload='dex_source_strategy_complete_owner_v2', variant=variant,
            case=case, success=True, diagnostic=False, failure_reason=None,
            source_sha256=self.file_hash if accepted else None, input_bytes=len(self.payload),
            logical_requested_bytes=len(self.payload), method_count=1 if accepted else 0,
            expected_status=wire.paired.EXPECTED_STATUS[case], observed_status=wire.paired.EXPECTED_STATUS[case],
            lease_final_correct=True, source_final_released=True, final_resource_state='CLOSED_ZERO',
            snapshot_released_before_last_read=accepted, final_release_on_other_thread=(case == 'cross-thread'),
            input_budget_bytes=32 * 1024 * 1024, parser_budget_bytes=256 * 1024 * 1024,
            callback_retains=0 if case in ('invalid-lease', 'borrow-budget') else 1,
            callback_releases_including_caller=1 if case in ('invalid-lease', 'borrow-budget') else 2,
            resource_scope='source_capacity_plus_unique_input_backing_parser_separate_metadata_RSS_unknown',
            product_scope='independent_DexMeguviz_not_BIL_DexKit')
        return value

    def bytes_for(self, variant='D-C1', case='normal', events=((1, 0), (1001, 5)),
                  consumed=None, limit=None, footer=None):
        payload = b'\0' + self.payload[1:] if case == 'corrupt-input' else self.payload
        consumed = consumed or hashlib.sha256(payload).hexdigest()
        limit = self.limit if limit is None else limit
        header = wire.MAGIC + struct.pack('<QQ', len(self.payload), limit) + consumed.encode('ascii')
        records = b''.join(b'\x01' + struct.pack('<HI', site, offset) for site, offset in events)
        footer = len(events) if footer is None else footer
        ending = b'\x02' + struct.pack('<Q', footer)
        module = b'MGMEM_CORE_MODULE_HEX ' + str(self.core.resolve()).encode('utf-8').hex().encode('ascii') + b'\n'
        report = (json.dumps(self.value(variant, case), separators=(',', ':')) + '\n').encode('utf-8')
        return header + records + ending + module + report

    def prepare(self, variant='D-C1', case='normal', events=((1, 0), (1001, 5))):
        data = self.bytes_for(variant, case, events)
        self.trace.write_bytes(data)
        payload = b'\0' + self.payload[1:] if case == 'corrupt-input' else self.payload
        return {'schema_version': 2, 'input_path': str(self.input), 'trace_path': str(self.trace),
                'variant': variant, 'case': case, 'input_sha256': self.file_hash,
                'input_bytes': len(self.payload), 'consumed_sha256': hashlib.sha256(payload).hexdigest(),
                'trace_sha256': hashlib.sha256(data).hexdigest(), 'trace_bytes': len(data),
                'event_limit': self.limit, 'event_count': len(events), 'core_path': str(self.core),
                'core_sha256': hashlib.sha256(self.core.read_bytes()).hexdigest(),
                'max_trace_bytes': wire.MAX_TRACE_BYTES}

    def scan(self, req, callback=None):
        return wire.scan_file(req['trace_path'], size=req['input_bytes'], file_sha256=req['input_sha256'],
            consumed_sha256=req['consumed_sha256'], limit=req['event_limit'], variant=req['variant'],
            case=req['case'], core_path=Path(req['core_path']), expected_count=req['event_count'],
            expected_trace_sha256=req['trace_sha256'], max_trace_bytes=req['max_trace_bytes'], on_event=callback)

    def test_stream_header_and_callback_preserve_order_above_old_limit(self):
        request = self.prepare()
        calls = []
        result = self.scan(request, lambda site, offset: calls.append((site, offset)))
        self.assertEqual(calls, [(1, 0), (1001, 5)])
        self.assertEqual((result['event_count'], result['site_count']), (2, 2))
        self.assertEqual(result['trace_sha256'], request['trace_sha256'])

    def test_many_events_do_not_form_an_in_memory_event_list(self):
        # Construct the file in fixed blocks; measure only the decoder, not
        # fixture construction. This is a space bound, not a timing benchmark.
        count = 400_000
        empty = self.bytes_for(events=())
        header, tail = empty[:wire.HEADER_BYTES], empty[wire.HEADER_BYTES + 9:]
        record = b'\x01' + struct.pack('<HI', 1, 0)
        block = record * 4096
        with self.trace.open('xb') as stream:
            stream.write(header)
            for _ in range(count // 4096): stream.write(block)
            stream.write(record * (count % 4096))
            stream.write(b'\x02' + struct.pack('<Q', count))
            stream.write(tail)
        sha = hashlib.sha256()
        with self.trace.open('rb') as stream:
            for chunk in iter(lambda: stream.read(65536), b''): sha.update(chunk)
        req = {'trace_path': str(self.trace), 'input_bytes': len(self.payload), 'input_sha256': self.file_hash,
               'consumed_sha256': self.file_hash, 'event_limit': self.limit, 'variant': 'D-C1', 'case': 'normal',
               'core_path': str(self.core), 'event_count': count, 'trace_sha256': sha.hexdigest(),
               'max_trace_bytes': wire.MAX_TRACE_BYTES}
        tracemalloc.start()
        try:
            baseline, _ = tracemalloc.get_traced_memory()
            tracemalloc.reset_peak()
            result = self.scan(req)
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(result['event_count'], count)
        self.assertLess(peak - baseline, 4 * 1024 * 1024)

    def test_zero_events_only_for_explicit_rejections(self):
        for case in ('invalid-lease', 'borrow-budget'):
            with self.subTest(case=case):
                req = self.prepare(case=case, events=())
                self.assertEqual(self.scan(req)['event_count'], 0)
        req = self.prepare(case='normal', events=())
        with self.assertRaises(ValueError): self.scan(req)
        req = self.prepare(case='corrupt-input', events=())
        with self.assertRaises(ValueError): self.scan(req)

    def test_corrupt_case_binds_modified_bytes(self):
        req = self.prepare(variant='D-Z0', case='corrupt-input', events=((1, 0),))
        value = worker.execute(req)
        expected = hashlib.sha256(struct.pack('<IQB', 1, 0, 0)).hexdigest()
        self.assertEqual(value['checksum_sha256'], expected)
        self.assertEqual(value['core_use'], 'NOT_APPLICABLE')

    def test_rejects_truncation_wrong_count_tag_offset_and_tail(self):
        data = self.bytes_for()
        for bad in (data[:-1], data[:88] + b'\x02' + struct.pack('<Q', 2)[:3],
                    data[:88] + b'\x03' + data[89:],
                    self.bytes_for(footer=3), self.bytes_for(events=((1, len(self.payload)),)),
                    self.bytes_for(events=((32, 0),)), data + b'extra'):
            with self.subTest(length=len(bad)):
                req = self.prepare()
                self.trace.write_bytes(bad)
                req['trace_sha256'] = hashlib.sha256(bad).hexdigest()
                req['trace_bytes'] = len(bad)
                with self.assertRaises(ValueError): self.scan(req)

    def test_rejects_wrong_header_identity_limits_and_bool(self):
        req = self.prepare()
        for key, bad in (('event_limit', True), ('input_bytes', True), ('trace_bytes', True),
                         ('consumed_sha256', '0' * 64), ('event_count', 3)):
            changed = dict(req); changed[key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError): worker.execute(changed)
        changed = dict(req); changed['max_trace_bytes'] = req['trace_bytes'] - 1
        with self.assertRaises(ValueError): worker.execute(changed)

    def test_wrong_expected_core_is_rejected_before_loading(self):
        req = self.prepare()
        req['core_sha256'] = '0' * 64
        with mock.patch.object(worker.legacy, 'Workspace') as factory:
            with self.assertRaisesRegex(ValueError, 'INPUT_INVALID'):
                worker.execute(req)
        factory.assert_not_called()

    def test_digest_and_deadline_are_enforced(self):
        req = self.prepare()
        req['trace_sha256'] = '0' * 64
        with self.assertRaises(ValueError): self.scan(req)
        req = self.prepare()
        with self.assertRaisesRegex(ValueError, 'TIMEOUT'):
            wire.scan_file(req['trace_path'], size=req['input_bytes'], file_sha256=req['input_sha256'],
                consumed_sha256=req['consumed_sha256'], limit=req['event_limit'], variant=req['variant'],
                case=req['case'], core_path=Path(req['core_path']), max_trace_bytes=req['max_trace_bytes'],
                deadline=time.monotonic() - 1)

    def test_core_postflight_observed_after_deadline_is_incomplete(self):
        real_fstat = os.fstat
        clock = [0.0]
        calls = [0]
        def late_second_fstat(fd):
            value = real_fstat(fd)
            calls[0] += 1
            if calls[0] == 2: clock[0] = 2.0
            return value
        with mock.patch.object(wire.os, 'fstat', side_effect=late_second_fstat), \
             mock.patch.object(wire.time, 'monotonic', side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(ValueError, 'TIMEOUT'):
                wire._core_identity(self.core, 1.0)

    def test_trace_postflight_observed_after_deadline_is_incomplete(self):
        req = self.prepare()
        original = wire._consumer_tail
        clock = [0.0]
        def late_tail(*args):
            result = original(*args)
            clock[0] = 2.0
            return result
        with mock.patch.object(wire, '_consumer_tail', side_effect=late_tail), \
             mock.patch.object(wire.time, 'monotonic', side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(ValueError, 'TIMEOUT'):
                wire.scan_file(req['trace_path'], size=req['input_bytes'], file_sha256=req['input_sha256'],
                    consumed_sha256=req['consumed_sha256'], limit=req['event_limit'], variant=req['variant'],
                    case=req['case'], core_path=Path(req['core_path']), expected_count=req['event_count'],
                    expected_trace_sha256=req['trace_sha256'], max_trace_bytes=req['max_trace_bytes'], deadline=1.0)

    def test_file_close_crossing_deadline_is_incomplete(self):
        req = self.prepare()
        original = wire._open_regular
        clock = [0.0]
        class LateClose:
            def __init__(self, stream): self.stream = stream
            def __getattr__(self, name): return getattr(self.stream, name)
            def close(self):
                self.stream.close()
                clock[0] = 2.0
        def open_core_late(path, maximum):
            selected, before, stream = original(path, maximum)
            return selected, before, LateClose(stream)
        with mock.patch.object(wire, '_open_regular', side_effect=open_core_late), \
             mock.patch.object(wire.time, 'monotonic', side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(ValueError, 'TIMEOUT'):
                wire._core_identity(self.core, 1.0)
        clock[0] = 0.0
        def open_trace_late(path, maximum):
            selected, before, stream = original(path, maximum)
            return (selected, before, LateClose(stream)) if selected == self.trace else (selected, before, stream)
        with mock.patch.object(wire, '_open_regular', side_effect=open_trace_late), \
             mock.patch.object(wire.time, 'monotonic', side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(ValueError, 'TIMEOUT'):
                wire.scan_file(req['trace_path'], size=req['input_bytes'], file_sha256=req['input_sha256'],
                    consumed_sha256=req['consumed_sha256'], limit=req['event_limit'], variant=req['variant'],
                    case=req['case'], core_path=Path(req['core_path']), expected_count=req['event_count'],
                    expected_trace_sha256=req['trace_sha256'], max_trace_bytes=req['max_trace_bytes'], deadline=1.0)

    def test_tail_rejects_wrong_status_boolean_and_foreign_module(self):
        req = self.prepare()
        data = self.trace.read_bytes()
        start = wire.HEADER_BYTES + 2 * 7 + 9
        module_end = data.index(b'\n', start) + 1
        original = json.loads(data[module_end:].decode('utf-8'))
        for field, bad in (('expected_status', False), ('observed_status', 2),
                           ('closed_backing_bytes', False), ('callback_retains', -1)):
            value = dict(original); value[field] = bad
            changed = data[:module_end] + (json.dumps(value) + '\n').encode('utf-8')
            self.trace.write_bytes(changed)
            copy = dict(req, trace_bytes=len(changed), trace_sha256=hashlib.sha256(changed).hexdigest())
            with self.subTest(field=field), self.assertRaises(ValueError): self.scan(copy)
        other = self.root / 'other-core.bin'; other.write_bytes(self.core.read_bytes())
        line = b'MGMEM_CORE_MODULE_HEX ' + str(other.resolve()).encode('utf-8').hex().encode('ascii') + b'\n'
        changed = data[:start] + line + data[module_end:]
        self.trace.write_bytes(changed)
        copy = dict(req, trace_bytes=len(changed), trace_sha256=hashlib.sha256(changed).hexdigest())
        with self.assertRaises(ValueError): self.scan(copy)

    def test_core_identity_is_bounded_and_changes_cannot_be_hidden(self):
        req = self.prepare()
        with mock.patch.object(wire.safe, 'MAX_FILE', 4):
            with self.assertRaises(ValueError): self.scan(req)
        class Workspace:
            def __init__(self, *args):
                self.core = object(); self.uncertain = False; self.finished = False; self.calls = 0
            def read(self, _offset): return 0
            def close(self): self.calls += 1; raise AssertionError('must not close after identity drift')
        value = Workspace()
        original = wire._core_identity
        seen = []
        def changed(path, deadline):
            result = original(path, deadline)
            seen.append(result)
            return result if len(seen) == 1 else (result[0], '0' * 64)
        with mock.patch.object(worker.legacy, 'Workspace', return_value=value), \
             mock.patch.object(wire, '_core_identity', side_effect=changed):
            with self.assertRaisesRegex(RuntimeError, 'CORE_OR_MEMORY_UNKNOWN'): worker.execute(req)
        self.assertEqual(len(seen), 2)
        self.assertTrue(value.uncertain)
        self.assertEqual(value.calls, 0)

    def test_file_changed_during_scan_is_rejected(self):
        req = self.prepare()
        def change(_site, _offset):
            os.utime(self.trace, ns=(time.time_ns(), time.time_ns() + 1_000_000_000))
        with self.assertRaises(ValueError): self.scan(req, change)

    def test_trace_hardlink_is_rejected(self):
        req = self.prepare()
        try:
            os.link(self.trace, self.root / 'other-link.bin')
        except (OSError, NotImplementedError):
            self.skipTest('creating hardlinks is unavailable')
        with self.assertRaises(ValueError): self.scan(req)

    def test_direct_variants_have_no_memory_domain(self):
        for variant in ('D-C0', 'D-Z0'):
            req = self.prepare(variant=variant)
            result = worker.execute(req)
            expected = hashlib.sha256(b''.join(struct.pack('<IQB', s, o, self.payload[o])
                                              for s, o in ((1, 0), (1001, 5)))).hexdigest()
            self.assertEqual(result['checksum_sha256'], expected)
            self.assertEqual(result['core_use'], 'NOT_APPLICABLE')
            self.assertEqual(result['core_sha256'], hashlib.sha256(self.core.read_bytes()).hexdigest())
            self.assertEqual(result['cleanup_outcome'], 'DIRECT_REFERENCES_CLEARED_NOT_OS_RECLAIM_PROOF')

    def test_isolated_cli_reports_fixed_fields_without_paths(self):
        req = self.prepare(variant='D-Z0')
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps(req), encoding='utf-8')
        result = subprocess.run([sys.executable, '-E', '-s', '-S', str(Path(worker.__file__)),
                                 '--request', str(request_path), '--timeout-seconds', '15'],
                                capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b'')
        self.assertNotIn(str(self.root).encode(), result.stdout)
        output = json.loads(result.stdout)
        self.assertEqual(output['state'], 'PASS_STREAM_V2_READER_BYTE_WORKER')
        self.assertEqual(output['core_sha256'], hashlib.sha256(self.core.read_bytes()).hexdigest())
        self.assertEqual(output['cleanup_outcome'], 'DIRECT_REFERENCES_CLEARED_NOT_OS_RECLAIM_PROOF')

    def test_isolated_cli_rejects_bad_binding_without_paths(self):
        req = self.prepare(variant='D-Z0'); req['trace_sha256'] = '0' * 64
        request_path = self.root / 'bad-request.json'
        request_path.write_text(json.dumps(req), encoding='utf-8')
        result = subprocess.run([sys.executable, '-E', '-s', '-S', str(Path(worker.__file__)),
                                 '--request', str(request_path), '--timeout-seconds', '15'],
                                capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stderr, b'')
        self.assertNotIn(str(self.root).encode(), result.stdout)
        self.assertEqual(json.loads(result.stdout)['state'], 'INCOMPLETE')

    def test_real_system_and_arena_if_available(self):
        candidates = [wire.safe.ROOT / 'build/windows-candidate-ce2a5b2-20261101/candidate-readback/sdk/bin/mgmem.dll',
                      wire.safe.ROOT / 'build/core/Release/mgmem.dll',
                      wire.safe.ROOT / 'build/core/libmgmem.so.1.2.2']
        core = next((p for p in candidates if p.is_file() and not p.is_symlink()), None)
        if core is None: self.skipTest('host shared Memory core unavailable')
        self.core = core
        for variant in ('D-C1', 'D-C2'):
            req = self.prepare(variant=variant)
            result = worker.execute(req)
            self.assertEqual(result['cleanup_outcome'], 'CORE_CLOSED_ZERO_AND_DESTROYED')
            self.assertEqual(result['core_use'], 'REQUESTED_SHARED_CORE_NOT_LOAD_ATTESTED')

    def test_failed_close_is_not_retried(self):
        req = self.prepare()
        class FailedWorkspace:
            def __init__(self, *args):
                self.core = object(); self.uncertain = False; self.finished = False; self.calls = 0
            def read(self, _offset): return 0
            def close(self): self.calls += 1; self.uncertain = True; return 'UNKNOWN_QUARANTINED'
        created = []
        def factory(*args):
            value = FailedWorkspace(*args); created.append(value); return value
        with mock.patch.object(worker.legacy, 'Workspace', side_effect=factory):
            with self.assertRaisesRegex(RuntimeError, 'CORE_OR_MEMORY_UNKNOWN'): worker.execute(req)
        self.assertEqual(created[0].calls, 1)

    def test_late_stream_failure_does_not_try_cleanup(self):
        req = self.prepare()
        class Workspace:
            def __init__(self, *args):
                self.core = object(); self.uncertain = False; self.finished = False; self.calls = 0
            def read(self, _offset): return 0
            def close(self): self.calls += 1; raise AssertionError('must not attempt cleanup')
        value = Workspace()
        first = self.scan(req)
        with mock.patch.object(worker.legacy, 'Workspace', return_value=value), \
             mock.patch.object(worker.wire, 'scan_file', side_effect=[first, ValueError('TRACE_INVALID')]):
            with self.assertRaisesRegex(RuntimeError, 'CORE_OR_MEMORY_UNKNOWN'): worker.execute(req)
        self.assertTrue(value.uncertain)
        self.assertEqual(value.calls, 0)


if __name__ == '__main__':
    unittest.main()
