#!/usr/bin/env python3
"""Focused host checks for scoped reader replay; never uses a device."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import unittest
from unittest import mock

import paired_replay_worker as subject


class ReplayWorkerTests(unittest.TestCase):
    def setUp(self):
        build = subject.bounded.build_root()
        self.root = build / ('dex-replay-worker-contract-' + secrets.token_hex(12))
        self.root.mkdir()
        self.input = self.root / 'input.dex'
        self.input.write_bytes(b'dex\nABCD')
        self.stdout = self.root / 'case.stdout'
        self.core = self.root / 'fake-core'
        self.core.write_bytes(b'not a shared library')

    def request(self, variant='D-Z0', case='normal', events=((1, 0), (1001, 5))):
        payload = self.input.read_bytes()
        consumed = (b'\0' + payload[1:]) if case == 'corrupt-input' else payload
        digest = hashlib.sha256(consumed).hexdigest()
        report = {'variant': variant, 'case': case, 'success': True,
                  'expected_status': subject.STATUSES[case], 'observed_status': subject.STATUSES[case]}
        lines = ['MGMEM_CORE_MODULE_HEX 61', json.dumps(report),
                 'MGMEM_DEX_READER_TRACE_V1 ' + str(len(events)) + ' ' + digest]
        lines.extend('E %d %d' % event for event in events)
        data = ('\n'.join(lines) + '\n').encode()
        self.stdout.write_bytes(data)
        return {'schema_version': 1, 'input_path': str(self.input), 'stdout_path': str(self.stdout),
                'variant': variant, 'case': case, 'input_sha256': hashlib.sha256(payload).hexdigest(),
                'input_bytes': len(payload), 'stdout_sha256': hashlib.sha256(data).hexdigest(),
                'stdout_bytes': len(data), 'consumed_sha256': digest, 'event_count': len(events),
                'event_limit': 100, 'core_path': str(self.core)}

    def expected(self, payload, events):
        digest = hashlib.sha256()
        for site, offset in events:
            digest.update(struct.pack('<IQB', site, offset, payload[offset]))
        return digest.hexdigest()

    def test_direct_copy_and_preloaded_borrow(self):
        for variant in ('D-C0', 'D-Z0'):
            with self.subTest(variant=variant):
                result = subject.execute(self.request(variant))
                self.assertEqual(result['state'], 'PASS_SCOPED_READER_BYTE_WORKER')
                self.assertEqual(result['checksum_sha256'], self.expected(self.input.read_bytes(), ((1, 0), (1001, 5))))
                self.assertEqual(result['events'], 2)
                self.assertEqual(result['core_use'], 'NOT_APPLICABLE')
                self.assertEqual(result['cleanup_outcome'], 'DIRECT_REFERENCES_CLEARED_NOT_OS_RECLAIM_PROOF')

    def test_corrupt_snapshot_is_used_and_not_original(self):
        result = subject.execute(self.request(case='corrupt-input', events=((2, 0),)))
        self.assertEqual(result['checksum_sha256'], self.expected(b'\0' + self.input.read_bytes()[1:], ((2, 0),)))

    def test_zero_events_only_for_explicit_rejections(self):
        for case in ('invalid-lease', 'borrow-budget'):
            result = subject.execute(self.request(case=case, events=()))
            self.assertEqual(result['checksum_sha256'], hashlib.sha256().hexdigest())
        with self.assertRaisesRegex(ValueError, 'TRACE_INVALID'):
            subject.execute(self.request(case='normal', events=()))

    def test_forged_hash_rejected_before_allocation(self):
        request = self.request()
        request['stdout_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'INPUT_INVALID'):
            subject.execute(request)
        request = self.request()
        request['consumed_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'INPUT_INVALID'):
            subject.execute(request)

    def test_out_of_range_bad_site_and_extra_lines_rejected(self):
        for events in (((1, 8),), ((32, 0),)):
            with self.subTest(events=events), self.assertRaisesRegex(ValueError, 'TRACE_INVALID'):
                subject.execute(self.request(events=events))
        request = self.request()
        data = self.stdout.read_bytes() + b'E 1 0\n'
        self.stdout.write_bytes(data)
        request.update(stdout_bytes=len(data), stdout_sha256=hashlib.sha256(data).hexdigest())
        with self.assertRaisesRegex(ValueError, 'TRACE_INVALID'):
            subject.execute(request)

    def test_bool_count_and_unknown_field_rejected(self):
        request = self.request()
        request['event_count'] = True
        with self.assertRaisesRegex(ValueError, 'INPUT_INVALID'):
            subject.execute(request)
        request = self.request()
        request['extra'] = True
        with self.assertRaisesRegex(ValueError, 'INPUT_INVALID'):
            subject.execute(request)

    def test_hardlinked_input_rejected(self):
        request = self.request()
        try:
            os.link(self.input, self.root / 'linked.dex')
        except (OSError, NotImplementedError):
            self.skipTest('creating hardlinks is unavailable')
        with self.assertRaises(ValueError):
            subject.execute(request)

    def test_worker_cli_does_not_report_raw_input_or_path(self):
        request = self.request()
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps(request), encoding='utf-8')
        result = subprocess.run([sys.executable, str(subject.ROOT / 'consumers/dex_trace/paired_replay_worker.py'),
                                 '--request', str(request_path)], capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b'')
        report = json.loads(result.stdout)
        self.assertEqual(report['state'], 'PASS_SCOPED_READER_BYTE_WORKER')
        self.assertNotIn(self.input.read_bytes(), result.stdout)
        self.assertNotIn(str(self.root).encode(), result.stdout)

    def test_failed_core_release_is_not_retried(self):
        class Failing:
            def __init__(self): self.calls = []
            def mgmem_release_v1(self, *args): self.calls.append('release'); return 9
            def __getattr__(self, name):
                def unexpected(*args): self.calls.append(name); raise AssertionError(name)
                return unexpected
        ws = subject.Workspace.__new__(subject.Workspace)
        ws.core = Failing(); ws.domain = ctypes.c_void_p(1); ws.owner = subject.image.Handle(1, 2, 3)
        ws.uncertain = False; ws.finished = False
        self.assertEqual(ws.close(), 'UNKNOWN_QUARANTINED')
        self.assertEqual(ws.close(), 'UNKNOWN_QUARANTINED')
        self.assertEqual(ws.core.calls, ['release'])

    def test_failed_allocation_does_not_guess_at_cleanup(self):
        class Failing:
            def __init__(self): self.calls = []
            def mgmem_capabilities_v1(self): self.calls.append('capabilities'); return 27
            def mgmem_domain_create_v1(self, options, output):
                self.calls.append('create')
                ctypes.cast(output, ctypes.POINTER(ctypes.c_void_p))[0] = ctypes.c_void_p(123)
                return 0
            def mgmem_allocate_v1(self, *args): self.calls.append('allocate'); return 3
            def __getattr__(self, name):
                def unexpected(*args): self.calls.append(name); raise AssertionError(name)
                return unexpected
        fake = Failing()
        with mock.patch.object(subject.image, 'load_core', return_value=fake):
            with self.assertRaisesRegex(RuntimeError, 'CORE_OR_MEMORY_UNKNOWN'):
                subject.execute(self.request('D-C1'))
        self.assertEqual(fake.calls, ['capabilities', 'create', 'allocate'])

    def test_real_core_system_arena_if_available(self):
        candidates = [
            subject.ROOT / 'build/windows-candidate-ce2a5b2-20261101/candidate-readback/sdk/bin/mgmem.dll',
            subject.ROOT / 'build/core/Release/mgmem.dll',
            subject.ROOT / 'build/core/libmgmem.so.1.2.2',
        ]
        core = next((p for p in candidates if p.is_file() and not p.is_symlink()), None)
        if core is None:
            self.skipTest('host shared Memory core unavailable')
        for variant in ('D-C1', 'D-C2'):
            with self.subTest(variant=variant):
                request = self.request(variant)
                request['core_path'] = str(core)
                result = subject.execute(request)
                self.assertEqual(result['checksum_sha256'], self.expected(self.input.read_bytes(), ((1, 0), (1001, 5))))
                self.assertEqual(result['core_use'], 'REQUESTED_SHARED_CORE_NOT_LOAD_ATTESTED')
                self.assertEqual(result['cleanup_outcome'], 'CORE_CLOSED_ZERO_AND_DESTROYED')


if __name__ == '__main__':
    unittest.main()
