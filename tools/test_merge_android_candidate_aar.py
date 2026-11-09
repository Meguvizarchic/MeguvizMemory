"""Synthetic archive contracts only; no Android runtime or device execution."""
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import merge_android_candidate_aar as subject


def archive(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w') as result:
        for name, data in entries.items(): result.writestr(name, data)
    return output.getvalue()


def candidate(change=None):
    outer = {}
    for abi, (kind, machine) in subject.ABIS.items():
        core = bytearray(24); core[:6] = b'\x7fELF' + bytes((kind, 1)); core[18:20] = machine.to_bytes(2, 'little')
        sdk = {'lib/' + name: bytes(core) + name.encode() for name in subject.LIBS}
        aar = {name: name.encode() for name in subject.COMMON}
        aar.update({'jni/' + abi + '/' + name: data for name, data in zip(subject.LIBS, sdk.values())})
        if change: change(abi, aar, sdk)
        outer['android/' + abi + '/MeguvizMemory.aar'] = archive(aar)
        outer['android/' + abi + '/MeguvizMemory-sdk.zip'] = archive(sdk)
    return archive(outer)


class MergeTests(unittest.TestCase):
    def run_fixture(self, data, expected=None, existing=False):
        with tempfile.TemporaryDirectory(prefix='universal-aar-offline-', dir=subject.ROOT / 'build') as tmp:
            root = Path(tmp); (root / 'dist').mkdir(); source = root / 'candidate.zip'; source.write_bytes(data)
            output = root / 'dist/merged.aar'
            if existing: output.write_bytes(b'preserve-existing')
            with patch.object(subject, 'ROOT', root):
                if existing:
                    with self.assertRaisesRegex(ValueError, 'output_already_exists'):
                        subject.merge(source, hashlib.sha256(data).hexdigest(), output)
                    self.assertEqual(output.read_bytes(), b'preserve-existing')
                    return
                result = subject.merge(source, expected or hashlib.sha256(data).hexdigest(), output)
                with zipfile.ZipFile(output) as package:
                    self.assertEqual(len(package.namelist()), 11)
                    self.assertEqual({n: package.read(n) for n in package.namelist()}, subject.members(data))
                self.assertFalse(list(output.parent.glob('*.pending')))
                self.assertEqual(result['apk_built_from_universal_aar'], 'NOT_RUN')

    def test_actual_member_inheritance(self): self.run_fixture(candidate())
    def test_no_overwrite(self): self.run_fixture(candidate(), existing=True)
    def test_candidate_identity(self):
        with self.assertRaisesRegex(ValueError, 'candidate_identity_mismatch'):
            self.run_fixture(candidate(), expected='0' * 64)
    def test_common_member_mismatch(self):
        def change(abi, aar, sdk):
            if abi == 'x86_64': aar['classes.jar'] = b'different fixture'
        with self.assertRaisesRegex(ValueError, 'non_jni_members_differ'): self.run_fixture(candidate(change))
    def test_sdk_member_mismatch(self):
        def change(abi, aar, sdk):
            if abi == 'arm64-v8a': sdk['lib/libmgmem.so'] += b'drift'
        with self.assertRaisesRegex(ValueError, 'sdk_aar_native_mismatch'): self.run_fixture(candidate(change))
    def test_extra_native_member(self):
        def change(abi, aar, sdk): aar['jni/' + abi + '/extra.so'] = b'extra'
        with self.assertRaisesRegex(ValueError, 'aar_member_closure_mismatch'): self.run_fixture(candidate(change))


if __name__ == '__main__': unittest.main()
