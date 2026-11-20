"""Offline contracts for host SDK archive safety; no device or toolchain."""
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import build_host_candidate as subject


class HostCandidateContracts(unittest.TestCase):
    def test_tool_invocation_keeps_requested_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            name = 'llvm-strip' + ('.exe' if os.name == 'nt' else '')
            requested = root / name
            requested.write_bytes(b'fixture')
            with patch.object(Path, 'resolve', side_effect=AssertionError('must retain argv zero')):
                self.assertEqual(subject.executable(root, 'llvm-strip'), requested)

    @unittest.skipIf(os.name == 'nt', 'POSIX executable symlink fixture')
    def test_multicall_symlink_keeps_mode_when_invoked(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / 'llvm-objcopy'
            target.write_text('#!/bin/sh\ncase "$0" in */llvm-strip) exit 0;; *) exit 1;; esac\n', encoding='ascii')
            target.chmod(0o755)
            (root / 'llvm-strip').symlink_to(target.name)
            requested = subject.executable(root, 'llvm-strip')
            self.assertEqual(requested.name, 'llvm-strip')
            self.assertEqual(subprocess.run([requested], check=False).returncode, 0)
            self.assertEqual(subprocess.run([requested.resolve()], check=False).returncode, 1)
            self.assertEqual(subject.common.file_identity(requested), subject.common.file_identity(target))

    def linux_fixture(self):
        files = {'sdk/lib/libmgmem.so.1.2.2': b'core', 'sdk/lib/libmgmem_pressure.a': b'pressure',
                 'sdk/lib/libmgmem_pressure_jni.so': b'jni', 'java/managed-api.jar': b'jar'}
        links = {'sdk/lib/libmgmem.so': 'libmgmem.so.1', 'sdk/lib/libmgmem.so.1': 'libmgmem.so.1.2.2'}
        return files, links

    def test_tar_roundtrip_preserves_only_relative_loader_links(self):
        files, links = self.linux_fixture()
        self.assertEqual(subject.tar_members(subject.tar_bytes(files, links)), (files, links))

    def test_linux_symlink_escape_and_cycle_are_rejected(self):
        files, links = self.linux_fixture()
        for name, target in [('sdk/lib/libmgmem.so', '../escape'), ('sdk/lib/libmgmem.so', '/absolute'),
                             ('sdk/lib/libmgmem.so', 'missing'), ('sdk/bin/link', 'anything')]:
            candidate = dict(links); candidate[name] = target
            with self.subTest(name=name, target=target), self.assertRaises(ValueError):
                subject.validate_relative_symlinks(candidate, files)
        with self.assertRaisesRegex(ValueError, 'host_symlink_cycle'):
            subject.validate_relative_symlinks({'sdk/lib/a': 'b', 'sdk/lib/b': 'a'}, files)

    def test_tar_rejects_duplicate_and_unexpected_member_types(self):
        for item_type in (tarfile.LNKTYPE, tarfile.DIRTYPE):
            output = io.BytesIO()
            with tarfile.open(fileobj=output, mode='w:gz') as archive:
                item = tarfile.TarInfo('sdk/lib/x'); item.type = item_type; item.linkname = 'target'
                archive.addfile(item)
            with self.subTest(kind=item_type), self.assertRaisesRegex(ValueError, 'invalid_host_tar_member'):
                subject.tar_members(output.getvalue())
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as archive:
            for _ in range(2):
                item = tarfile.TarInfo('sdk/x'); item.size = 1
                archive.addfile(item, io.BytesIO(b'x'))
        with self.assertRaisesRegex(ValueError, 'duplicate_host_member'):
            subject.tar_members(output.getvalue())

    def test_tar_private_scan_descends_into_files_and_zip(self):
        files, links = self.linux_fixture(); files['sdk/private.txt'] = b'example-private-marker'
        data = subject.tar_bytes(files, links)
        with self.assertRaisesRegex(ValueError, 'private_identifier'):
            subject.scan_host_archive(data, ['example-private-marker'], False)
        files['sdk/private.txt'] = b'/home/private/example'
        with self.assertRaisesRegex(ValueError, 'machine_path'):
            subject.scan_host_archive(subject.tar_bytes(files, links), ['never-match'], False)

    def test_tar_forbidden_members_are_rejected_directly_and_nested(self):
        names = ('sdk/include/mgmem/private.apk', 'sdk/include/mgmem/trace.dex',
                 'sdk/include/mgmem/private.pem', 'sdk/include/mgmem/signing.keystore',
                 'sdk/include/mgmem/.env', 'sdk/include/mgmem/.env.production')
        for name in names:
            output = io.BytesIO()
            with tarfile.open(fileobj=output, mode='w:gz') as archive:
                item = tarfile.TarInfo(name); item.size = 1
                archive.addfile(item, io.BytesIO(b'x'))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'forbidden_host_archive_member'):
                subject.tar_members(output.getvalue())
        nested = subject.common.zip_bytes({'inner/hidden.apk': b'dummy'})
        with self.assertRaisesRegex(ValueError, 'forbidden_archive_member'):
            subject.scan_host_archive(subject.tar_bytes({'nested.zip': nested}, {}), ['never-match'], False)

    def test_tar_forbidden_link_and_casefold_duplicates_are_rejected(self):
        files = {'sdk/lib/libmgmem.so.1.2.2': b'core'}
        with self.assertRaisesRegex(ValueError, 'forbidden_host_archive_member'):
            subject.tar_bytes(files, {'sdk/lib/private.key': 'libmgmem.so.1.2.2'})
        with self.assertRaisesRegex(ValueError, 'duplicate_host_member'):
            subject.tar_members(subject.tar_bytes({'sdk/Foo': b'a', 'sdk/foo': b'b'}, {}))

    def test_windows_archive_rejects_symlinks(self):
        with self.assertRaisesRegex(ValueError, 'host_windows_symlink_unsupported'):
            subject.archive_data({'sdk/a': b'a'}, {'sdk/lib/a': 'b'}, True)

    def test_windows_sdk_allowlist_rejects_extra_and_missing_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp) / 'sdk'
            names = ['bin/mgmem.dll', 'bin/mgmem_pressure_jni.dll', 'lib/mgmem.lib',
                     'lib/mgmem_pressure.lib', 'lib/mgmem_pressure_jni.lib', 'include/mgmem/mgmem.h',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryConfig.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryConfigVersion.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryTargets.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryTargets-release.cmake']
            for name in names:
                path = sdk / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'x')
            subject.sdk_files_for_host(sdk, True)
            (sdk / 'bin/unexpected.dll').write_bytes(b'x')
            with self.assertRaisesRegex(ValueError, 'host_sdk_unexpected_member'):
                subject.sdk_files_for_host(sdk, True)

    def test_installed_headers_must_exactly_match_source_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp); header = source / 'include/mgmem/mgmem.h'; header.parent.mkdir(parents=True)
            header.write_bytes(b'header')
            expected = {'sdk/include/mgmem/mgmem.h': b'header'}
            subject.validate_sdk_headers(source, expected)
            for value in ({}, {'sdk/include/mgmem/mgmem.h': b'changed'},
                          {**expected, 'sdk/include/mgmem/extra.hpp': b'extra'}):
                with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'host_sdk_header_mismatch'):
                    subject.validate_sdk_headers(source, value)

    def test_cmake_cache_must_resolve_selected_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); expected = root / 'sdk/lib/cmake/MeguvizMemory'; expected.mkdir(parents=True)
            cache = root / 'CMakeCache.txt'
            cache.write_text('MeguvizMemory_DIR:PATH=' + expected.as_posix() + '\n', encoding='utf-8')
            subject.require_cached_package_dir(cache, expected)
            cache.write_text('MeguvizMemory_DIR:PATH=' + (root / 'other').as_posix() + '\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'host_cmake_package_dir_mismatch'):
                subject.require_cached_package_dir(cache, expected)

    def test_windows_dynamic_checks_require_defined_jni_export_and_pe_abi(self):
        imports = 'Format: COFF-x86-64\nImport {\n  Name: mgmem.dll\n}\n'
        exports = 'Format: COFF-x86-64\nExport {\n  Name: Java_example\n  RVA: 0x3000\n}\n'
        with patch.object(subject, 'pe_export_directory', return_value=(0x2000, 0x2100, [(0x3000, 0x4000)])), \
             patch.object(subject.common, 'run_tool', side_effect=[imports, exports]):
            result = subject.dynamic_windows({'llvm-readobj': 'fake'}, Path('jni.dll'), Path('.'), [],
                                             'jni', ('Java_example',))
            self.assertEqual(result['needed'], ['mgmem.dll'])
        with patch.object(subject, 'pe_export_directory', return_value=(0x2000, 0x2100, [(0x3000, 0x4000)])), \
             patch.object(subject.common, 'run_tool', side_effect=[imports, exports]):
            with self.assertRaisesRegex(ValueError, 'host_pe_export_missing'):
                subject.dynamic_windows({'llvm-readobj': 'fake'}, Path('jni.dll'), Path('.'), [],
                                        'jni', ('Java_missing',))
        with patch.object(subject.common, 'run_tool', side_effect=[imports.replace('x86-64', 'i386'), exports]):
            with self.assertRaisesRegex(ValueError, 'wrong_host_pe_abi'):
                subject.dynamic_windows({'llvm-readobj': 'fake'}, Path('jni.dll'), Path('.'), [], 'jni')

    def test_pe_required_exports_reject_zero_or_forwarded_rva(self):
        for address in ('0x0', '0x2050'):
            block = 'Export {\n  Name: mgmem_domain_create_v1\n  RVA: ' + address + '\n}\n'
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, 'host_pe_forwarded_or_empty_export'):
                subject.validate_pe_exports(block, (0x2000, 0x2100, [(0x3000, 0x4000)]), {'mgmem_domain_create_v1'})
        for address in ('0x5000', '0xFFFFFFF0'):
            block = 'Export {\n  Name: mgmem_domain_create_v1\n  RVA: ' + address + '\n}\n'
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, 'host_pe_export_not_executable'):
                subject.validate_pe_exports(block, (0x2000, 0x2100, [(0x3000, 0x4000)]), {'mgmem_domain_create_v1'})

    def test_jni_requires_exact_one_core_dependency(self):
        subject.validate_host_core_dependency({'soname': ['libmgmem.so.1']},
                                              {'needed': ['libc.so.6', 'libmgmem.so.1']}, False)
        for needed in (['libmgmem.so.fake'], ['libmgmem.so.1', 'libmgmem.so.fake'],
                       ['libmgmem.so.1', 'libmgmem.so.1']):
            with self.subTest(needed=needed), self.assertRaisesRegex(ValueError, 'host_jni_core_dependency'):
                subject.validate_host_core_dependency({'soname': ['libmgmem.so.1']}, {'needed': needed}, False)
        with self.assertRaisesRegex(ValueError, 'host_core_soname_mismatch'):
            subject.validate_host_core_dependency({'soname': ['libmgmem.so.fake']},
                                                  {'needed': ['libmgmem.so.fake']}, False)
        subject.validate_host_core_dependency({}, {'needed': ['KERNEL32.dll', 'mgmem.dll']}, True)
        with self.assertRaisesRegex(ValueError, 'host_jni_core_dependency'):
            subject.validate_host_core_dependency({}, {'needed': ['mgmem.dll', 'mgmem_fake.dll']}, True)

    def test_dynamic_dependencies_and_elf_search_path_are_bounded(self):
        subject.validate_host_dependencies({'needed': ['KERNEL32.dll', 'VCRUNTIME140.dll']}, True, False)
        subject.validate_host_dependencies({'needed': ['libc.so.6', 'libmgmem.so.1']}, False, True)
        for is_windows, needed in ((True, ['KERNEL32.dll', 'unexpected.dll']),
                                   (False, ['libc.so.6', 'libasan.so.8'])):
            with self.subTest(needed=needed), self.assertRaisesRegex(ValueError, 'host_unexpected_dynamic_dependency'):
                subject.validate_host_dependencies({'needed': needed}, is_windows, False)
        with self.assertRaisesRegex(ValueError, 'host_unexpected_dynamic_dependency'):
            subject.validate_host_dependencies({'needed': ['libc.so.6', 'libc.so.6']}, False, False)
        subject.validate_elf_search_path('Dynamic section without search paths')
        for value in ('0x1 (RUNPATH) Library runpath: [/usr/local/lib]',
                      '0x1 (RPATH) Library rpath: [$ORIGIN/../../escape]',
                      '0x1 (RUNPATH) Library runpath: [$ORIGIN]',
                      '0x1 (FILTER) Filter library: [libalternate.so]',
                      '0x1 (AUXILIARY) Auxiliary library: [libalternate.so]',
                      '0x1 (AUDIT) Audit library: [libaudit.so]',
                      '0x1 (DEPAUDIT) Dependency audit library: [libaudit.so]'):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'host_elf_search_path_forbidden'):
                subject.validate_elf_search_path(value)

    def test_coff_section_metadata_and_payload_change_detection(self):
        text = '''  Section {\n    Name: .text\n    VirtualSize: 0\n    VirtualAddress: 0\n    RawDataSize: 2\n    PointerToRawData: 0x100\n    RelocationCount: 0\n    LineNumberCount: 0\n    Characteristics [ (0x60000020)\n    ]\n    SectionData (\n      0000: AABB |..|\n    )\n  }\n'''
        original = subject.parse_archive_sections(text)
        self.assertEqual(len(original), 1)
        self.assertNotEqual(original, subject.parse_archive_sections(text.replace('AABB', 'AABC')))
        self.assertNotEqual(original, subject.parse_archive_sections(text.replace('0x60000020', '0x40000020')))
        with self.assertRaisesRegex(ValueError, 'coff_section_size_mismatch'):
            subject.parse_archive_sections(text.replace('RawDataSize: 2', 'RawDataSize: 4'))
        with self.assertRaisesRegex(ValueError, 'coff_section_size_mismatch'):
            subject.parse_archive_sections(text.replace('    SectionData (\n      0000: AABB |..|\n    )\n', ''))
        bss = (text.replace('Name: .text', 'Name: .bss').replace('0x60000020', '0xC0000080')
               .replace('    SectionData (\n      0000: AABB |..|\n    )\n', ''))
        subject.parse_archive_sections(bss)

    def test_elf_initialized_and_nobits_section_payloads(self):
        text = '''  Section {\n    Name: .data\n    Type: SHT_PROGBITS\n    Flags [ (0x2)\n      SHF_ALLOC\n    ]\n    Size: 2\n    SectionData (\n      0000: AABB |..|\n    )\n  }\n'''
        subject.validate_elf_allocated_section_payloads(text)
        with self.assertRaisesRegex(ValueError, 'elf_static_section_size_mismatch'):
            subject.validate_elf_allocated_section_payloads(text.replace('    SectionData (\n      0000: AABB |..|\n    )\n', ''))
        nobits = text.replace('SHT_PROGBITS', 'SHT_NOBITS').replace('    SectionData (\n      0000: AABB |..|\n    )\n', '')
        subject.validate_elf_allocated_section_payloads(nobits)

    def test_wrong_coff_static_architecture_is_rejected(self):
        with patch.object(subject.common, 'run_tool', side_effect=['pressure.obj\n', 'Format: COFF-i386\n']):
            with self.assertRaisesRegex(ValueError, 'wrong_host_static_abi'):
                subject.coff_archive_contract({'llvm-ar': 'ar', 'llvm-readobj': 'readobj'},
                                              Path('pressure.lib'), Path('.'), [], 'pressure')

    def test_corrupt_or_wrong_host_elf_rejected_before_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'lib.so'; path.write_bytes(b'not-elf')
            with self.assertRaisesRegex(ValueError, 'wrong_host_elf_abi'):
                subject.dynamic_linux({}, path, Path(tmp), [], 'wrong')
            raw = bytearray(20); raw[:6] = b'\x7fELF\x02\x01'
            raw[16:18] = (3).to_bytes(2, 'little'); raw[18:20] = (183).to_bytes(2, 'little')
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'wrong_host_elf_abi'):
                subject.dynamic_linux({}, path, Path(tmp), [], 'wrong')

    def test_core_header_exports_are_complete_and_duplicates_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp); header = source / 'include/mgmem/mgmem.h'; header.parent.mkdir(parents=True)
            text = ('MGMEM_API mgmem_status mgmem_domain_create_v1(void);\n'
                    'MGMEM_API mgmem_status mgmem_import_external_checked_v1(void);\n'
                    'MGMEM_API uint64_t mgmem_capabilities_v1(void);\n')
            header.write_text(text, encoding='utf-8')
            self.assertEqual(len(subject.required_core_exports(source)), 3)
            header.write_text(text + '  MGMEM_API mgmem_status mgmem_indented_v1(void);\n', encoding='utf-8')
            self.assertIn('mgmem_indented_v1', subject.required_core_exports(source))
            header.write_text(text + 'MGMEM_API mgmem_status mgmem_domain_create_v1(void);\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'host_core_declarations_missing'):
                subject.required_core_exports(source)
            header.write_text(text + 'MGMEM_API unsupported syntax\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'host_core_declarations_unparseable'):
                subject.required_core_exports(source)

    def test_pressure_header_requires_v1_v2_and_rejects_duplicates(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp); header = source / 'include/mgmem/pressure.h'; header.parent.mkdir(parents=True)
            text = ('MGMEM_PRESSURE_API mgmem_status mgmem_pressure_create_v1(void);\n'
                    '  MGMEM_PRESSURE_API mgmem_status mgmem_pressure_create_v2(void);\n')
            header.write_text(text, encoding='utf-8')
            self.assertEqual(len(subject.required_pressure_exports(source)), 2)
            header.write_text(text + 'MGMEM_PRESSURE_API mgmem_status mgmem_pressure_create_v1(void);\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'host_pressure_declarations_missing'):
                subject.required_pressure_exports(source)

    def test_safe_materialization_rejects_path_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'invalid_source_path'):
                subject.materialize({'../outside': b'x'}, {}, Path(tmp) / 'output')


if __name__ == '__main__':
    unittest.main()
