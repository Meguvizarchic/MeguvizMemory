"""Offline, host-only contracts for Android candidate assembly; no NDK or ADB."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import build_android_candidate as subject
from source_lock import LOCAL_DIRECTORIES, scoped_files


class CandidateContracts(unittest.TestCase):
    def test_static_section_requires_complete_initialized_payload(self):
        subject.validate_static_section_payload('SHT_PROGBITS', 2, b'xy')
        subject.validate_static_section_payload('SHT_PROGBITS', 0, b'')
        subject.validate_static_section_payload('SHT_NOBITS', 8, b'')
        with self.assertRaisesRegex(ValueError, 'static_section_size'):
            subject.validate_static_section_payload('SHT_PROGBITS', 2, b'')
        with self.assertRaisesRegex(ValueError, 'static_section_size'):
            subject.validate_static_section_payload('SHT_PROGBITS', 2, b'x')
        with self.assertRaisesRegex(ValueError, 'static_nobits_has_payload'):
            subject.validate_static_section_payload('SHT_NOBITS', 8, b'x')

    def test_static_contract_rejects_missing_initialized_section_data(self):
        section = ('  Section {\n    Name: .data\n    Type: SHT_PROGBITS\n'
                   '    Flags [ (0x2)\n      SHF_ALLOC\n    ]\n    Size: 2\n'
                   '    AddressAlignment: 8\n  }\n')
        with patch.object(subject, 'run_tool', side_effect=['pressure.o\n', section]):
            with self.assertRaisesRegex(ValueError, 'static_section_size'):
                subject.static_contract('readobj', 'nm', 'ar', Path('pressure.a'), Path('.'), [], 'test')

    def fixture(self, directory):
        root = directory / 'source'
        root.mkdir()
        names = ['CMakeLists.txt', 'src/mgmem.cpp', 'cmake/MeguvizMemoryConfig.cmake.in',
                 'adapters/android/CMakeLists.txt', 'adapters/android/native/pressure.cpp',
                 'adapters/android/native/pressure_jni.cpp', 'adapters/android/native/borrow_jni.cpp',
                 'adapters/android/native/workspace_jni.cpp', 'adapters/android/consumer-rules.pro',
                 'third_party_notices/llvm-NOTICE.txt']
        names += [subject.JAVA_BASE + name + '.java' for name in subject.REQUIRED_JAVA]
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('fixture:' + name).encode())
        lock = directory / 'source-lock.json'
        records = [{'root': 'MeguvizMemory', 'path': p.relative_to(root).as_posix(), **subject.file_identity(p)}
                   for p in scoped_files(root, LOCAL_DIRECTORIES, lock)]
        records.sort(key=lambda row: (row['root'], row['path']))
        lock.write_bytes(subject.json_bytes({'schema_version': 1, 'scope': 'SCOPED_SOURCE_TREE_LOCK',
                                            'file_count': len(records), 'files': records,
                                            'source_set_sha256': subject.sha(subject.canonical(records))}))
        return root, lock

    def test_source_lock_copy_and_drift(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); root, lock = self.fixture(directory)
            raw, records = subject.verify_lock(root, lock)
            self.assertTrue(raw)
            snapshot = directory / 'copy'
            subject.copy_snapshot(root, snapshot, records)
            subject.verify_snapshot(snapshot, records)
            (snapshot / 'src/mgmem.cpp').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'snapshot_changed'):
                subject.verify_snapshot(snapshot, records)
            (root / 'src/mgmem.cpp').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'source_lock_mismatch'):
                subject.verify_lock(root, lock)

    def test_source_lock_rejects_added_and_tampered_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); root, lock = self.fixture(directory)
            (root / 'src/unrecorded.cpp').write_bytes(b'extra')
            with self.assertRaisesRegex(ValueError, 'source_set_mismatch'):
                subject.verify_lock(root, lock)
            data = json.loads(lock.read_text(encoding='utf-8'))
            data['files'][0]['sha256'] = 'a' * 64
            lock.write_bytes(subject.json_bytes(data))
            with self.assertRaisesRegex(ValueError, 'source_lock_hash'):
                subject.verify_lock(root, lock)

    def test_archive_names_and_roundtrip(self):
        archive = subject.zip_bytes({'a/file.txt': b'a', 'b/file.bin': b'b'})
        self.assertEqual(subject.archive_members(archive), {'a/file.txt': b'a', 'b/file.bin': b'b'})
        for name in ('../escape', '/absolute', 'C:/outside', 'x\\bad'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                subject.zip_bytes({name: b'bad'})

    def test_recursive_privacy_and_unsafe_members(self):
        nested = subject.zip_bytes({'inner.txt': b'private-needle'})
        outer = subject.zip_bytes({'nested.zip': nested})
        with self.assertRaisesRegex(ValueError, 'private_identifier'):
            subject.Scanner(['private-needle']).archive(outer, 'outer')
        nested = subject.zip_bytes({'inner.txt': b'C:/Users/private/file'})
        with self.assertRaisesRegex(ValueError, 'machine_path'):
            subject.Scanner(['never-match']).archive(nested, 'path')

    def test_jni_exports_reject_old_or_test_symbols(self):
        expected = {'Java_io_github_jichuo1_meguviz_memory_android_ManagedMemoryDomain_nativeAttachPressure'}
        symbol = next(iter(expected))
        subject.validate_jni_symbols('  1: 00000010  16 FUNC GLOBAL DEFAULT 12 ' + symbol, expected)
        with self.assertRaisesRegex(ValueError, 'jni_export_missing'):
            subject.validate_jni_symbols('  1: 00000000 0 FUNC GLOBAL DEFAULT UND ' + symbol, expected)
        with self.assertRaisesRegex(ValueError, 'jni_export_missing'):
            subject.validate_jni_symbols('  1: 00000010 16 FUNC WEAK DEFAULT 12 ' + symbol, expected)
        with self.assertRaisesRegex(ValueError, 'test_native_export'):
            subject.validate_jni_symbols('  1: 00000010 16 FUNC GLOBAL DEFAULT 12 ' + symbol +
                '\n  2: 00000020 16 FUNC GLOBAL DEFAULT 12 mgmem_pressure_test_x', expected)
        with self.assertRaisesRegex(ValueError, 'unresolved_pressure_symbol'):
            subject.validate_jni_symbols('  1: 00000010 16 FUNC GLOBAL DEFAULT 12 ' + symbol +
                '\n  2: 00000000 0 FUNC GLOBAL DEFAULT UND mgmem_pressure_create_v2', expected)

    def test_keep_rules_reject_missing_pressure_class(self):
        rules = b'\n'.join(('-keep class io.github.jichuo1.meguviz.memory.android.' + n + ' { *; }').encode()
                           for n in (*subject.REQUIRED_JAVA, 'ManagedMemoryDomain$Allocation',
                                     'ManagedMemoryDomain$Lease', 'ManagedMemoryDomain$Pressure',
                                     'ManagedMemoryDomain$ManagedException'))
        subject.validate_keep_rules(rules)
        with self.assertRaisesRegex(ValueError, 'keep_rule_missing'):
            subject.validate_keep_rules(rules.replace(b'ManagedMemoryDomain$Pressure', b'MissingPressure'))

    def test_sdk_exact_member_allowlist(self):
        files = {'include/mgmem/a.h': b'h', 'lib/libmgmem.so': b'a', 'lib/libmgmem_pressure.a': b'b',
                 'lib/libmgmem_pressure_jni.so': b'jni',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryConfig.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryConfigVersion.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryTargets.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryTargets-release.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniABI.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets-release.cmake': b'',
                 'share/MeguvizMemory/licenses/llvm-NOTICE.txt': b'',
                 'share/MeguvizMemory/licenses/LICENSE': b'',
                 'share/MeguvizMemory/licenses/NOTICE': b''}
        subject.validate_sdk_members(files, {'a.h': b'h'})
        with self.assertRaisesRegex(ValueError, 'sdk_unexpected_or_missing_member'):
            subject.validate_sdk_members({**files, 'bin/unexpected': b'x'}, {'a.h': b'h'})
        del files['lib/libmgmem.so']
        with self.assertRaisesRegex(ValueError, 'sdk_unexpected_or_missing_member'):
            subject.validate_sdk_members(files, {'a.h': b'h'})

    def test_sdk_requires_installed_jni_target_metadata(self):
        files = {'include/mgmem/a.h': b'h', 'lib/libmgmem.so': b'a', 'lib/libmgmem_pressure.a': b'b',
                 'lib/libmgmem_pressure_jni.so': b'jni',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryConfig.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryConfigVersion.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryTargets.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryTargets-release.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniABI.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets.cmake': b'',
                 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets-release.cmake': b'',
                 'share/MeguvizMemory/licenses/llvm-NOTICE.txt': b'',
                 'share/MeguvizMemory/licenses/LICENSE': b'',
                 'share/MeguvizMemory/licenses/NOTICE': b''}
        subject.validate_sdk_members(files, {'a.h': b'h'})
        for name in ('lib/libmgmem_pressure_jni.so',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniABI.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets.cmake'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'sdk_unexpected_or_missing_member'):
                subject.validate_sdk_members({k: v for k, v in files.items() if k != name}, {'a.h': b'h'})

    def test_expected_failure_must_match_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); operations = []
            rejected = subprocess.CompletedProcess(['fake'], 1, stdout=b'', stderr=b'known rejection')
            with patch.object(subject.subprocess, 'run', return_value=rejected):
                subject.run_tool(['fake'], root, operations, 'expected', expected_error='known rejection')
                with self.assertRaisesRegex(ValueError, 'expected_rejection_missing'):
                    subject.run_tool(['fake'], root, operations, 'wrong-reason', expected_error='different reason')
            unexpected = subprocess.CompletedProcess(['fake'], 0, stdout=b'known rejection', stderr=b'')
            with patch.object(subject.subprocess, 'run', return_value=unexpected):
                with self.assertRaisesRegex(ValueError, 'expected_rejection_missing'):
                    subject.run_tool(['fake'], root, operations, 'unexpected-success', expected_error='known rejection')

    def test_aar_sdk_independent_readback_rejects_mixed_binaries(self):
        abi = 'arm64-v8a'
        aar = subject.zip_bytes({'jni/' + abi + '/libmgmem.so': b'core',
                                 'jni/' + abi + '/libmgmem_pressure_jni.so': b'jni'})
        sdk = {'lib/libmgmem.so': b'core', 'lib/libmgmem_pressure_jni.so': b'jni',
               'lib/libmgmem_pressure.a': b'pressure'}
        contracts = {'core': {'candidate': subject.ident(b'core')},
                     'jni': {'candidate': subject.ident(b'jni')},
                     'static': {'candidate': subject.ident(b'pressure')}}
        subject.validate_output_identities(aar, subject.zip_bytes(sdk), abi, contracts)
        for key, reason in (('lib/libmgmem.so', 'sdk_aar_core_mismatch'),
                            ('lib/libmgmem_pressure_jni.so', 'sdk_aar_jni_mismatch'),
                            ('lib/libmgmem_pressure.a', 'sdk_static_mismatch')):
            bad = {**sdk, key: b'stale'}
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, reason):
                subject.validate_output_identities(aar, subject.zip_bytes(bad), abi, contracts)

    def test_wrong_and_truncated_elf_rejected(self):
        with self.assertRaisesRegex(ValueError, 'invalid_elf'):
            subject.elf_parts(b'not-elf', 'arm64-v8a')
        raw = bytearray(64); raw[:4] = b'\x7fELF'; raw[4] = 2; raw[5] = 1
        raw[16:18] = (3).to_bytes(2, 'little'); raw[18:20] = (62).to_bytes(2, 'little')
        with self.assertRaisesRegex(ValueError, 'elf_wrong_abi'):
            subject.elf_parts(bytes(raw), 'arm64-v8a')
        with self.assertRaisesRegex(ValueError, 'elf_bad_tables'):
            subject.elf_parts(bytes(raw), 'x86_64')

    def test_load_alignment_and_congruence(self):
        raw = bytearray(512); raw[:4] = b'\x7fELF'; raw[4] = 2; raw[5] = 1
        struct.pack_into('<HH', raw, 16, 3, 183)
        struct.pack_into('<Q', raw, 32, 64); struct.pack_into('<Q', raw, 40, 128)
        struct.pack_into('<HH', raw, 54, 56, 1); struct.pack_into('<HHH', raw, 58, 64, 2, 1)
        struct.pack_into('<IIQQQQQQ', raw, 64, 1, 5, 0, 0, 0, 128, 128, 16384)
        names = b'\x00.shstrtab\x00'; raw[320:320 + len(names)] = names
        struct.pack_into('<IIQQQQIIQQ', raw, 192, 1, 3, 0, 0, 320, len(names), 0, 0, 1, 0)
        subject.verify_stripped_sections(bytes(raw), bytes(raw), 'arm64-v8a')
        struct.pack_into('<Q', raw, 64 + 48, 4096)
        with self.assertRaisesRegex(ValueError, 'elf_not_16k_load_aligned'):
            subject.elf_parts(bytes(raw), 'arm64-v8a')
        struct.pack_into('<Q', raw, 64 + 48, 65536)
        struct.pack_into('<Q', raw, 64 + 16, 16384)
        with self.assertRaisesRegex(ValueError, 'elf_not_16k_load_aligned'):
            subject.elf_parts(bytes(raw), 'arm64-v8a')
        struct.pack_into('<Q', raw, 64 + 48, 16384)
        struct.pack_into('<Q', raw, 64 + 16, 4096)
        with self.assertRaisesRegex(ValueError, 'elf_not_16k_load_aligned'):
            subject.elf_parts(bytes(raw), 'arm64-v8a')

    def test_generator_binding(self):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / 'tool.py'; file.write_bytes(b'known')
            records = {'tools/tool.py': subject.file_identity(file)}
            subject.verify_generator_bindings(records, {'tool': file})
            file.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'generator_source_mismatch'):
                subject.verify_generator_bindings(records, {'tool': file})

    def test_atomic_publication_refuses_overwrite_and_alias(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); temp = root / 'temp'; dest = root / 'result.zip'
            temp.write_bytes(b'first')
            subject.publish_no_replace(temp, dest)
            self.assertFalse(temp.exists())
            self.assertEqual(dest.read_bytes(), b'first')
            next_temp = root / 'next'; next_temp.write_bytes(b'second')
            with self.assertRaises(OSError):
                subject.publish_no_replace(next_temp, dest)
            self.assertEqual(next_temp.read_bytes(), b'second')
            self.assertEqual(dest.read_bytes(), b'first')

    def test_timeout_retains_partial_output_and_journal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); operations = []
            timeout = subprocess.TimeoutExpired('fake', 1, output=b'partial', stderr=b'incomplete')
            with patch.object(subject.subprocess, 'run', side_effect=timeout):
                with self.assertRaisesRegex(ValueError, 'tool_timeout_step'):
                    subject.run_tool(['fake'], root, operations, 'step', timeout=1)
            self.assertEqual((root / 'logs/step.stdout').read_bytes(), b'partial')
            self.assertEqual((root / 'logs/step.stderr').read_bytes(), b'incomplete')
            self.assertEqual(operations[0]['state'], 'TIMEOUT')
            self.assertEqual(json.loads((root / 'operations.jsonl').read_text(encoding='utf-8'))['state'], 'TIMEOUT')

    def test_tool_step_duplicate_does_not_rerun_or_overwrite_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); operations = []
            result = subprocess.CompletedProcess(['fake'], 0, stdout=b'first-output', stderr=b'first-error')
            with patch.object(subject.subprocess, 'run', return_value=result) as run:
                subject.run_tool(['fake'], root, operations, 'step')
                before = {name: (root / 'logs' / ('step.' + name)).read_bytes() for name in ('stdout', 'stderr')}
                journal = (root / 'operations.jsonl').read_bytes()
                with self.assertRaisesRegex(ValueError, 'duplicate_tool_step'):
                    subject.run_tool(['fake'], root, operations, 'step')
                with self.assertRaisesRegex(ValueError, 'duplicate_tool_step'):
                    subject.run_tool(['fake'], root, operations, 'STEP')
                self.assertEqual(run.call_count, 1)
                self.assertEqual(before, {name: (root / 'logs' / ('step.' + name)).read_bytes()
                                          for name in ('stdout', 'stderr')})
                self.assertEqual((root / 'operations.jsonl').read_bytes(), journal)

    def test_tool_step_preexisting_log_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); log = root / 'logs'; log.mkdir()
            (log / 'step.stderr').write_bytes(b'previous')
            with patch.object(subject.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'tool_log_exists'):
                    subject.run_tool(['fake'], root, [], 'step')
                run.assert_not_called()
            self.assertEqual((log / 'step.stderr').read_bytes(), b'previous')
            self.assertFalse((log / 'step.stdout').exists())

    def create_real_symlink_or_report_unavailable(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            detail = {'state': 'UNAVAILABLE', 'scope': 'REAL_SYMLINK_CREATION',
                      'error_type': type(error).__name__, 'errno': getattr(error, 'errno', None),
                      'winerror': getattr(error, 'winerror', None)}
            print(json.dumps(detail), file=sys.stderr)
            self.skipTest('UNAVAILABLE_REAL_SYMLINK_CREATION')
        self.assertTrue(link.is_symlink(), 'real symlink required; no mock fallback')

    def test_tool_step_rejects_real_logs_directory_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); target = root / 'existing-log-target'; target.mkdir()
            sentinel = target / 'keep.txt'; sentinel.write_bytes(b'unchanged')
            link = root / 'logs'
            self.create_real_symlink_or_report_unavailable(link, target, directory=True)
            with patch.object(subject.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'tool_log_dir_invalid'):
                    subject.run_tool(['fake'], root, [], 'step')
                run.assert_not_called()
            self.assertEqual(sentinel.read_bytes(), b'unchanged')
            self.assertEqual(sorted(p.name for p in target.iterdir()), ['keep.txt'])
            self.assertTrue(link.is_symlink())
            self.assertFalse((root / 'operations.jsonl').exists())

    def assert_dangling_step_log_rejected(self, stream):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); log = root / 'logs'; log.mkdir()
            target = root / ('uncreated-' + stream)
            link = log / ('step.' + stream)
            self.create_real_symlink_or_report_unavailable(link, target)
            self.assertFalse(target.exists())
            with patch.object(subject.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'tool_log_exists'):
                    subject.run_tool(['fake'], root, [], 'step')
                run.assert_not_called()
            self.assertTrue(link.is_symlink())
            self.assertFalse(target.exists())
            self.assertEqual(sorted(p.name for p in log.iterdir()), ['step.' + stream])
            self.assertFalse((root / 'operations.jsonl').exists())

    def test_tool_step_rejects_real_dangling_stdout_link(self):
        self.assert_dangling_step_log_rejected('stdout')

    def test_tool_step_rejects_real_dangling_stderr_link(self):
        self.assert_dangling_step_log_rejected('stderr')

    def test_tool_step_rejects_unsafe_label_before_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(subject.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'invalid_tool_step'):
                    subject.run_tool(['fake'], Path(tmp), [], '../outside')
                run.assert_not_called()

    def test_preexisting_journal_is_rejected_before_process_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); journal = root / 'operations.jsonl'; journal.write_bytes(b'previous')
            with patch.object(subject.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'tool_journal_exists'):
                    subject.run_tool(['fake'], root, [], 'step')
                run.assert_not_called()
            self.assertEqual(journal.read_bytes(), b'previous')
            self.assertFalse((root / 'logs').exists())

    def test_dangling_journal_name_is_rejected_before_process_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); journal = root / 'operations.jsonl'
            if os.name != 'nt':
                journal.symlink_to(root / 'missing-journal')
                with patch.object(subject.subprocess, 'run') as run:
                    with self.assertRaisesRegex(ValueError, 'tool_journal_exists'):
                        subject.run_tool(['fake'], root, [], 'step')
                    run.assert_not_called()
                self.assertTrue(journal.is_symlink())
            else:
                original_open = Path.open
                def simulate_dangling(path, *args, **kwargs):
                    if path == journal and args and args[0] == 'xb':
                        raise FileExistsError('occupied')
                    return original_open(path, *args, **kwargs)
                with patch.object(Path, 'open', simulate_dangling), patch.object(subject.subprocess, 'run') as run:
                    with self.assertRaisesRegex(ValueError, 'tool_journal_exists'):
                        subject.run_tool(['fake'], root, [], 'step')
                    run.assert_not_called()

    def test_replaced_journal_is_rejected_before_next_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); operations = []
            result = subprocess.CompletedProcess(['fake'], 0, stdout=b'first', stderr=b'')
            with patch.object(subject.subprocess, 'run', return_value=result) as run:
                subject.run_tool(['fake'], root, operations, 'first')
                journal = root / 'operations.jsonl'; original = journal.read_bytes()
                journal.rename(root / 'retained-original.jsonl')
                journal.write_bytes(original)
                with self.assertRaisesRegex(ValueError, 'tool_journal_identity_changed'):
                    subject.run_tool(['fake'], root, operations, 'second')
                self.assertEqual(run.call_count, 1)
                self.assertEqual((root / 'retained-original.jsonl').read_bytes(), original)

    def test_tool_journal_and_logs_must_match_before_publication(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); operations = []
            result = subprocess.CompletedProcess(['fake'], 0, stdout=b'known', stderr=b'')
            with patch.object(subject.subprocess, 'run', return_value=result):
                subject.run_tool(['fake'], root, operations, 'step')
            subject.verify_tool_log_integrity(root, operations)
            (root / 'logs/step.stdout').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'tool_log_hash_mismatch'):
                subject.verify_tool_log_integrity(root, operations)

    def test_workspace_roots_created_inside_and_external_link_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'project'; root.mkdir()
            outside = Path(tmp) / 'external'; outside.mkdir()
            with patch.object(subject, 'ROOT', root):
                self.assertEqual(subject.workspace_directory('build'), (root / 'build').resolve())
                original_resolve = Path.resolve
                def fake_resolve(path, *args, **kwargs):
                    if path == root / 'dist': return original_resolve(outside)
                    return original_resolve(path, *args, **kwargs)
                with patch.object(Path, 'resolve', fake_resolve):
                    with self.assertRaisesRegex(ValueError, 'dist_root_outside_workspace'):
                        subject.workspace_directory('dist')

    def test_fresh_destination_rejects_existing_and_dangling_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); dist = root / 'dist'; build = root / 'build'; dist.mkdir(); build.mkdir()
            output = dist / 'candidate.zip'; work = build / 'new-work'
            self.assertEqual(subject.fresh_output_path(output, dist.resolve(), '.zip', 'not_fresh'), output.resolve())
            self.assertEqual(subject.fresh_work_directory(work, build.resolve()), work.resolve())
            output.write_bytes(b'existing')
            with self.assertRaisesRegex(ValueError, 'not_fresh'):
                subject.fresh_output_path(output, dist.resolve(), '.zip', 'not_fresh')
            missing = dist / 'dangling.zip'; dangling_work = build / 'dangling-work'
            if os.name != 'nt':
                missing.symlink_to(dist / 'absent.zip'); dangling_work.symlink_to(build / 'absent-work')
                with self.assertRaisesRegex(ValueError, 'not_fresh'):
                    subject.fresh_output_path(missing, dist.resolve(), '.zip', 'not_fresh')
                with self.assertRaisesRegex(ValueError, 'work_dir_not_fresh'):
                    subject.fresh_work_directory(dangling_work, build.resolve())
            else:
                real_lexists = os.path.lexists
                def simulate_dangling(value):
                    return Path(value) in (missing, dangling_work) or real_lexists(value)
                with patch.object(subject.os.path, 'lexists', side_effect=simulate_dangling):
                    with self.assertRaisesRegex(ValueError, 'not_fresh'):
                        subject.fresh_output_path(missing, dist.resolve(), '.zip', 'not_fresh')
                    with self.assertRaisesRegex(ValueError, 'work_dir_not_fresh'):
                        subject.fresh_work_directory(dangling_work, build.resolve())


if __name__ == '__main__':
    unittest.main()
