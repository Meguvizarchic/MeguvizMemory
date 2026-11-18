#!/usr/bin/env python3
"""Build a source-locked Windows/Linux host SDK and managed-JVM candidate.

No Android device, signing, or publication is involved. Raw files and logs
remain under build/; only a checked archive is moved into dist/.
"""
from __future__ import annotations

import argparse
import gzip
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile

import build_android_candidate as common

ROOT = Path(__file__).resolve().parents[1]
JAVA = 'adapters/android/java/io/github/jichuo1/meguviz/memory/android/'
JAVA_PACKAGE = 'io/github/jichuo1/meguviz/memory/android/'
README = b'''# MeguvizMemory host engineering candidate\n\nThis is a host SDK, host JNI composition and pure-JVM managed API JAR.\nIt is not an Android AAR or a formal release. The JAR includes only\nNativeMemoryLease and ManagedMemoryDomain; Android framework callbacks are not\nincluded. Do not use this JNI library with an independently embedded core.\n\nOn Windows the appropriate Microsoft C++ runtime must be available. On Linux\npreserve the included relative shared-library symlinks and make sdk/lib\navailable to the dynamic loader. No runtime libraries are bundled.\n\nThe installed image consumer checks are host-only and not a performance, ART,\nR8, Android device, MTE, energy, BIL or formal-release qualification. The\nproject's release license has not been specified. Path/identifier scanning is\nbounded and cannot establish absence of all private information.\n'''
FORBIDDEN_SUFFIXES = {'.apk', '.dex', '.key', '.pem', '.p12', '.pfx', '.jks', '.keystore'}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def validate_host_member_name(name):
    path = common.safe_relative(name)
    lower = path.name.lower()
    require(path.suffix.lower() not in FORBIDDEN_SUFFIXES and lower != '.env' and not lower.startswith('.env.'),
            'forbidden_host_archive_member')
    return path


def parse_archive_sections(text):
    blocks = re.findall(r'(?ms)^  Section \{\n(.*?)^  \}', text.replace('\r\n', '\n'))
    rows = []
    for block in blocks:
        name = re.search(r'(?m)^    Name: (\S+)', block)
        require(name is not None, 'coff_section_name_missing')
        name = name.group(1)
        if name.startswith('.debug'):
            continue
        fields = []
        for key in ('VirtualSize', 'VirtualAddress', 'RawDataSize', 'RelocationCount', 'LineNumberCount'):
            match = re.search(r'(?m)^    ' + key + r': (\S+)', block)
            require(match is not None, 'coff_section_metadata_missing')
            fields.append(match.group(1))
        flags = re.search(r'(?m)^    Characteristics \[ \((0x[0-9A-Fa-f]+)\)', block)
        pointer = re.search(r'(?m)^    PointerToRawData: (0x[0-9A-Fa-f]+)', block)
        require(flags is not None, 'coff_section_flags_missing')
        require(pointer is not None, 'coff_section_raw_pointer_missing')
        section = re.search(r'(?ms)^    SectionData \(\n(.*?)^    \)', block)
        pieces = ([''.join(re.findall(r'\b[0-9A-F]{2,8}\b', line.split('|')[0].split(':', 1)[1]))
                   for line in section.group(1).splitlines() if ':' in line] if section else [])
        payload = bytes.fromhex(''.join(pieces))
        size = int(fields[2])
        # COFF uninitialized-data sections have no file payload; llvm-strip
        # may assign them a nonzero raw-data pointer, so the characteristic is
        # authoritative rather than the incidental pointer value.
        uninitialized = bool(int(flags.group(1), 16) & 0x80)
        require((not payload if uninitialized else len(payload) == size), 'coff_section_size_mismatch')
        rows.append([name, *fields, flags.group(1), uninitialized, common.sha(payload)])
    require(rows, 'coff_sections_missing')
    return rows


def validate_elf_allocated_section_payloads(text):
    blocks = re.findall(r'(?ms)^  Section \{\n(.*?)^  \}', text.replace('\r\n', '\n'))
    checked = 0
    for block in blocks:
        if 'SHF_ALLOC' not in block:
            continue
        size = re.search(r'(?m)^    Size: (\d+)', block)
        kind = re.search(r'(?m)^    Type: (\S+)', block)
        require(size is not None and kind is not None, 'elf_static_section_metadata_missing')
        section = re.search(r'(?ms)^    SectionData \(\n(.*?)^    \)', block)
        pieces = ([''.join(re.findall(r'\b[0-9A-F]{2,8}\b', line.split('|')[0].split(':', 1)[1]))
                   for line in section.group(1).splitlines() if ':' in line] if section else [])
        payload = bytes.fromhex(''.join(pieces))
        if kind.group(1) == 'SHT_NOBITS':
            require(not payload, 'elf_nobits_has_payload')
        else:
            require(len(payload) == int(size.group(1)), 'elf_static_section_size_mismatch')
        checked += 1
    require(checked > 0, 'elf_static_allocated_sections_missing')


def coff_archive_contract(tools, path, work, operations, label):
    members = common.run_tool([tools['llvm-ar'], 't', path], work, operations, label + '-members').splitlines()
    require(len(members) == 1 and len(set(members)) == len(members), 'coff_archive_members')
    header = common.run_tool([tools['llvm-readobj'], '--file-headers', path], work, operations, label + '-header')
    require('Format: COFF-x86-64' in header and 'IMAGE_FILE_MACHINE_AMD64' in header, 'wrong_host_static_abi')
    sections = common.run_tool([tools['llvm-readobj'], '--sections', '--section-data', path],
                               work, operations, label + '-sections')
    reloc = common.run_tool([tools['llvm-readobj'], '--relocations', path], work, operations, label + '-relocations')
    groups = re.findall(r'(?ms)^  Section \(\d+\) (\S+) \{\n(.*?)^  \}', reloc.replace('\r\n', '\n'))
    rows = [[name, [re.sub(r'\s+\(\d+\)\s*$', '', line.strip()) for line in body.splitlines() if line.strip()]]
            for name, body in groups if not name.startswith('.debug')]
    require(rows, 'coff_relocations_missing')
    symbols = common.run_tool([tools['llvm-nm'], '--defined-only', '--extern-only', '--format=posix', path],
                              work, operations, label + '-symbols')
    exports = sorted(line.strip() for line in symbols.splitlines()
                     if line.strip() and not line.rstrip().endswith(':'))
    require(exports and not any('mgmem_pressure_test_' in line or 'managed_test_' in line for line in exports),
            'coff_static_export_invalid')
    return {'members': members, 'sections_sha256': common.sha(common.canonical(parse_archive_sections(sections))),
            'relocations_sha256': common.sha(common.canonical(rows)), 'symbols_sha256': common.sha(common.canonical(exports))}


