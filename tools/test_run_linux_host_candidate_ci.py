#!/usr/bin/env python3
"""Offline safety contracts for the isolated Linux CI runner; no build/device."""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch
import zipfile

import build_android_candidate as common
import build_host_candidate as host
import run_linux_host_candidate_ci as subject


class RunnerContracts(unittest.TestCase):
    def test_strip_detail_consumes_real_failed_tool_journal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'raw-sdk/lib').mkdir(parents=True)
            (root / 'raw-sdk/lib/libmgmem_pressure.a').write_bytes(b'!<arch>\nfixture-data')
            llvm = root / 'llvm'
            llvm.mkdir()
            strip = llvm / 'llvm-strip'
            strip.write_bytes(b'tool fixture')
            identities = {'llvm-strip': common.file_identity(strip)}
            operations = []
            with self.assertRaisesRegex(ValueError, '^tool_failed_pressure-strip$'):
                common.run_tool([sys.executable, '-c',
                    "import sys; sys.stderr.write('llvm-strip: error: no such file or directory\\n'); sys.exit(1)"],
                    root, operations, 'pressure-strip')
            (root / 'STATUS.json').write_bytes(common.json_bytes({'state': 'FAILED',
                'reason': 'tool_failed_pressure-strip', 'output_written': False}))
            detail = subject.collect_pressure_strip_detail(root, {'llvm': llvm}, identities)
            self.assertEqual(detail['journal_status'], 'VERIFIED')
            self.assertEqual(detail['category'], 'MISSING_FILE')
            self.assertEqual(detail['tool_exit_code'], 1)
            self.assertEqual(detail['archive_format'], 'AR')
            self.assertEqual(detail['llvm_identity'], 'MATCH_PREBUILD')

    def strip_fixture(self, root, stderr, code=1):
        (root / 'logs').mkdir(parents=True)
        (root / 'raw-sdk/lib').mkdir(parents=True)
        (root / 'logs/pressure-strip.stderr').write_bytes(stderr)
        archive = b'!<arch>\nfixture-data'
        (root / 'raw-sdk/lib/libmgmem_pressure.a').write_bytes(archive)
        (root / 'STATUS.json').write_bytes(common.json_bytes({'state': 'FAILED',
            'reason': 'tool_failed_pressure-strip', 'output_written': False}))
        row = {'step': 'pressure-strip', 'state': 'EXITED', 'exit_code': code,
               'stderr_sha256': subject.digest_bytes(stderr)}
        (root / 'operations.jsonl').write_bytes(common.canonical(row) + b'\n')
        llvm = root / 'llvm'; llvm.mkdir(); tool = llvm / 'llvm-strip'; tool.write_bytes(b'tool fixture')
        return {'llvm': llvm}, {'llvm-strip': common.file_identity(tool)}, row, archive

    def test_strip_patterns_are_fixed_and_ambiguous_is_unknown(self):
        cases = (
            (b"llvm-strip: error: 'PRIVATE_PATH': no such file or directory\n", 'MISSING_FILE'),
            (b"llvm-strip: error: 'PRIVATE_PATH': permission denied\n", 'PERMISSION_DENIED'),
            (b"llvm-strip: error: no space left on device\n", 'NO_SPACE'),
            (b"llvm-strip: error: unknown argument '--PRIVATE_TOKEN'\n", 'UNKNOWN_OPTION'),
            (b"llvm-strip: error: file format not recognized\n", 'FORMAT_UNRECOGNIZED'),
            (b"llvm-strip: error: unsupported archive format\n", 'UNSUPPORTED_FORMAT_OR_ARCH'),
            (b"llvm-strip: error: truncated archive\n", 'INVALID_OR_TRUNCATED_ARCHIVE'),
            (b"llvm-strip: error: PRIVATE_TOKEN unexpected\n", 'UNKNOWN'),
            (b"llvm-strip: error: permission denied; no space left on device\n", 'UNKNOWN'),
            (b"warning: permission denied\n", 'UNKNOWN'),
            (b"warning: error: permission denied\n", 'UNKNOWN'),
            (b"unrelated: error: no space left on device\n", 'UNKNOWN'),
            (b'error: permission denied\0private', 'UNKNOWN'),
            (b'x' * (subject.MAX_STRIP_STDERR + 1), 'UNKNOWN'),
        )
        for data, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(subject.classify_strip_stderr(data), expected)

    def test_strip_detail_is_bound_to_private_journal_without_text_leak(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stderr = b"llvm-strip: error: 'PRIVATE_TOKEN_123': no such file or directory\n"
            tools, identities, _row, archive = self.strip_fixture(root, stderr)
            detail = subject.collect_pressure_strip_detail(root, tools, identities)
            self.assertEqual(detail['journal_status'], 'VERIFIED')
            self.assertEqual(detail['stderr_status'], 'OK')
            self.assertEqual(detail['stderr_bytes'], len(stderr))
            self.assertEqual(detail['stderr_sha256'], subject.digest_bytes(stderr))
            self.assertEqual(detail['tool_outcome'], 'EXITED_NONZERO')
            self.assertEqual(detail['tool_exit_code'], 1)
            self.assertEqual(detail['category'], 'MISSING_FILE')
            self.assertEqual(detail['archive_format'], 'AR')
            self.assertEqual(detail['archive_sha256'], subject.digest_bytes(archive))
            self.assertEqual(detail['llvm_identity'], 'MATCH_PREBUILD')
            self.assertNotIn('PRIVATE_TOKEN_123', json.dumps(detail))
            self.assertNotIn(str(root), json.dumps(detail))
            self.assertIn('NOT_ROOT_CAUSE', detail['classification_scope'])

    def test_strip_detail_rejects_missing_oversized_and_mismatched_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); stderr = b'llvm-strip: error: permission denied\n'
            tools, identities, row, _archive = self.strip_fixture(root, stderr)
            row['stderr_sha256'] = '0' * 64
            (root / 'operations.jsonl').write_bytes(common.canonical(row) + b'\n')
            detail = subject.collect_pressure_strip_detail(root, tools, identities)
            self.assertEqual(detail['journal_status'], 'INVALID')
            self.assertEqual(detail['category'], 'UNKNOWN')
            self.assertIsNone(detail['tool_exit_code'])
            (root / 'logs/pressure-strip.stderr').write_bytes(b'x' * (subject.MAX_STRIP_STDERR + 1))
            detail = subject.collect_pressure_strip_detail(root, tools, identities)
            self.assertEqual(detail['stderr_status'], 'OVERSIZE')
            self.assertIsNone(detail['stderr_bytes'])
            self.assertIsNone(detail['stderr_sha256'])
            self.assertEqual(detail['category'], 'UNKNOWN')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); tools = {'llvm': root}; identities = {}
            detail = subject.collect_pressure_strip_detail(root, tools, identities)
            self.assertEqual(detail['journal_status'], 'MISSING')
            self.assertEqual(detail['stderr_status'], 'MISSING')
            self.assertEqual(detail['category'], 'UNKNOWN')
            self.assertEqual(detail['archive_status'], 'MISSING')

    def test_strip_detail_rejects_duplicate_or_failed_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); stderr = b'llvm-strip: error: no space left on device\n'
            tools, identities, row, _archive = self.strip_fixture(root, stderr)
            (root / 'operations.jsonl').write_bytes(common.canonical(row) + b'\n' + common.canonical(row) + b'\n')
            detail = subject.collect_pressure_strip_detail(root, tools, identities)
            self.assertEqual(detail['journal_status'], 'INVALID')
            self.assertEqual(detail['category'], 'UNKNOWN')
            (root / 'operations.jsonl').write_bytes(common.canonical(row) + b'\n')
            (root / 'STATUS.json').write_bytes(common.json_bytes({'state': 'FAILED',
                'reason': 'tool_failed_pressure-strip', 'output_written': True}))
            detail = subject.collect_pressure_strip_detail(root, tools, identities)
            self.assertEqual(detail['journal_status'], 'INVALID')
            self.assertEqual(detail['category'], 'UNKNOWN')

    def test_strip_diagnostic_reads_only_for_exact_failure(self):
        with patch.object(subject, 'collect_pressure_strip_detail', return_value={'category': 'UNKNOWN'}) as collect:
            others = (
                subject.ChildStageFailure('source_lock', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2),
                subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'tool_failed_core-build', 2),
                subject.ChildStageFailure('host_builder', 'TIMEOUT', 'anything', None),
            )
            for error in others:
                subject.attach_pressure_strip_detail(error, Path('.'), {}, {})
            collect.assert_not_called()
            exact = subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2)
            subject.attach_pressure_strip_detail(exact, Path('.'), {}, {})
            collect.assert_called_once()
            self.assertIn('pressure_strip', exact.public_record())

    def test_strip_diagnostic_errors_cannot_mask_original_failure(self):
        for failure in (ValueError('PRIVATE_TOKEN_123'), RecursionError('PRIVATE_TOKEN_123'),
                        MemoryError('PRIVATE_TOKEN_123')):
            with self.subTest(kind=type(failure).__name__), \
                 patch.object(subject, 'collect_pressure_strip_detail', side_effect=failure):
                error = subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2)
                subject.attach_pressure_strip_detail(error, Path('.'), {}, {})
                record = subject.public_failure(error)
                self.assertEqual((record['stage'], record['outcome'], record['reason'], record['exit_code']),
                                 ('host_builder', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2))
                self.assertEqual(record['pressure_strip']['category'], 'UNKNOWN')
                self.assertEqual(record['pressure_strip']['journal_status'], 'UNKNOWN')
                self.assertNotIn('PRIVATE_TOKEN_123', json.dumps(record))

    def test_deep_private_journal_cannot_replace_original_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); stderr = b'llvm-strip: error: permission denied\n'
            tools, identities, _row, _archive = self.strip_fixture(root, stderr)
            (root / 'operations.jsonl').write_bytes(b'[' * 4000 + b']' * 4000 + b'\n')
            error = subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2)
            subject.attach_pressure_strip_detail(error, root, tools, identities)
            record = subject.public_failure(error)
            self.assertEqual(record['reason'], 'tool_failed_pressure-strip')
            self.assertEqual(record['pressure_strip']['category'], 'UNKNOWN')

    def test_strip_detail_is_only_exposed_for_exact_failure_and_is_allowlisted(self):
        dirty = {'journal_status': 'VERIFIED', 'stderr_status': 'OK', 'stderr_bytes': 3,
                 'stderr_sha256': 'a' * 64, 'tool_outcome': 'EXITED_NONZERO', 'tool_exit_code': 1,
                 'category': 'PRIVATE_TOKEN_123', 'archive_status': 'OK', 'archive_format': 'SECRET',
                 'archive_bytes': 9999999999, 'archive_sha256': 'PRIVATE_TOKEN_123',
                 'llvm_identity': 'PRIVATE_TOKEN_123', 'llvm_sha256': 'PRIVATE_TOKEN_123',
                 'path': 'PRIVATE_TOKEN_123'}
        error = subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2)
        error.pressure_strip_detail = dirty
        record = error.public_record()
        self.assertEqual(record['pressure_strip']['category'], 'UNKNOWN')
        self.assertEqual(record['pressure_strip']['archive_format'], 'UNKNOWN')
        self.assertNotIn('PRIVATE_TOKEN_123', json.dumps(record))
        other = subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'tool_failed_core-build', 2)
        other.pressure_strip_detail = dirty
        self.assertNotIn('pressure_strip', other.public_record())
        source = subject.ChildStageFailure('source_lock', 'EXITED_NONZERO', 'tool_failed_pressure-strip', 2)
        source.pressure_strip_detail = dirty
        self.assertNotIn('pressure_strip', source.public_record())

    def test_builder_diagnostics_only_allow_fixed_codes(self):
        cases = (
            (b'{"state":"FAILED","reason":"host_jni_core_dependency"}', 'host_jni_core_dependency'),
            (b'{"state":"PREFLIGHT_FAILED","reason":"tool_failed_core-configure"}', 'tool_failed_core-configure'),
            (b'{"state":"FAILED","reason":"tool_failed_private_TOKEN"}', 'UNKNOWN_BUILDER_REASON'),
            (b'{"state":"FAILED","reason":"SECRET_TOKEN_123456"}', 'UNKNOWN_BUILDER_REASON'),
            (b'{"state":"FAILED","reason":"host_jni_core_dependency/private"}', 'UNKNOWN_BUILDER_REASON'),
            (b'{"state":"OK","reason":"host_jni_core_dependency"}', 'UNKNOWN_BUILDER_REASON'),
            (b'{"state":"FAILED","reason":"x","reason":"host_jni_core_dependency"}', 'UNKNOWN_BUILDER_REASON'),
            (b'not-json SECRET_TOKEN_123456', 'UNKNOWN_BUILDER_REASON'),
            (b'x' * 4097, 'UNKNOWN_BUILDER_REASON'),
        )
        for payload, expected in cases:
            with self.subTest(payload=payload[:60]):
                self.assertEqual(subject.safe_builder_reason(payload), expected)
        result = subject.ChildStageFailure('host_builder', 'EXITED_NONZERO', 'SECRET_TOKEN_123456', 1).public_record()
        self.assertEqual(result['reason'], 'UNKNOWN_BUILDER_REASON')
        self.assertNotIn('SECRET_TOKEN_123456', json.dumps(result))

    def test_builder_failure_preserves_private_output_but_exposes_no_extra_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stderr = b'{"state":"FAILED","reason":"tool_failed_core-configure","extra":"SECRET_TOKEN_123456"}'
            result = subprocess.CompletedProcess(['fake'], 1, stdout=b'private stdout', stderr=stderr)
            with patch.object(subject.subprocess, 'run', return_value=result):
                with self.assertRaises(subject.ChildStageFailure) as caught:
                    subject.execute(['fake'], {}, root / 'stdout', root / 'stderr', 1, 'host_builder')
            self.assertEqual((root / 'stderr').read_bytes(), stderr)
            self.assertEqual(caught.exception.public_record(), {'state': 'FAILED_NO_CI_ARTIFACT_UPLOAD',
                'stage': 'host_builder', 'outcome': 'EXITED_NONZERO', 'reason': 'tool_failed_core-configure', 'exit_code': 1})
            self.assertNotIn('SECRET_TOKEN_123456', json.dumps(caught.exception.public_record()))
            self.assertFalse((root / 'export').exists())

    def test_source_lock_failure_never_exposes_child_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = subprocess.CompletedProcess(['fake'], 2, stdout=b'',
                stderr=b'{"state":"FAILED","reason":"host_jni_core_dependency","secret":"hidden"}')
            with patch.object(subject.subprocess, 'run', return_value=result):
                with self.assertRaises(subject.ChildStageFailure) as caught:
                    subject.execute(['fake'], {}, root / 'stdout', root / 'stderr', 1, 'source_lock')
            self.assertEqual(caught.exception.public_record()['reason'], 'SOURCE_LOCK_FAILED')
            self.assertEqual(caught.exception.public_record()['stage'], 'source_lock')
            self.assertNotIn('hidden', json.dumps(caught.exception.public_record()))

    def test_existing_private_log_refuses_child_and_preserves_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); existing = root / 'stderr'; existing.write_bytes(b'preserve')
            with patch.object(subject.subprocess, 'run') as child:
                with self.assertRaises(subject.ChildStageFailure) as caught:
                    subject.execute(['fake'], {}, root / 'stdout', existing, 1, 'host_builder')
            child.assert_not_called()
            self.assertEqual(existing.read_bytes(), b'preserve')
            self.assertEqual(caught.exception.public_record()['outcome'], 'LOG_WRITE_FAILED')
            self.assertFalse((root / 'export').exists())

    def test_child_timeout_and_launch_failure_have_fixed_stage_diagnostics(self):
        for stage in ('source_lock', 'host_builder'):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                error = subprocess.TimeoutExpired('fake', 1, output=b'partial private', stderr=b'SECRET_TOKEN_123456')
                with patch.object(subject.subprocess, 'run', side_effect=error):
                    with self.assertRaises(subject.ChildStageFailure) as caught:
                        subject.execute(['fake'], {}, root / 'timeout.stdout', root / 'timeout.stderr', 1, stage)
                self.assertEqual(caught.exception.public_record()['stage'], stage)
                self.assertEqual(caught.exception.public_record()['outcome'], 'TIMEOUT')
                self.assertEqual(caught.exception.public_record()['reason'], 'CHILD_TIMEOUT')
                self.assertEqual((root / 'timeout.stdout').read_bytes(), b'partial private')
                self.assertNotIn('SECRET_TOKEN_123456', json.dumps(caught.exception.public_record()))
                with patch.object(subject.subprocess, 'run', side_effect=OSError('SECRET_TOKEN_123456')):
                    with self.assertRaises(subject.ChildStageFailure) as launch:
                        subject.execute(['fake'], {}, root / 'launch.stdout', root / 'launch.stderr', 1, stage)
                self.assertEqual(launch.exception.public_record()['outcome'], 'LAUNCH_ERROR')
                self.assertEqual(launch.exception.public_record()['reason'], 'CHILD_LAUNCH_FAILED')
                self.assertNotIn('SECRET_TOKEN_123456', json.dumps(launch.exception.public_record()))

    def test_unknown_runner_errors_are_not_echoed(self):
        self.assertEqual(subject.public_failure(ValueError('checkout_not_clean'))['reason'], 'checkout_not_clean')
        self.assertEqual(subject.public_failure(ValueError('SECRET_TOKEN_123456'))['reason'], 'UNKNOWN_RUNNER_REASON')
        with patch.object(subject, 'run', side_effect=ValueError('SECRET_TOKEN_123456')), \
             patch.object(sys, 'argv', ['runner', '--commit', 'a' * 40]), \
             redirect_stderr(io.StringIO()) as output:
            self.assertEqual(subject.main(), 1)
        self.assertEqual(json.loads(output.getvalue())['reason'], 'UNKNOWN_RUNNER_REASON')
        self.assertNotIn('SECRET_TOKEN_123456', output.getvalue())

    def test_commit_is_exact_lowercase_hex(self):
        self.assertEqual(subject.valid_commit('a' * 40), 'a' * 40)
        for value in ('a' * 39, 'A' * 40, 'a' * 40 + '\n', 'f' * 40 + ';id', '', None):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'invalid_commit_sha'):
                subject.valid_commit(value)

    def test_evidence_json_rejects_duplicate_or_nonfinite_values(self):
        self.assertEqual(subject.strict_json(b'{"ok":1}'), {'ok': 1})
        with self.assertRaisesRegex(ValueError, 'duplicate_json_key'):
            subject.strict_json(b'{"state":"PASS","state":"FAIL"}')
        with self.assertRaisesRegex(ValueError, 'invalid_json_constant'):
            subject.strict_json(b'{"number":NaN}')

    def test_workflow_ref_and_checkout_must_be_the_same_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); sha = 'a' * 40
            env = {'GITHUB_ACTIONS': 'true', 'RUNNER_OS': 'Linux', 'RUNNER_ARCH': 'X64',
                   'RUNNER_ENVIRONMENT': 'github-hosted', 'GITHUB_EVENT_NAME': 'workflow_dispatch',
                   'GITHUB_SHA': sha, 'GITHUB_WORKSPACE': str(root)}
            subject.verify_ci_identity(sha, env, root)
            for change, reason in (({'GITHUB_SHA': 'b' * 40}, 'workflow_commit_mismatch'),
                                   ({'GITHUB_EVENT_NAME': 'push'}, 'github_hosted_linux_required'),
                                   ({'GITHUB_WORKSPACE': str(root / 'different')}, 'github_workspace_mismatch')):
                altered = dict(env); altered.update(change)
                # resolve(strict=True) can reject a nonexistent workspace even earlier.
                with self.subTest(change=change), self.assertRaises((ValueError, OSError)):
                    subject.verify_ci_identity(sha, altered, root)

    def test_source_paths_reject_escapes_and_controls(self):
        self.assertEqual(subject.safe_name('src/example.cpp').as_posix(), 'src/example.cpp')
        for value in ('../escape', '/absolute', 'C:/outside', 'a\\b', 'a//b', 'a/./b', 'a\nb', ''):
            with self.subTest(value=value), self.assertRaises(ValueError):
                subject.safe_name(value)

    def test_git_tree_rejects_symlink_submodule_and_control_names(self):
        good = (b'100644 blob ' + b'a' * 40 + b' 1\tCMakeLists.txt\0' +
                b'100644 blob ' + b'b' * 40 + b' 1\ttools/run_linux_host_candidate_ci.py\0')
        with patch.object(subject, 'git', return_value=good):
            self.assertEqual(len(subject.git_tree(Path('.'), 'a' * 40)), 2)
        for bad in (b'120000 blob ' + b'c' * 40 + b' 1\tlink\0',
                    b'160000 commit ' + b'd' * 40 + b' -\tsubmodule\0',
                    b'100644 blob ' + b'e' * 40 + b' 1\tbad\nname\0',
                    b'100644 blob ' + b'f' * 40 + b' 999999999\ttoo-large\0'):
            with self.subTest(bad=bad), patch.object(subject, 'git', return_value=good + bad), self.assertRaises(ValueError):
                subject.git_tree(Path('.'), 'a' * 40)

    def test_archive_rejects_extra_duplicate_symlink_and_oversized(self):
        expected = {'a.txt': {'mode': '100644', 'oid': 'a' * 40, 'bytes': 1}}
        item = zipfile.ZipInfo('a.txt'); item.file_size = 1
        self.assertEqual(set(subject.compare_archive_names([item], expected)), {'a.txt'})
        extra = zipfile.ZipInfo('b.txt'); extra.file_size = 1
        with self.assertRaisesRegex(ValueError, 'source_archive_set_mismatch'):
            subject.compare_archive_names([item, extra], expected)
        duplicate = zipfile.ZipInfo('A.TXT'); duplicate.file_size = 1
        with self.assertRaisesRegex(ValueError, 'duplicate_archive_name'):
            subject.compare_archive_names([item, duplicate], expected)
        link = zipfile.ZipInfo('a.txt'); link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaisesRegex(ValueError, 'source_archive_symlink'):
            subject.compare_archive_names([link], expected)
        large = zipfile.ZipInfo('a.txt'); large.file_size = subject.MAX_SOURCE_FILE + 1
        with self.assertRaisesRegex(ValueError, 'source_file_size_limit'):
            subject.compare_archive_names([large], expected)

    def test_real_git_archive_is_bound_to_exact_blob_bytes(self):
        git = shutil.which('git')
        if not git:
            self.fail('git is required for the source archive contract')
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp); root = parent / 'repo'; root.mkdir()
            (root / 'tools').mkdir(); (root / '.gitattributes').write_bytes(b'* -text\n')
            (root / 'CMakeLists.txt').write_bytes(b'project(sample)\n')
            (root / 'tools/run_linux_host_candidate_ci.py').write_bytes(b'print("fixture")\n')
            subprocess.run([git, 'init', '-q', str(root)], check=True, capture_output=True)
            prefix = [git, '-c', 'safe.directory=' + root.as_posix(), '-C', str(root)]
            subprocess.run(prefix + ['add', '--', '.gitattributes', 'CMakeLists.txt', 'tools/run_linux_host_candidate_ci.py'],
                           check=True, capture_output=True)
            subprocess.run(prefix + ['-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                           '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture'], check=True, capture_output=True)
            commit = subprocess.check_output(prefix + ['rev-parse', 'HEAD']).decode('ascii').strip()
            subject.verify_checkout(root.resolve(), commit)
            work = parent / 'work'; work.mkdir()
            archive, snapshot, records = subject.export_snapshot(root.resolve(), commit, work)
            self.assertTrue(archive.is_file())
            self.assertEqual((snapshot / 'CMakeLists.txt').read_bytes(), b'project(sample)\n')
            self.assertEqual(len(records), 3)
            (snapshot / 'CMakeLists.txt').write_bytes(b'project(changed)\n')
            with self.assertRaisesRegex(ValueError, 'snapshot_changed'):
                subject.verify_snapshot(snapshot, records)

    def test_real_git_export_substitution_is_rejected(self):
        git = shutil.which('git')
        if not git:
            self.fail('git is required for the source archive contract')
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp); root = parent / 'repo'; root.mkdir(); (root / 'tools').mkdir()
            (root / '.gitattributes').write_bytes(b'* -text\n*.txt export-subst\n')
            (root / 'CMakeLists.txt').write_bytes(b'project(sample)\n')
            (root / 'identity.txt').write_bytes(b'$Format:%H$\n')
            (root / 'tools/run_linux_host_candidate_ci.py').write_bytes(b'print("fixture")\n')
            subprocess.run([git, 'init', '-q', str(root)], check=True, capture_output=True)
            prefix = [git, '-c', 'safe.directory=' + root.as_posix(), '-C', str(root)]
            subprocess.run(prefix + ['add', '--', '.gitattributes', 'CMakeLists.txt', 'identity.txt',
                                      'tools/run_linux_host_candidate_ci.py'], check=True, capture_output=True)
            subprocess.run(prefix + ['-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                           '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture'], check=True, capture_output=True)
            commit = subprocess.check_output(prefix + ['rev-parse', 'HEAD']).decode('ascii').strip()
            work = parent / 'work'; work.mkdir()
            with self.assertRaisesRegex(ValueError, 'source_git_size_mismatch|source_archive_size_mismatch|source_archive_blob_mismatch'):
                subject.export_snapshot(root.resolve(), commit, work)
            self.assertFalse((work / 'export').exists())

    def test_snapshot_drift_and_unexpected_file_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); path = root / 'src/core.cpp'; path.parent.mkdir(); path.write_bytes(b'one')
            records = {'src/core.cpp': {'bytes': 3, 'sha256': subject.digest_bytes(b'one'), 'mode': '100644'}}
            subject.verify_snapshot(root, records)
            path.write_bytes(b'two')
            with self.assertRaisesRegex(ValueError, 'snapshot_changed'):
                subject.verify_snapshot(root, records)
            path.write_bytes(b'one'); (root / 'extra').write_bytes(b'not allowed')
            with self.assertRaisesRegex(ValueError, 'snapshot_set_changed'):
                subject.verify_snapshot(root, records)

    def test_ci_child_environment_does_not_forward_injection(self):
        tools = {name: Path('/test/' + name) for name in ('cmake', 'ninja', 'gcc', 'g++')}
        tools.update(jdk=Path('/jdk'), llvm=Path('/llvm'))
        original = {'HOME': '/existing-home', 'LD_PRELOAD': 'bad', 'JAVA_TOOL_OPTIONS': 'bad',
                    'PYTHONPATH': 'bad', 'GITHUB_TOKEN': 'secret', 'LDFLAGS': 'bad'}
        env = subject.child_environment(tools, original)
        self.assertEqual(env['HOME'], original['HOME'])
        for name in ('LD_PRELOAD', 'JAVA_TOOL_OPTIONS', 'PYTHONPATH', 'GITHUB_TOKEN', 'LDFLAGS'):
            self.assertNotIn(name, env)

    def test_missing_toolchain_is_not_success(self):
        with patch.object(subject.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'required_tool_missing_cmake'):
                subject.discover_tools({'PATH': ''})

    def test_selected_tool_identities_must_match_report_and_stay_fixed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ('cmake', 'ninja', 'gcc', 'g++', 'git', 'python', 'java', 'javac', *subject.LLVM_TOOLS)
            files = {}
            for name in names:
                path = root / name; path.write_bytes(('tool-' + name).encode()); files[name] = path
            identities = {name: common.file_identity(path) for name, path in files.items()}
            host_names = ('cmake', 'ninja', 'java', 'javac', *subject.LLVM_TOOLS)
            report = {'toolchain': {name: identities[name] for name in host_names}, 'compiler': identities['g++']}
            subject.verify_tool_identities(files, identities, report)
            files['llvm-nm'].write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'selected_tool_changed'):
                subject.verify_tool_identities(files, identities, report)
            files['llvm-nm'].write_bytes(b'tool-llvm-nm')
            report['compiler'] = identities['gcc']
            with self.assertRaisesRegex(ValueError, 'candidate_toolchain_mismatch'):
                subject.verify_tool_identities(files, identities, report)

    def make_lock_and_source(self):
        entries = [{'root': 'MeguvizMemory', 'path': 'include/mgmem/example.h',
                    'bytes': 6, 'sha256': subject.digest_bytes(b'header')},
                   {'root': 'MeguvizMemory', 'path': 'third_party_notices/llvm-NOTICE.txt',
                    'bytes': 6, 'sha256': subject.digest_bytes(b'notice')}]
        lock = {'schema_version': 1, 'scope': 'SCOPED_SOURCE_TREE_LOCK', 'file_count': 2,
                'files': entries, 'source_set_sha256': subject.digest_bytes(common.canonical(entries))}
        raw = common.json_bytes(lock)
        records = {'include/mgmem/example.h': {'bytes': 6, 'sha256': subject.digest_bytes(b'header')},
                   'third_party_notices/llvm-NOTICE.txt': {'bytes': 6, 'sha256': subject.digest_bytes(b'notice')}}
        source = {'local_source_count': 2, 'local_source_records': records,
                  'source_lock_sha256': subject.digest_bytes(raw),
                  'local_source_set_sha256': subject.digest_bytes(common.canonical(records))}
        return raw, source

    def test_candidate_source_lock_matches_entire_local_record_set(self):
        raw, source = self.make_lock_and_source()
        self.assertEqual(len(subject.validate_source_lock(raw, source)), 2)
        changed = dict(source); changed['local_source_count'] = 3
        with self.assertRaisesRegex(ValueError, 'candidate_source_lock_mismatch'):
            subject.validate_source_lock(raw, changed)
        changed = dict(source); changed['local_source_set_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'candidate_source_lock_mismatch'):
            subject.validate_source_lock(raw, changed)

    def test_source_lock_rejects_external_or_tampered_records(self):
        raw, source = self.make_lock_and_source()
        lock = json.loads(raw); lock['files'][0]['root'] = 'DexMeguviz'
        lock['source_set_sha256'] = subject.digest_bytes(common.canonical(lock['files']))
        with self.assertRaisesRegex(ValueError, 'unexpected_external_source'):
            subject.validate_source_lock(common.json_bytes(lock), source)
        lock = json.loads(raw); lock['files'][0]['bytes'] = 7
        with self.assertRaisesRegex(ValueError, 'source_lock_digest_invalid'):
            subject.validate_source_lock(common.json_bytes(lock), source)

    def journal_fixture(self, root):
        names = ('compile-managed-java', 'core-configure', 'core-build', 'core-install', 'jni-configure',
                 'jni-build', 'jni-install', 'pressure-strip', 'pressure-consumer-configure',
                 'pressure-consumer-build', 'pressure-v1-run', 'pressure-v2-run', 'consumer-probe-configure',
                 'consumer-probe-build', 'consumer-reference-configure', 'consumer-reference-build', 'consumer-verify')
        logs = root / 'logs'; logs.mkdir()
        rows = []
        for name in names:
            out, err = (name + '-out').encode(), b''
            if name == 'pressure-v1-run': out = b'{"status":"PASS","tests":3}'
            if name == 'pressure-v2-run': out = b'managed-pressure-v2 PASS (42 checks)\n'
            (logs / (name + '.stdout')).write_bytes(out); (logs / (name + '.stderr')).write_bytes(err)
            rows.append({'step': name, 'state': 'EXITED', 'exit_code': 0,
                         'stdout_sha256': subject.digest_bytes(out), 'stderr_sha256': subject.digest_bytes(err)})
        (root / 'operations.jsonl').write_bytes(b''.join(common.canonical(row) + b'\n' for row in rows))
        return rows

    def test_journal_detects_corruption_duplicates_and_failed_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); rows = self.journal_fixture(root)
            report = subject.validate_journal(root)
            self.assertEqual((report['steps'], report['streams']), (17, 34))
            target = root / 'logs/core-build.stdout'; original = target.read_bytes(); target.write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'journal_log_hash_mismatch'):
                subject.validate_journal(root)
            target.write_bytes(original)
            (root / 'operations.jsonl').write_bytes(b''.join(common.canonical(row) + b'\n' for row in rows + [rows[0]]))
            with self.assertRaisesRegex(ValueError, 'journal_duplicate_step'):
                subject.validate_journal(root)
            rows[0]['exit_code'] = 1
            (root / 'operations.jsonl').write_bytes(b''.join(common.canonical(row) + b'\n' for row in rows))
            with self.assertRaisesRegex(ValueError, 'journal_step_failed'):
                subject.validate_journal(root)

    def candidate_fixture(self, root):
        raw, source = self.make_lock_and_source()
        core = bytearray(20); core[:6] = b'\x7fELF\x02\x01'; core[16:18] = (3).to_bytes(2, 'little'); core[18:20] = (62).to_bytes(2, 'little')
        core = bytes(core); jni = core; pressure, jar = b'pressure', b'jar'
        verify = root / 'consumer-source/build/installed-consumer-verification/receipt.json'
        verify.parent.mkdir(parents=True)
        verify.write_bytes(common.json_bytes({'state': 'PASS_HOST_FUNCTIONAL_DEVELOPMENT', 'cases': [{}] * 12,
            'pressure_cases': [{}] * 12, 'negative_cases': [{}] * 10, 'pressure_retry_contract': 'PASS_JVM_ADMISSION_ONLY',
            'plain_without_sidecar': 'PASS', 'identities': {'core_sha256': subject.digest_bytes(core),
                'jni_sha256': subject.digest_bytes(jni), 'java_jar_sha256': subject.digest_bytes(jar)}}))
        consumer = {'state': 'PASS_HOST_INSTALLED_JVM_JNI_IMAGE', 'default_cases': 12,
                    'synthetic_pressure_cases': 12, 'negative_cases': 10,
                    'core_sha256': subject.digest_bytes(core), 'jni_sha256': subject.digest_bytes(jni),
                    'jar_sha256': subject.digest_bytes(jar), 'receipt_sha256': subject.digest_bytes(verify.read_bytes()),
                    'packaged_pressure': {'state': 'PASS_HOST_PACKAGED_STATIC_PRESSURE_V1_V2',
                        'core_identity': 'EACH_PROCESS_OS_MODULE_PATH', 'required_checked_external_capability': 'PASS',
                        'core_sha256': subject.digest_bytes(core), 'pressure_sha256': subject.digest_bytes(pressure)}}
        report = {'schema_version': 1, 'state': 'SCOPED_HOST_ENGINEERING_CANDIDATE',
                  'scope': 'SOURCE_BUILD_SANITIZED_INSTALLED_HOST_CONSUMER_ONLY', 'platform': 'linux-x86_64',
                  'source': source, 'android_runtime': 'NOT_RUN', 'device_execution': 'NOT_RUN',
                  'performance': 'NOT_MEASURED', 'formal_release': 'NOT_QUALIFIED',
                  'tool_logs': 'UNIQUE_EXCLUSIVE_AND_HASH_VERIFIED_IN_PRIVATE_WORK_DIRECTORY',
                  'core': {'identity': common.ident(core)}, 'jni': {'identity': common.ident(jni)},
                  'static_pressure': {'candidate': common.ident(pressure)}, 'managed_jar': common.ident(jar),
                  'consumer': consumer}
        files = {'SOURCE.json': common.json_bytes(source), 'MANIFEST.json': common.json_bytes(report),
                 'README.md': b'candidate', 'NOTICE-LLVM.txt': b'notice',
                 'sdk/include/mgmem/example.h': b'header', 'sdk/lib/libmgmem.so.1.2.2': core,
                 'sdk/lib/libmgmem_pressure_jni.so': jni, 'sdk/lib/libmgmem_pressure.a': pressure,
                 'java/managed-api.jar': jar}
        for name in ('MeguvizMemoryConfig.cmake', 'MeguvizMemoryConfigVersion.cmake',
                     'MeguvizMemoryTargets.cmake', 'MeguvizMemoryTargets-release.cmake'):
            files['sdk/lib/cmake/MeguvizMemory/' + name] = b'fixture'
        links = {'sdk/lib/libmgmem.so': 'libmgmem.so.1', 'sdk/lib/libmgmem.so.1': 'libmgmem.so.1.2.2'}
        self.journal_fixture(root)
        return raw, files, links, report

    def test_candidate_requires_matching_consumer_and_core(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); lock, files, links, report = self.candidate_fixture(root)
            payload = host.tar_bytes(files, links)
            source, actual, journal = subject.validate_candidate(payload, ['ci-sentinel-unrelated'], lock, root)
            self.assertEqual(journal['steps'], 17)
            self.assertEqual(actual['consumer']['default_cases'], 12)
            direct_link = dict(links); direct_link['sdk/lib/libmgmem.so'] = 'libmgmem.so.1.2.2'
            subject.validate_candidate(host.tar_bytes(files, direct_link), ['ci-sentinel-unrelated'], lock, root)
            wrong_link = dict(links); wrong_link['sdk/lib/libmgmem.so'] = 'libmgmem_pressure_jni.so'
            with self.assertRaisesRegex(ValueError, 'candidate_core_link_mismatch'):
                subject.validate_candidate(host.tar_bytes(files, wrong_link), ['ci-sentinel-unrelated'], lock, root)
            report['consumer']['packaged_pressure']['state'] = 'UNKNOWN'
            files['MANIFEST.json'] = common.json_bytes(report)
            with self.assertRaisesRegex(ValueError, 'candidate_consumer_mismatch'):
                subject.validate_candidate(host.tar_bytes(files, links), ['ci-sentinel-unrelated'], lock, root)

    def test_candidate_rejects_wrong_headers_or_scan_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); lock, files, links, _ = self.candidate_fixture(root)
            files['sdk/include/mgmem/example.h'] = b'changed'
            with self.assertRaisesRegex(ValueError, 'candidate_header_mismatch'):
                subject.validate_candidate(host.tar_bytes(files, links), ['ci-sentinel-unrelated'], lock, root)
            files['sdk/include/mgmem/example.h'] = b'header'
            files['extra.txt'] = b'private-needle'
            with self.assertRaisesRegex(ValueError, 'private_identifier'):
                subject.validate_candidate(host.tar_bytes(files, links), ['private-needle'], lock, root)

    def test_direct_run_rejects_wrong_host_before_output(self):
        with patch.object(subject.sys, 'platform', 'win32'):
            with self.assertRaisesRegex(ValueError, 'linux_x86_64_required'):
                subject.run('a' * 40)

    def test_failed_receipt_scan_creates_no_upload_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'private_identifier'):
                subject.write_export(root, b'candidate', b'{"leak":"private-needle"}', ['private-needle'])
            self.assertFalse((root / 'export').exists())
            subject.write_export(root, b'candidate', b'{"state":"PASS"}', ['private-needle'])
            self.assertEqual({x.name for x in (root / 'export').iterdir()},
                             {subject.EXPORT_ARCHIVE, subject.EXPORT_RECEIPT})
            with self.assertRaises(FileExistsError):
                subject.write_export(root, b'replacement', b'{"state":"PASS"}', ['private-needle'])
            self.assertEqual((root / 'export' / subject.EXPORT_ARCHIVE).read_bytes(), b'candidate')


if __name__ == '__main__':
    unittest.main()
