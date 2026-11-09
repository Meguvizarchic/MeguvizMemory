"""Offline checks for the isolated managed diagnostic builder; no ADB."""
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from unittest.mock import patch

import build_android_managed_probe as subject


class ManagedProbeBuilderTests(unittest.TestCase):
    def test_copy_invocations_and_private_diagnostic_key(self):
        descriptor = 'L' + subject.INSTRUMENT.replace('.', '/') + ';'
        dump = "Class #0 -\nClass descriptor  : '" + descriptor + "'\n" + (
            'invoke-virtual {v1}, Lowner;.copyTo:(Lowner;JJI)V\n' * 3)
        subject.validate_copy_calls(dump)
        with self.assertRaisesRegex(ValueError, 'copy_to_instrumentation_calls_missing'):
            subject.validate_copy_calls(dump.replace('invoke-virtual', 'no-invocation'))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); private = root / 'build'; private.mkdir()
            key = private / 'diagnostic.keystore'; key.write_bytes(b'private diagnostic fixture')
            self.assertEqual(subject.diagnostic_key_input(key, private), key.resolve())
            self.assertIsNone(subject.diagnostic_key_input(None, private))
            outside = root / 'outside.keystore'; outside.write_bytes(b'fixture')
            with self.assertRaisesRegex(ValueError, 'diagnostic_key_must_remain_private_build_file'):
                subject.diagnostic_key_input(outside, private)

    def test_javap_descriptors_are_required_and_unique(self):
        text = ('  private static native long nativeCreate(boolean, long);\n'
                '    descriptor: (ZJ)J\n'
                '  private static native int nativeClose(long);\n'
                '    descriptor: (J)I\n')
        self.assertEqual(subject.native_descriptors(text), {'nativeCreate': '(ZJ)J', 'nativeClose': '(J)I'})
        with self.assertRaisesRegex(ValueError, 'javap_native_declarations_invalid'):
            subject.native_descriptors(text + text)
        with self.assertRaisesRegex(ValueError, 'javap_native_declarations_invalid'):
            subject.native_descriptors('no native methods')

    def test_dex_checks_class_method_and_signature(self):
        owner = 'io.github.example.Probe'
        block = ("Class #0            -\n"
                 "  Class descriptor  : 'Lio/github/example/Probe;'\n"
                 "    #0              : (in Lio/github/example/Probe;)\n"
                 "      name          : 'nativeCall'\n"
                 "      type          : '(J)I'\n"
                 "      access        : 0x010a (PRIVATE STATIC NATIVE)\n")
        subject.validate_dex(block, {owner: {'nativeCall': '(J)I'}})
        with self.assertRaisesRegex(ValueError, 'dex_native_signature_missing'):
            subject.validate_dex(block, {owner: {'nativeCall': '(J)J'}})
        with self.assertRaisesRegex(ValueError, 'dex_native_class_missing'):
            subject.validate_dex(block, {'io.github.example.Other': {'nativeCall': '(J)I'}})
        with self.assertRaisesRegex(ValueError, 'shrink_witness_remains'):
            subject.validate_dex(block + "Class descriptor  : 'L" + subject.WITNESS.replace('.', '/') + ";'\n",
                                 {owner: {'nativeCall': '(J)I'}})

    def test_archive_member_rejects_missing_duplicate_and_oversize(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w') as archive:
            archive.writestr('classes.jar', b'class')
            archive.writestr('lib/core.so', b'core')
        with zipfile.ZipFile(io.BytesIO(output.getvalue())) as archive:
            self.assertEqual(subject.read_unique(archive, 'classes.jar'), b'class')
            with self.assertRaisesRegex(ValueError, 'archive_member_missing_or_duplicate'):
                subject.read_unique(archive, 'missing')
            with patch.object(subject, 'MAX_MEMBER', 2):
                with self.assertRaisesRegex(ValueError, 'archive_member_invalid'):
                    subject.read_unique(archive, 'classes.jar')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'duplicate.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('same', b'a')
                with self.assertWarns(UserWarning):
                    archive.writestr('same', b'b')
            with zipfile.ZipFile(path) as archive:
                with self.assertRaisesRegex(ValueError, 'archive_member_missing_or_duplicate'):
                    subject.read_unique(archive, 'same')

    def test_bounded_input_and_duplicate_evidence_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            candidate = root / 'candidate.zip'; candidate.write_bytes(b'1234')
            evidence = root / 'evidence.json'; evidence.write_text('{"state":"a","state":"b"}', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'input_file_limit'):
                subject.bounded_file(candidate, 3)
            self.assertEqual(subject.bounded_file(candidate, 4), b'1234')
            with self.assertRaisesRegex(ValueError, 'duplicate_json_key'):
                subject.candidate_inputs(candidate, evidence, 'arm64-v8a')

    def test_candidate_source_identity_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); candidate = root / 'candidate.zip'; evidence = root / 'evidence.json'
            aar, sdk = b'aar', b'sdk'
            with zipfile.ZipFile(candidate, 'w') as archive:
                archive.writestr('android/arm64-v8a/MeguvizMemory.aar', aar)
                archive.writestr('android/arm64-v8a/MeguvizMemory-sdk.zip', sdk)
                archive.writestr('SOURCE.json', json.dumps({'source_lock_sha256': 'a' * 64,
                    'local_source_set_sha256': 'b' * 64, 'local_source_count': 2}))
            record = {'state': 'PASS_SCOPED_ENGINEERING_CANDIDATES', 'source_commit': 'c' * 40,
                      'source_lock_sha256': 'd' * 64, 'source_set_sha256': 'b' * 64,
                      'source_record_count': 2,
                      'android': {'sha256': subject.digest(candidate.read_bytes()), 'profiles': {
                          'arm64-v8a': {'aar_sha256': subject.digest(aar), 'sdk_sha256': subject.digest(sdk)}}}}
            evidence.write_text(json.dumps(record), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'candidate_source_identity_mismatch'):
                subject.candidate_inputs(candidate, evidence, 'arm64-v8a')


if __name__ == '__main__':
    unittest.main()