def validate_relative_symlinks(links, files):
    """Only flat, relative Linux loader links ending at a packaged file."""
    for name, target in links.items():
        path = validate_host_member_name(name)
        require(path.parts[0] == 'sdk' and len(path.parts) == 3 and path.parts[1] == 'lib', 'host_symlink_path')
        require(target and '/' not in target and '\\' not in target and target not in ('.', '..'), 'host_symlink_target')
        resolved = str(path.parent / target)
        visited = {name}
        while resolved in links:
            require(resolved not in visited, 'host_symlink_cycle')
            visited.add(resolved)
            next_target = links[resolved]
            require(next_target and '/' not in next_target and '\\' not in next_target and
                    next_target not in ('.', '..'), 'host_symlink_target')
            resolved = str(PurePosixPath(resolved).parent / next_target)
        require(resolved in files, 'host_symlink_dangling')


def tar_bytes(files, links):
    require(not set(files).intersection(links), 'duplicate_host_member')
    validate_relative_symlinks(links, files)
    output = io.BytesIO()
    with gzip.GzipFile(fileobj=output, mode='wb', mtime=0, filename='') as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name in sorted(set(files) | set(links)):
                validate_host_member_name(name)
                item = tarfile.TarInfo(name); item.uid = item.gid = 0; item.uname = item.gname = ''; item.mtime = 0
                if name in links:
                    item.type = tarfile.SYMTYPE; item.linkname = links[name]; item.mode = 0o777
                    archive.addfile(item)
                else:
                    item.size = len(files[name]); item.mode = 0o644
                    archive.addfile(item, io.BytesIO(files[name]))
    return output.getvalue()


def tar_members(data):
    files, links, folded_names = {}, {}, set()
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for item in archive:
            validate_host_member_name(item.name)
            require(item.name not in files and item.name not in links and item.name.casefold() not in folded_names,
                    'duplicate_host_member')
            folded_names.add(item.name.casefold())
            if item.issym():
                links[item.name] = item.linkname
            else:
                require(item.isfile() and item.size <= 64 * 1024 * 1024, 'invalid_host_tar_member')
                total += item.size; require(total <= 256 * 1024 * 1024, 'host_tar_too_large')
                stream = archive.extractfile(item); require(stream is not None, 'host_tar_member_missing')
                value = stream.read(); require(len(value) == item.size, 'host_tar_member_short')
                files[item.name] = value
    validate_relative_symlinks(links, files)
    return files, links


def scan_host_archive(data, markers, is_windows):
    scanner = common.Scanner(markers)
    if is_windows:
        scanner.archive(data, 'host-candidate')
        return common.archive_members(data), {}
    scanner.identifiers(data, 'host-candidate:compressed')
    files, links = tar_members(data)
    for name, value in files.items():
        scanner.bytes(name.encode(), 'host-member-name')
        if zipfile_is(value):
            scanner.archive(value, 'host-member-archive')
        else:
            scanner.bytes(value, 'host-member')
    for name, target in links.items():
        scanner.bytes(name.encode(), 'host-link-name'); scanner.bytes(target.encode(), 'host-link-target')
    return files, links


def zipfile_is(data):
    import zipfile
    return zipfile.is_zipfile(io.BytesIO(data))


def materialize(files, links, directory):
    directory.mkdir()
    validate_relative_symlinks(links, files)
    for name, data in files.items():
        path = directory.joinpath(*validate_host_member_name(name).parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as out:
            out.write(data)
    for name, target in links.items():
        path = directory.joinpath(*validate_host_member_name(name).parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(target, path)


def executable(tools, name):
    path = tools / (name + ('.exe' if os.name == 'nt' else ''))
    require(path.is_file(), 'required_host_tool_missing')
    # LLVM multicall binaries select their mode from argv[0].  In particular,
    # llvm-strip can be a symlink to llvm-objcopy; resolving that link before
    # execution changes its mode and makes strip's -o option invalid.
    # file_identity still follows the link when checking tool bytes.
    return path


def read_cmake_compiler(cache):
    value = re.search(r'(?m)^CMAKE_CXX_COMPILER:FILEPATH=(.+)$', cache.read_text(encoding='utf-8'))
    require(value is not None, 'host_compiler_identity_missing')
    path = Path(value.group(1).strip()).resolve(strict=True)
    require(path.is_file(), 'host_compiler_missing')
    return path


def require_cached_package_dir(cache, expected):
    match = re.search(r'(?m)^MeguvizMemory_DIR:[^=]+=(.+)$', cache.read_text(encoding='utf-8'))
    require(match is not None and Path(match.group(1).strip()).resolve() == expected.resolve(),
            'host_cmake_package_dir_mismatch')


def required_core_exports(source):
    text = (source / 'include/mgmem/mgmem.h').read_text(encoding='utf-8')
    declarations = re.findall(r'(?m)^[ \t]*MGMEM_API\b[^\n]*', text)
    names = [re.match(r'^[ \t]*MGMEM_API\s+\w+\s+(mgmem_\w+)\s*\(', line) for line in declarations]
    require(declarations and all(names), 'host_core_declarations_unparseable')
    result = {match.group(1) for match in names}
    require(len(result) == len(declarations) and
            {'mgmem_domain_create_v1', 'mgmem_import_external_checked_v1', 'mgmem_capabilities_v1'} <= result,
            'host_core_declarations_missing')
    return result


def required_pressure_exports(source):
    text = (source / 'include/mgmem/pressure.h').read_text(encoding='utf-8')
    declarations = re.findall(r'(?m)^[ \t]*MGMEM_PRESSURE_API\b[^\n]*', text)
    names = [re.match(r'^[ \t]*MGMEM_PRESSURE_API\s+\w+\s+(mgmem_pressure_\w+)\s*\(', line)
             for line in declarations]
    require(declarations and all(names), 'host_pressure_declarations_unparseable')
    result = {match.group(1) for match in names}
    require(len(result) == len(declarations) and {'mgmem_pressure_create_v1', 'mgmem_pressure_create_v2'} <= result,
            'host_pressure_declarations_missing')
    return result


def validate_pressure_archive_exports(tools, archive, source, work, operations):
    text = common.run_tool([tools['llvm-nm'], '--defined-only', '--extern-only', '--format=posix', archive],
                           work, operations, 'pressure-clean-public-exports')
    names = {line.split()[0] for line in text.splitlines() if line.strip() and not line.rstrip().endswith(':')}
    expected = required_pressure_exports(source)
    require(expected <= names, 'host_pressure_export_missing')
    return len(expected)


def pe_export_directory(path):
    data = path.read_bytes()
    require(len(data) >= 64 and data[:2] == b'MZ', 'invalid_host_pe')
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    require(offset + 24 <= len(data) and data[offset:offset + 4] == b'PE\0\0', 'invalid_host_pe')
    machine, section_count = struct.unpack_from('<HH', data, offset + 4)
    optional_size = struct.unpack_from('<H', data, offset + 20)[0]
    optional = offset + 24
    require(machine == 0x8664 and optional_size >= 120 and optional + optional_size <= len(data) and
            struct.unpack_from('<H', data, optional)[0] == 0x20b and
            struct.unpack_from('<I', data, optional + 108)[0] >= 1, 'invalid_host_pe')
    start, size = struct.unpack_from('<II', data, optional + 112)
    image_size = struct.unpack_from('<I', data, optional + 56)[0]
    table = optional + optional_size
    require(0 < section_count <= 96 and table + section_count * 40 <= len(data) and image_size > 0 and
            start > 0 and size > 0 and start + size <= image_size, 'invalid_host_pe_export_directory')
    executable = []
    for index in range(section_count):
        section = table + index * 40
        virtual_size, address, raw_size = struct.unpack_from('<III', data, section + 8)
        characteristics = struct.unpack_from('<I', data, section + 36)[0]
        length = max(virtual_size, raw_size)
        if characteristics & 0x20000000 and length:
            require(address < image_size and address + length <= image_size, 'invalid_host_pe_executable_section')
            executable.append((address, address + length))
    require(executable, 'host_pe_executable_sections_missing')
    return start, start + size, executable


def validate_pe_exports(text, export_range, required):
    blocks = re.findall(r'(?ms)^Export \{\n(.*?)^\}', text.replace('\r\n', '\n'))
    names = set()
    for block in blocks:
        name = re.search(r'(?m)^\s*Name:\s*(\S+)\s*$', block)
        if not name:
            continue
        name = name.group(1)
        address = re.search(r'(?m)^\s*RVA:\s*(0x[0-9A-Fa-f]+|\d+)\s*$', block)
        require(address is not None and name not in names, 'host_pe_export_invalid')
        rva = int(address.group(1), 0)
        require(rva != 0 and not (export_range[0] <= rva < export_range[1]) and
                not re.search(r'(?m)^\s*Forwarder:', block), 'host_pe_forwarded_or_empty_export')
        require(any(start <= rva < end for start, end in export_range[2]), 'host_pe_export_not_executable')
        names.add(name)
    require(set(required) <= names, 'host_pe_export_missing')
    require(not any('managed_test_' in name or 'mgmem_pressure_test_' in name for name in names), 'host_pe_test_export')
    return names


def dynamic_windows(tools, path, work, operations, label, required_exports=()):
    imports = common.run_tool([tools['llvm-readobj'], '--coff-imports', path], work, operations, label + '-imports')
    exports = common.run_tool([tools['llvm-readobj'], '--coff-exports', path], work, operations, label + '-exports')
    require('Format: COFF-x86-64' in imports and 'Format: COFF-x86-64' in exports, 'wrong_host_pe_abi')
    dependencies = re.findall(r'(?m)^  Name: (\S+)', imports)
    exported = validate_pe_exports(exports, pe_export_directory(path), required_exports)
    return {'needed': dependencies, 'exports_sha256': common.sha(common.canonical(sorted(exported)))}


def dynamic_linux(tools, path, work, operations, label, required_exports=()):
    data = path.read_bytes()
    require(len(data) >= 20 and data[:6] == b'\x7fELF\x02\x01' and
            int.from_bytes(data[16:18], 'little') == 3 and int.from_bytes(data[18:20], 'little') == 62,
            'wrong_host_elf_abi')
    dynamic, symbols = common.dynamic_contract(tools['llvm-readelf'], path, work, operations, label)
    table = common.run_tool([tools['llvm-readelf'], '--dynamic', '--wide', path], work, operations, label + '-search-path')
    validate_elf_search_path(table)
    common.validate_jni_symbols(symbols, set(required_exports)) if required_exports else None
    return dynamic


def validate_elf_search_path(dynamic_table):
    # This host SDK requires an explicit loader search path. Reject even a
    # relative RUNPATH and alternate loader modules we did not configure.
    require(not re.search(r'\((?:RPATH|RUNPATH|FILTER|AUXILIARY|AUDIT|DEPAUDIT)\)', dynamic_table),
            'host_elf_search_path_forbidden')


WINDOWS_RUNTIME_LIBRARIES = {
    'kernel32.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'ucrtbase.dll',
    *('api-ms-win-crt-' + name + '-l1-1-0.dll' for name in
      ('heap', 'string', 'runtime', 'math', 'stdio', 'locale', 'convert', 'time',
       'filesystem', 'utility', 'environment', 'conio')),
}
LINUX_RUNTIME_LIBRARIES = {'libc.so.6', 'libstdc++.so.6', 'libgcc_s.so.1', 'libm.so.6',
                           'libpthread.so.0', 'libdl.so.2', 'librt.so.1', 'libatomic.so.1',
                           'ld-linux-x86-64.so.2'}


def validate_host_dependencies(contract, is_windows, include_core):
    allowed = set(WINDOWS_RUNTIME_LIBRARIES if is_windows else LINUX_RUNTIME_LIBRARIES)
    if include_core:
        allowed.add('mgmem.dll' if is_windows else 'libmgmem.so.1')
    needed = [name.lower() if is_windows else name for name in contract['needed']]
    require(len(needed) == len(set(needed)) and set(needed) <= allowed, 'host_unexpected_dynamic_dependency')


def validate_host_core_dependency(core_contract, jni_contract, is_windows):
    dependencies = jni_contract['needed']
    if is_windows:
        expected = 'mgmem.dll'
        normalized = [name.lower() for name in dependencies]
        related = [name for name in normalized if name.startswith(('mgmem', 'libmgmem'))]
        require(related == [expected], 'host_jni_core_dependency')
    else:
        # The installed core target and symlinks use native ABI/SOVERSION 1.
        require(core_contract.get('soname') == ['libmgmem.so.1'], 'host_core_soname_mismatch')
        related = [name for name in dependencies if name.startswith(('libmgmem', 'mgmem'))]
        require(related == core_contract['soname'], 'host_jni_core_dependency')


def archive_data(files, links, is_windows):
    if is_windows:
        require(not links, 'host_windows_symlink_unsupported')
        return common.zip_bytes(files)
    return tar_bytes(files, links)


def sdk_files_for_host(sdk, is_windows):
    files, links = {}, {}
    for path in sdk.rglob('*'):
        name = 'sdk/' + path.relative_to(sdk).as_posix()
        common.safe_relative(name)
        if path.is_symlink():
            require(not is_windows, 'host_windows_symlink_unsupported')
            links[name] = os.readlink(path)
        elif path.is_file():
            files[name] = path.read_bytes()
    validate_relative_symlinks(links, files)
    include_prefix = 'sdk/include/mgmem/'
    require(any(name.startswith(include_prefix) for name in files), 'host_sdk_headers_missing')
    expected = ({'sdk/bin/mgmem.dll', 'sdk/bin/mgmem_pressure_jni.dll', 'sdk/lib/mgmem.lib',
                 'sdk/lib/mgmem_pressure.lib', 'sdk/lib/mgmem_pressure_jni.lib'} if is_windows else
                {'sdk/lib/libmgmem_pressure.a', 'sdk/lib/libmgmem_pressure_jni.so'})
    cmake = {'sdk/lib/cmake/MeguvizMemory/' + name for name in ('MeguvizMemoryConfig.cmake',
        'MeguvizMemoryConfigVersion.cmake', 'MeguvizMemoryTargets.cmake', 'MeguvizMemoryTargets-release.cmake')}
    require(expected | cmake <= set(files), 'host_sdk_member_missing')
    if not is_windows:
        require('sdk/lib/libmgmem.so' in links and 'sdk/lib/libmgmem.so.1' in links and
                'sdk/lib/libmgmem.so.1.2.1' in files, 'host_linux_core_links_missing')
        expected |= {'sdk/lib/libmgmem.so.1.2.1'}
    allowed = expected | cmake | {name for name in files if name.startswith(include_prefix)}
    require(set(files) == allowed, 'host_sdk_unexpected_member')
    return files, links


def validate_sdk_headers(source, files):
    root = source / 'include/mgmem'
    expected = {p.relative_to(root).as_posix(): p.read_bytes()
                for p in root.rglob('*') if p.is_file() and not p.is_symlink()}
    actual = {name.removeprefix('sdk/include/mgmem/'): value for name, value in files.items()
              if name.startswith('sdk/include/mgmem/')}
    require(expected and expected == actual, 'host_sdk_header_mismatch')


def configure(tools, source, build, work, operations, label, options=()):
    command = [tools['cmake'], '-S', source, '-B', build, '-G', 'Ninja',
               '-DCMAKE_MAKE_PROGRAM=' + str(tools['ninja']), '-DCMAKE_BUILD_TYPE=Release', *options]
    common.run_tool(command, work, operations, label)


def host_executable_dependencies(tools, path, work, operations, label, is_windows):
    if is_windows:
        text = common.run_tool([tools['llvm-readobj'], '--coff-imports', path], work, operations, label + '-imports')
        require('Format: COFF-x86-64' in text, 'wrong_host_consumer_abi')
        return {'needed': re.findall(r'(?m)^  Name: (\S+)', text)}
    data = path.read_bytes()
    require(len(data) >= 20 and data[:6] == b'\x7fELF\x02\x01' and
            int.from_bytes(data[16:18], 'little') in (2, 3) and int.from_bytes(data[18:20], 'little') == 62,
            'wrong_host_consumer_abi')
    text = common.run_tool([tools['llvm-readelf'], '--dynamic', '--wide', path], work, operations, label + '-dynamic')
    return {'needed': re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', text)}


def pressure_consumer_sources(consumer_source, builds):
    """Wrap each locked host test to attest its own loaded core identity."""
    fixture = builds / 'pressure-consumer-source'; fixture.mkdir()
    transformed = {}
    for mode, name in (('v1', 'pressure_tests.cpp'), ('v2', 'managed_pressure_v2.cpp')):
        original = (consumer_source / 'tests' / name).read_bytes()
        require(original.count(b'int main()') == 1, 'pressure_test_main_unexpected')
        main = original[original.index(b'int main()'):]
        require(not re.search(rb'\breturn\b', main), 'pressure_test_main_return_unsupported')
        adapted = original.replace(b'int main()', b'void mgmem_embedded_test_main()', 1)
        (fixture / (mode + '-test.cpp')).write_bytes(adapted)
        transformed[mode] = {'source_sha256': common.sha(original), 'adapted_sha256': common.sha(adapted),
                             'transformation': 'MAIN_NAME_AND_RETURN_TYPE_ONLY'}
        wrapper = '''#include "''' + mode + '''-test.cpp"
#include <filesystem>
#include <system_error>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
int wmain(int argc, wchar_t **argv) {
  if (argc != 2) return 90;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(&mgmem_domain_create_v1)), &module)) return 91;
  wchar_t path[32768]; DWORD length = GetModuleFileNameW(module, path, 32768);
  if (!length || length >= 32768) return 92;
  std::error_code error;
  if (!std::filesystem::equivalent(std::filesystem::path(path), std::filesystem::path(argv[1]), error) || error) return 93;
#else
#include <dlfcn.h>
int main(int argc, char **argv) {
  if (argc != 2) return 90;
  Dl_info info{};
  if (!dladdr(reinterpret_cast<const void *>(&mgmem_domain_create_v1), &info) || !info.dli_fname) return 91;
  std::error_code error;
  if (!std::filesystem::equivalent(std::filesystem::path(info.dli_fname), std::filesystem::path(argv[1]), error) || error) return 93;
#endif
  if (mgmem_abi_version_v1() != ((MGMEM_ABI_MAJOR << 16) | MGMEM_ABI_MINOR)) return 94;
  if (!(mgmem_capabilities_v1() & MGMEM_CAP_CHECKED_EXTERNAL_RELEASE)) return 95;
  mgmem_embedded_test_main();
  return 0;
}
'''
        (fixture / (mode + '-wrapper.cpp')).write_text(wrapper, encoding='utf-8', newline='\n')
    (fixture / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(InstalledPressureProbe LANGUAGES CXX)
find_package(MeguvizMemory CONFIG REQUIRED)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected-paths.txt"
  CONTENT "$<TARGET_FILE:MeguvizMemory::pressure>\\n$<TARGET_FILE:MeguvizMemory::mgmem>\\n")
foreach(mode v1 v2)
  add_executable(pressure_${mode} ${mode}-wrapper.cpp)
  target_compile_features(pressure_${mode} PRIVATE cxx_std_17)
  target_link_libraries(pressure_${mode} PRIVATE MeguvizMemory::pressure ${CMAKE_DL_LIBS})
endforeach()
''', encoding='utf-8', newline='\n')
    return fixture, transformed


def verify_packaged_pressure(tools, consumer_source, builds, sdk, work, operations, is_windows):
    fixture, transformed = pressure_consumer_sources(consumer_source, builds)
    build = builds / 'pressure-consumer-build'
    package_dir = sdk / 'lib/cmake/MeguvizMemory'
    configure(tools, fixture, build, work, operations, 'pressure-consumer-configure',
              ('-DCMAKE_PREFIX_PATH=' + sdk.as_posix(), '-DMeguvizMemory_DIR=' + package_dir.as_posix()))
    require_cached_package_dir(build / 'CMakeCache.txt', package_dir)
    selected = (build / 'selected-paths.txt').read_text(encoding='utf-8').splitlines()
    pressure = sdk / ('lib/mgmem_pressure.lib' if is_windows else 'lib/libmgmem_pressure.a')
    core = sdk / ('bin/mgmem.dll' if is_windows else 'lib/libmgmem.so')
    require(len(selected) == 2 and Path(selected[0]).resolve() == pressure.resolve() and
            Path(selected[1]).resolve() == core.resolve(), 'pressure_consumer_imported_path_mismatch')
    common.run_tool([tools['cmake'], '--build', build, '--parallel', '2'], work, operations, 'pressure-consumer-build')
    old_path = os.environ.get('PATH', ''); old_ld = os.environ.get('LD_LIBRARY_PATH')
    os.environ['PATH'] = str(core.parent) + os.pathsep + old_path
    if not is_windows:
        os.environ['LD_LIBRARY_PATH'] = str(core.parent) + (os.pathsep + old_ld if old_ld else '')
    try:
        for mode in ('v1', 'v2'):
            exe = build / ('pressure_' + mode + ('.exe' if is_windows else ''))
            dependencies = host_executable_dependencies(tools, exe, work, operations, 'pressure-' + mode, is_windows)
            validate_host_dependencies(dependencies, is_windows, True)
            validate_host_core_dependency({'soname': ['libmgmem.so.1']}, dependencies, is_windows)
            output = common.run_tool([exe, core.resolve()], work, operations, 'pressure-' + mode + '-run', timeout=60)
            stderr = (work / 'logs' / ('pressure-' + mode + '-run.stderr')).read_bytes()
            require(not stderr, 'pressure_consumer_unexpected_stderr')
            if mode == 'v1':
                result = json.loads(output)
                require(result.get('status') == 'PASS' and result.get('tests') == 3, 'pressure_v1_result_mismatch')
            else:
                require(re.fullmatch(r'managed-pressure-v2 PASS \(\d+ checks\)\s*', output) is not None,
                        'pressure_v2_result_mismatch')
    finally:
        os.environ['PATH'] = old_path
        if old_ld is None: os.environ.pop('LD_LIBRARY_PATH', None)
        else: os.environ['LD_LIBRARY_PATH'] = old_ld
    return {'state': 'PASS_HOST_PACKAGED_STATIC_PRESSURE_V1_V2', 'core_identity': 'EACH_PROCESS_OS_MODULE_PATH',
            'required_checked_external_capability': 'PASS', 'test_sources': transformed,
            'pressure_sha256': common.file_identity(pressure)['sha256'], 'core_sha256': common.file_identity(core)['sha256']}


def verify_installed_consumer(tools, source, records, lock_path, files, links, work, operations, is_windows):
    """Exercise bytes recovered from the provisional archive via real consumers."""
    recovered = work / 'candidate-readback'
    materialize(files, links, recovered)
    sdk = recovered / 'sdk'; jar = recovered / 'java/managed-api.jar'
    core = sdk / ('bin/mgmem.dll' if is_windows else 'lib/libmgmem.so')
    jni = sdk / ('bin/mgmem_pressure_jni.dll' if is_windows else 'lib/libmgmem_pressure_jni.so')
    require(core.is_file() and jni.is_file() and jar.is_file(), 'host_readback_missing')
    consumer_source = work / 'consumer-source'
    common.copy_snapshot(source, consumer_source, records)
    builds = consumer_source / 'build'; builds.mkdir()
    pressure_result = verify_packaged_pressure(tools, consumer_source, builds, sdk, work, operations, is_windows)
    plain, probe, reference = builds / 'plain', builds / 'probe', builds / 'reference'
    cmake_prefix = '-DCMAKE_PREFIX_PATH=' + sdk.as_posix()
    package_dir = sdk / 'lib/cmake/MeguvizMemory'
    package_option = '-DMeguvizMemory_DIR=' + package_dir.as_posix()
    jar_option = '-DMGMEM_ANDROID_CLASSES_JAR=' + jar.as_posix()
    configure(tools, consumer_source / 'consumers/java_image', plain, work, operations, 'consumer-plain-configure',
              (jar_option,))
    common.run_tool([tools['cmake'], '--build', plain, '--parallel', '2'], work, operations, 'consumer-plain-build')
    configure(tools, consumer_source / 'consumers/java_image', probe, work, operations, 'consumer-probe-configure',
              (cmake_prefix, package_option, jar_option, '-DMGMEM_JNI_PREFIX=' + sdk.as_posix(),
               '-DMGMEM_JAVA_IMAGE_BUILD_IDENTITY_PROBE=ON'))
    require_cached_package_dir(probe / 'CMakeCache.txt', package_dir)
    common.run_tool([tools['cmake'], '--build', probe, '--parallel', '2'], work, operations, 'consumer-probe-build')
    configure(tools, consumer_source / 'consumers/image', reference, work, operations, 'consumer-reference-configure',
              (cmake_prefix, package_option))
    require_cached_package_dir(reference / 'CMakeCache.txt', package_dir)
    common.run_tool([tools['cmake'], '--build', reference, '--target', 'mgmem_image', '--parallel', '2'],
                    work, operations, 'consumer-reference-build')
    probe_path = Path((probe / 'java-image-library-Release.txt').read_text(encoding='utf-8').strip()).resolve()
    require(probe_path.is_relative_to(probe.resolve()) and probe_path.is_file(), 'host_probe_path_invalid')
    reference_exe = reference / ('mgmem_image.exe' if is_windows else 'mgmem_image')
    verify_out = builds / 'installed-consumer-verification'
    common.run_tool([sys.executable, consumer_source / 'consumers/java_image/verify_host.py',
        '--java', tools['java'], '--javac', tools['javac'], '--reference', reference_exe,
        '--core', core, '--jni', jni, '--consumer-jni', probe_path, '--classes', probe / 'classes',
        '--plain-classes', plain / 'classes', '--adapter-jar', jar, '--out-dir', verify_out],
        work, operations, 'consumer-verify', timeout=600)
    report = json.loads((verify_out / 'receipt.json').read_text(encoding='utf-8'))
    require(report.get('state') == 'PASS_HOST_FUNCTIONAL_DEVELOPMENT' and len(report.get('cases', [])) == 12 and
            len(report.get('pressure_cases', [])) == 12 and len(report.get('negative_cases', [])) == 10 and
            report.get('pressure_retry_contract') == 'PASS_JVM_ADMISSION_ONLY' and
            report.get('plain_without_sidecar') == 'PASS', 'host_consumer_result_mismatch')
    identities = report.get('identities', {})
    require(identities.get('core_sha256') == common.file_identity(core)['sha256'] and
            identities.get('jni_sha256') == common.file_identity(jni)['sha256'] and
            identities.get('java_jar_sha256') == common.file_identity(jar)['sha256'], 'host_loaded_identity_mismatch')
    # Build outputs under build/ are excluded by source_lock; recorded sources
    # must still be byte-identical and the original snapshot must be untouched.
    _, after = common.verify_lock(consumer_source, lock_path)
    require(after == records, 'host_consumer_source_changed')
    return {'state': 'PASS_HOST_INSTALLED_JVM_JNI_IMAGE', 'default_cases': 12,
            'synthetic_pressure_cases': 12, 'negative_cases': 10,
            'packaged_pressure': pressure_result,
            'receipt_sha256': common.file_identity(verify_out / 'receipt.json')['sha256'],
            'core_sha256': common.file_identity(core)['sha256'], 'jni_sha256': common.file_identity(jni)['sha256'],
            'jar_sha256': common.file_identity(jar)['sha256']}


def build_host(args, work, source, lock_path, records, markers, is_windows):
    operations = []
    tools = {'cmake': args.cmake.resolve(strict=True), 'ninja': args.ninja.resolve(strict=True)}
    jdk = args.jdk.resolve(strict=True)
    for name in ('java', 'javac'):
        tools[name] = executable(jdk / 'bin', name)
    llvm = args.llvm_bin.resolve(strict=True)
    for name in ('llvm-strip', 'llvm-readobj', 'llvm-nm', 'llvm-ar', 'llvm-readelf'):
        tools[name] = executable(llvm, name)
    require(all(path.is_file() for path in tools.values()), 'host_tool_missing')
    tool_hashes = {name: common.file_identity(path) for name, path in tools.items()}
    # Only this builder process and children see these settings.
    os.environ['JAVA_HOME'] = str(jdk)
    os.environ['PATH'] = str(jdk / 'bin') + os.pathsep + os.environ.get('PATH', '')

    classes = work / 'classes'; classes.mkdir()
    headers = work / 'jni-headers'; headers.mkdir()
    java_sources = [source / (JAVA + name + '.java') for name in ('NativeMemoryLease', 'ManagedMemoryDomain')]
    common.run_tool([tools['javac'], '--release', '8', '-h', headers, '-d', classes, *java_sources],
                    work, operations, 'compile-managed-java')
    header_names = ['io_github_jichuo1_meguviz_memory_android_' + name + '.h'
                    for name in ('NativeMemoryLease', 'ManagedMemoryDomain')]
    require(all((headers / name).is_file() for name in header_names), 'host_generated_jni_headers_missing')
    umbrella = headers / 'all-host-jni-generated.h'
    umbrella.write_text(''.join('#include "' + name + '"\n' for name in header_names), encoding='ascii', newline='\n')
    class_files = {p.relative_to(classes).as_posix(): p.read_bytes() for p in classes.rglob('*') if p.is_file()}
    for name in ('NativeMemoryLease', 'ManagedMemoryDomain', 'ManagedMemoryDomain$Pressure',
                 'ManagedMemoryDomain$Allocation', 'ManagedMemoryDomain$Lease'):
        require(JAVA_PACKAGE + name + '.class' in class_files, 'host_java_class_missing')
    jar = common.zip_bytes(class_files)

    sdk = work / 'raw-sdk'; core_build = work / 'core-build'; jni_build = work / 'jni-build'
    configure(tools, source, core_build, work, operations, 'core-configure',
              ('-DBUILD_SHARED_LIBS=ON', '-DMGMEM_BUILD_TESTS=OFF', '-DMGMEM_BUILD_BENCHMARKS=OFF',
               '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_INSTALL_BINDIR=bin', '-DCMAKE_INSTALL_PREFIX=' + sdk.as_posix()))
    compiler = read_cmake_compiler(core_build / 'CMakeCache.txt')
    compiler_hash = common.file_identity(compiler)
    common.run_tool([tools['cmake'], '--build', core_build, '--parallel', str(args.jobs)], work, operations, 'core-build')
    common.run_tool([tools['cmake'], '--install', core_build], work, operations, 'core-install')
    # CMake's compiler try-compile also sees CMAKE_CXX_FLAGS before FindJNI has
    # attached target include dirs, so include the matching JDK headers there.
    jni_include = (jdk / 'include').as_posix()
    platform_include = (jdk / 'include' / ('win32' if is_windows else 'linux')).as_posix()
    require((jdk / 'include/jni.h').is_file() and Path(platform_include, 'jni_md.h').is_file(), 'jdk_jni_headers_missing')
    forced_header = (('/I"' + jni_include + '" /I"' + platform_include + '" /FI"' + umbrella.as_posix() + '"')
                     if is_windows else
                     ('-I"' + jni_include + '" -I"' + platform_include + '" -include "' + umbrella.as_posix() + '"'))
    configure(tools, source / 'consumers/java_image/runtime', jni_build, work, operations, 'jni-configure',
              ('-DCMAKE_PREFIX_PATH=' + sdk.as_posix(),
               '-DMeguvizMemory_DIR=' + (sdk / 'lib/cmake/MeguvizMemory').as_posix(),
               '-DCMAKE_INSTALL_PREFIX=' + sdk.as_posix(),
               '-DCMAKE_CXX_FLAGS=' + forced_header))
    require_cached_package_dir(jni_build / 'CMakeCache.txt', sdk / 'lib/cmake/MeguvizMemory')
    require(common.file_identity(read_cmake_compiler(jni_build / 'CMakeCache.txt')) == compiler_hash,
            'host_core_jni_compiler_mismatch')
    common.run_tool([tools['cmake'], '--build', jni_build, '--parallel', str(args.jobs)], work, operations, 'jni-build')
    common.run_tool([tools['cmake'], '--install', jni_build], work, operations, 'jni-install')
    files, links = sdk_files_for_host(sdk, is_windows)
    validate_sdk_headers(source, files)
    raw_sdk_files, raw_sdk_links = dict(files), dict(links)
    pressure_name = 'sdk/lib/mgmem_pressure.lib' if is_windows else 'sdk/lib/libmgmem_pressure.a'
    pressure = sdk / pressure_name.removeprefix('sdk/')
    pressure_raw = pressure.read_bytes()
    require(files[pressure_name] == pressure_raw, 'host_static_input_changed')
    clean = work / ('mgmem_pressure-clean.lib' if is_windows else 'libmgmem_pressure-clean.a')
    common.run_tool([tools['llvm-strip'], '--strip-debug', '-o', clean, pressure], work, operations, 'pressure-strip')
    pressure_clean = clean.read_bytes()
    if is_windows:
        left = coff_archive_contract(tools, pressure, work, operations, 'pressure-original')
        right = coff_archive_contract(tools, clean, work, operations, 'pressure-clean')
        sections = common.run_tool([tools['llvm-readobj'], '--sections', clean], work, operations, 'pressure-clean-debug-sections')
        require(not re.search(r'(?m)^    Name: \.debug', sections), 'host_static_debug_remains')
    else:
        for label, archive in (('pressure-original', pressure), ('pressure-clean', clean)):
            header = common.run_tool([tools['llvm-readobj'], '--file-headers', archive], work, operations, label + '-header')
            require('Format: elf64-x86-64' in header and 'EM_X86_64' in header, 'wrong_host_static_abi')
            payloads = common.run_tool([tools['llvm-readobj'], '--sections', '--section-data', archive],
                                       work, operations, label + '-payload-completeness')
            validate_elf_allocated_section_payloads(payloads)
        left = common.static_contract(tools['llvm-readobj'], tools['llvm-nm'], tools['llvm-ar'], pressure,
                                      work, operations, 'pressure-original')
        right = common.static_contract(tools['llvm-readobj'], tools['llvm-nm'], tools['llvm-ar'], clean,
                                       work, operations, 'pressure-clean')
        sections = common.run_tool([tools['llvm-readobj'], '--sections', clean], work, operations, 'pressure-clean-debug-sections')
        require(not re.search(r'(?m)^    Name: \.(?:debug|zdebug)', sections), 'host_static_debug_remains')
    require(left == right, 'host_static_contract_changed')
    require(pressure.read_bytes() == pressure_raw and clean.read_bytes() == pressure_clean, 'host_static_changed_during_validation')
    pressure_export_count = validate_pressure_archive_exports(tools, clean, source, work, operations)
    files[pressure_name] = pressure_clean

    core = sdk / ('bin/mgmem.dll' if is_windows else 'lib/libmgmem.so.1.2.1')
    jni = sdk / ('bin/mgmem_pressure_jni.dll' if is_windows else 'lib/libmgmem_pressure_jni.so')
    core_exports = required_core_exports(source)
    expected = common.native_names(source) | {'mgmem_android_borrow_register_v1', 'mgmem_android_borrow_cancel_v1',
                                              'mgmem_android_pressure_register_v1', 'mgmem_android_pressure_unregister_v1'}
    if is_windows:
        core_contract = dynamic_windows(tools, core, work, operations, 'core', core_exports)
        jni_contract = dynamic_windows(tools, jni, work, operations, 'jni', expected)
    else:
        core_contract = dynamic_linux(tools, core, work, operations, 'core', core_exports)
        jni_contract = dynamic_linux(tools, jni, work, operations, 'jni', expected)
    validate_host_core_dependency(core_contract, jni_contract, is_windows)
    validate_host_dependencies(core_contract, is_windows, False)
    validate_host_dependencies(jni_contract, is_windows, True)
    require(common.file_identity(core) == common.ident(files['sdk/' + core.relative_to(sdk).as_posix()]) and
            common.file_identity(jni) == common.ident(files['sdk/' + jni.relative_to(sdk).as_posix()]),
            'host_dynamic_binary_changed')
    files['java/managed-api.jar'] = jar
    files['README.md'] = README
    files['NOTICE-LLVM.txt'] = (source / 'third_party_notices/llvm-NOTICE.txt').read_bytes()
    initial_archive = archive_data(files, links, is_windows)
    restored_files, restored_links = scan_host_archive(initial_archive, markers, is_windows)
    require(restored_files == files and restored_links == links, 'host_archive_roundtrip_mismatch')
    consumer = verify_installed_consumer(tools, source, records, lock_path, restored_files, restored_links,
                                         work, operations, is_windows)
    sdk_after, links_after = sdk_files_for_host(sdk, is_windows)
    require(sdk_after == raw_sdk_files and links_after == raw_sdk_links and clean.read_bytes() == pressure_clean,
            'host_sdk_changed_during_validation')
    common.verify_snapshot(source, records)
    require(all(common.file_identity(path) == tool_hashes[name] for name, path in tools.items()) and
            common.file_identity(compiler) == compiler_hash, 'host_toolchain_changed')
    common.verify_tool_log_integrity(work, operations)
    source_info = {'local_source_set_sha256': common.sha(common.canonical(records)),
                   'local_source_count': len(records), 'local_source_records': records,
                   'source_lock_sha256': common.file_identity(lock_path)['sha256']}
    report = {'schema_version': 1, 'state': 'SCOPED_HOST_ENGINEERING_CANDIDATE',
              'scope': 'SOURCE_BUILD_SANITIZED_INSTALLED_HOST_CONSUMER_ONLY',
              'platform': 'windows-x86_64' if is_windows else 'linux-x86_64',
              'source': source_info, 'toolchain': tool_hashes, 'compiler': compiler_hash,
              'managed_jar': common.ident(jar), 'core': {'identity': common.file_identity(core), **core_contract},
              'core_header_declaration_count': len(core_exports),
              'jni': {'identity': common.file_identity(jni), **jni_contract},
              'static_pressure': {'original': common.ident(pressure_raw), 'candidate': common.ident(pressure_clean), **right},
              'pressure_header_declaration_count': pressure_export_count,
              'jni_signature_check': 'JAVAC_HEADERS_FORCED_INTO_HOST_JNI_TRANSLATION_UNITS_FOR_PACKAGED_JAVA_CLASSES',
              'tool_logs': 'UNIQUE_EXCLUSIVE_AND_HASH_VERIFIED_IN_PRIVATE_WORK_DIRECTORY',
              'consumer': consumer, 'android_runtime': 'NOT_RUN', 'device_execution': 'NOT_RUN',
              'performance': 'NOT_MEASURED', 'formal_release': 'NOT_QUALIFIED',
              'limits': ['Host JNI is not an Android AAR or ART qualification.',
                         'The packaged JAR excludes Android framework callback classes.',
                         'Binary comparisons and source hashes are not signed complete build provenance.',
                         'Bounded path and identifier scans cannot prove absence of all private information.']}
    files['SOURCE.json'] = common.json_bytes(source_info)
    files['MANIFEST.json'] = common.json_bytes(report)
    package = archive_data(files, links, is_windows)
    final_files, final_links = scan_host_archive(package, markers, is_windows)
    require(final_files == files and final_links == links, 'host_final_archive_mismatch')
    return package, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--source-lock', type=Path, required=True)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--cmake', type=Path, required=True)
    parser.add_argument('--ninja', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, required=True)
    parser.add_argument('--private-identifiers-file', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    is_windows = os.name == 'nt'
    require(is_windows or sys.platform.startswith('linux'), 'unsupported_host_os')
    require(platform.machine().lower() in ('amd64', 'x86_64'), 'unsupported_host_architecture')
    require(1 <= args.jobs <= 16, 'invalid_jobs')
    require(common.ROOT.resolve() == ROOT.resolve(), 'generator_root_mismatch')
    source_root = args.source_root.resolve(strict=True)
    lock_path = args.source_lock.resolve(strict=True)
    require(source_root.is_dir() and lock_path.is_file(), 'source_or_lock_missing')
    build_root = common.workspace_directory('build')
    work = common.fresh_work_directory(args.work_dir, build_root)
    require(not source_root.is_relative_to(work) and not work.is_relative_to(source_root), 'work_overlaps_source')
    private = args.private_identifiers_file.resolve(strict=True)
    require(private.is_relative_to(build_root) and private.is_file() and not args.private_identifiers_file.is_symlink(),
            'private_markers_outside_build')
    markers = [line.strip().lower() for line in private.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    require(markers and all(len(value) >= 3 for value in markers), 'invalid_private_markers')
    dist = common.workspace_directory('dist')
    suffix = '.zip' if is_windows else '.tar.gz'
    out = common.fresh_output_path(args.output, dist, suffix, 'output_not_fresh_dist_archive')
    work.mkdir(parents=True, exist_ok=False)
    status = work / 'STATUS.json'
    status.write_bytes(common.json_bytes({'state': 'RUNNING', 'output_written': False}))
    try:
        lock_bytes, records = common.verify_lock(source_root, lock_path)
        generators = {'build_host_candidate': ROOT / 'tools/build_host_candidate.py',
                      'build_android_candidate': ROOT / 'tools/build_android_candidate.py',
                      'source_lock': ROOT / 'tools/source_lock.py',
                      'package_feature_preview': ROOT / 'tools/package_feature_preview.py'}
        common.verify_generator_bindings(records, generators)
        generator_hashes = {name: common.file_identity(path) for name, path in generators.items()}
        snapshot = work / 'source-snapshot'
        common.copy_snapshot(source_root, snapshot, records)
        package, report = build_host(args, work, snapshot, lock_path, records, markers, is_windows)
        current_lock, current_records = common.verify_lock(source_root, lock_path)
        require(current_lock == lock_bytes and current_records == records, 'host_source_changed_during_build')
        require(all(common.file_identity(path) == generator_hashes[name] for name, path in generators.items()),
                'host_generator_changed_during_build')
        (work / 'MANIFEST.public.json').write_bytes(common.json_bytes(report))
        temporary = work / ('candidate' + suffix)
        with temporary.open('xb') as stream:
            stream.write(package); stream.flush(); os.fsync(stream.fileno())
        common.publish_no_replace(temporary, out)
        require(common.file_identity(out) == common.ident(package), 'host_published_identity_mismatch')
        status.write_bytes(common.json_bytes({'state': 'PASS_SCOPED_HOST_ENGINEERING_CANDIDATE',
                                             'output_written': True, 'sha256': common.sha(package)}))
        print(json.dumps({'state': 'PASS_SCOPED_HOST_ENGINEERING_CANDIDATE',
                          'platform': report['platform'], 'bytes': len(package),
                          'sha256': common.sha(package), 'android_runtime': 'NOT_RUN'}))
        return 0
    except BaseException as error:
        reason = str(error)
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', reason): reason = type(error).__name__
        status.write_bytes(common.json_bytes({'state': 'FAILED', 'reason': reason, 'output_written': out.exists()}))
        print(json.dumps({'state': 'FAILED', 'reason': reason, 'work_preserved': True}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        reason = str(error)
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', reason): reason = type(error).__name__
        print(json.dumps({'state': 'PREFLIGHT_FAILED', 'reason': reason, 'work_preserved': False}), file=sys.stderr)
        raise SystemExit(2)
