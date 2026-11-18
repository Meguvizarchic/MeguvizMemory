#!/usr/bin/env python3
"""Build a source-locked Android engineering candidate, without ADB or signing.

This tool builds from a verified private copy of a caller-selected source
snapshot. Raw build outputs and logs remain in the fresh work directory; only
recursively scanned, checked artifacts are atomically linked into dist.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import sys
import zipfile

from package_feature_preview import Scanner, archive_members
import package_feature_preview
import source_lock
from source_lock import LOCAL_DIRECTORIES, scoped_files

ROOT = Path(__file__).resolve().parents[1]
ABIS = {'arm64-v8a': (2, 183), 'x86_64': (2, 62), 'armeabi-v7a': (1, 40)}
JAVA_BASE = 'adapters/android/java/io/github/jichuo1/meguviz/memory/android/'
JAVA_PREFIX = 'Java_io_github_jichuo1_meguviz_memory_android_'
REQUIRED_JAVA = ('NativeMemoryLease', 'NativePressureBackend', 'ManagedMemoryDomain', 'ManagedPressureBackend', 'MemoryPressureAdapter')
README = b'''# MeguvizMemory Android engineering candidate\n\nThree independently built single-ABI AARs and native SDKs share the source\nsnapshot identified by SOURCE.json. These are engineering artifacts, not a\nformal release. The SDK and AAR for each ABI contain the same shared core.\nNo APK, DEX, signing key, raw build log, or original binary is included.\n\nStatic checks and bounded privacy scans do not prove behavior, complete build\nprovenance, or absence of all secrets. Android ART, R8 runtime, real devices,\n16 KiB loading, MTE, performance, energy and BIL integration are not qualified.\n'''


def require(value, reason):
    if not value:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ident(data):
    return {'bytes': len(data), 'sha256': sha(data)}


def file_identity(path):
    return ident(path.read_bytes())


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode('utf-8')


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf-8')


def safe_relative(name):
    require(isinstance(name, str) and name and '\\' not in name and not name.startswith('/'), 'invalid_source_path')
    value = PurePosixPath(name)
    require('..' not in value.parts and '.' not in value.parts and not re.match(r'^[A-Za-z]:', name), 'invalid_source_path')
    return value


def safe_file(root, name):
    pure = safe_relative(name)
    current = root
    for part in pure.parts:
        current = current / part
        require(not current.is_symlink(), 'source_symlink')
    real = current.resolve(strict=True)
    require(real.is_relative_to(root.resolve()) and real.is_file(), 'source_file_missing')
    return real


def verify_lock(root, lock_path):
    raw = lock_path.read_bytes()
    data = json.loads(raw.decode('utf-8-sig'))
    require(data.get('schema_version') == 1 and data.get('scope') == 'SCOPED_SOURCE_TREE_LOCK', 'source_lock_scope')
    records = data.get('files')
    require(isinstance(records, list) and data.get('file_count') == len(records), 'source_lock_count')
    require(sha(canonical(records)) == data.get('source_set_sha256'), 'source_lock_hash')
    require(all(isinstance(item, dict) and item.get('root') in ('MeguvizMemory', 'DexMeguviz') for item in records), 'source_lock_unknown_root')
    local = [item for item in records if item.get('root') == 'MeguvizMemory']
    seen = {}
    for item in local:
        name = item.get('path'); safe_relative(name)
        require(name not in seen, 'duplicate_source_path')
        expected = {'bytes': item.get('bytes'), 'sha256': item.get('sha256')}
        require(file_identity(safe_file(root, name)) == expected, 'source_lock_mismatch')
        seen[name] = expected
    actual = {p.relative_to(root).as_posix() for p in scoped_files(root, LOCAL_DIRECTORIES, lock_path.resolve())}
    require(set(seen) == actual, 'source_set_mismatch')
    required = {'CMakeLists.txt', 'src/mgmem.cpp', 'cmake/MeguvizMemoryConfig.cmake.in',
                'adapters/android/CMakeLists.txt', 'adapters/android/native/pressure.cpp',
                'adapters/android/native/pressure_jni.cpp', 'adapters/android/native/borrow_jni.cpp',
                'adapters/android/native/workspace_jni.cpp', 'adapters/android/consumer-rules.pro',
                'third_party_notices/llvm-NOTICE.txt'}
    required.update(JAVA_BASE + n + '.java' for n in REQUIRED_JAVA)
    require(required <= set(seen), 'required_source_missing')
    return raw, seen


def copy_snapshot(source, target, records):
    target.mkdir(parents=True, exist_ok=False)
    for name, expected in sorted(records.items()):
        data = safe_file(source, name).read_bytes()
        require(ident(data) == expected, 'source_changed_during_copy')
        destination = target.joinpath(*PurePosixPath(name).parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('xb') as out:
            out.write(data)
    verify_snapshot(target, records)


def verify_snapshot(root, records):
    for name, expected in records.items():
        require(file_identity(safe_file(root, name)) == expected, 'snapshot_changed')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    require(actual == set(records), 'snapshot_set_changed')


def zip_bytes(files):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, payload in sorted(files.items()):
            safe_relative(name)
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, payload)
    result = out.getvalue()
    require(archive_members(result) == files, 'archive_roundtrip_mismatch')
    return result


def elf_parts(data, abi):
    klass, machine = ABIS[abi]
    require(len(data) >= 64 and data[:4] == b'\x7fELF' and data[4] == klass and data[5] == 1, 'invalid_elf')
    require(struct.unpack_from('<H', data, 16)[0] == 3 and struct.unpack_from('<H', data, 18)[0] == machine, 'elf_wrong_abi')
    if klass == 2:
        phoff = struct.unpack_from('<Q', data, 32)[0]; phentsize, phnum = struct.unpack_from('<HH', data, 54)
        shoff = struct.unpack_from('<Q', data, 40)[0]; shentsize, shnum, shstr = struct.unpack_from('<HHH', data, 58)
        phfmt, shfmt, mask = '<IIQQQQQQ', '<IIQQQQIIQQ', ((40, 48), (58, 64))
    else:
        phoff = struct.unpack_from('<I', data, 28)[0]; phentsize, phnum = struct.unpack_from('<HH', data, 42)
        shoff = struct.unpack_from('<I', data, 32)[0]; shentsize, shnum, shstr = struct.unpack_from('<HHH', data, 46)
        phfmt, shfmt, mask = '<IIIIIIII', '<IIIIIIIIII', ((32, 36), (46, 52))
    require(phentsize == struct.calcsize(phfmt) and shentsize == struct.calcsize(shfmt) and
            0 < phnum < 256 and 0 < shnum < 65536 and shstr < shnum and
            phoff + phnum * phentsize <= len(data) and shoff + shnum * shentsize <= len(data), 'elf_bad_tables')
    sections = [struct.unpack_from(shfmt, data, shoff + i * shentsize) for i in range(shnum)]
    table = sections[shstr]; require(table[4] + table[5] <= len(data), 'elf_bad_names')
    names = data[table[4]:table[4] + table[5]]
    loads, ranges = [], []
    for i in range(phnum):
        fields = struct.unpack_from(phfmt, data, phoff + i * phentsize)
        if fields[0] != 1: continue
        if klass == 2: _, flags, off, va, pa, size, mem, align = fields
        else: _, off, va, pa, size, mem, flags, align = fields
        require(off + size <= len(data) and size <= mem, 'elf_bad_load')
        require(align >= 16384 and (align & (align - 1)) == 0 and (va - off) % align == 0,
                'elf_not_16k_load_aligned')
        payload = bytearray(data[off:off + size])
        for start, end in mask:
            for pos in range(max(start, off), min(end, off + size)): payload[pos - off] = 0
        loads.append({'fields': [flags, off, va, pa, size, mem, align], 'sha256': sha(payload)})
        ranges.append((off, off + size))
    require(loads, 'elf_load_missing')
    section_rows = []
    for item in sections:
        offset = item[0]; require(offset < len(names), 'elf_bad_name_offset')
        end = names.find(b'\0', offset); require(end >= 0, 'elf_bad_name')
        section_rows.append({'name': names[offset:end].decode('ascii', 'replace'),
                             'type': item[1], 'flags': item[2], 'offset': item[4], 'size': item[5]})
    return {'class': klass, 'machine': machine, 'loads': loads}, section_rows, ranges


def verify_stripped_sections(before, after, abi):
    original, sections, ranges = elf_parts(before, abi)
    candidate, after_sections, _ = elf_parts(after, abi)
    require(original == candidate, 'elf_load_changed')
    removed = [s for s in sections if s['name'] == '.comment' or s['name'].startswith(('.debug', '.zdebug'))]
    for section in removed:
        require(not section['flags'] & 2 and section['type'] != 8, 'stripped_section_allocated')
        start, end = section['offset'], section['offset'] + section['size']
        require(end <= len(before) and all(not (start < b and a < end) for a, b in ranges), 'stripped_section_loaded')
    require(not any(s['name'] == '.comment' or s['name'].startswith(('.debug', '.zdebug')) for s in after_sections), 'debug_section_remains')
    return original


_TOOL_JOURNAL_IDENTITIES = {}


def _journal_file_identity(info):
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_ino != 0,
            'tool_journal_not_private_regular_file')
    return info.st_dev, info.st_ino


def _reserve_tool_journal(work, operations):
    """Create once for this run; never append to another run's journal."""
    key = work.resolve()
    journal = work / 'operations.jsonl'
    if not operations:
        require(key not in _TOOL_JOURNAL_IDENTITIES, 'tool_journal_already_started')
        try:
            with journal.open('xb') as stream:
                identity = _journal_file_identity(os.fstat(stream.fileno()))
                stream.flush(); os.fsync(stream.fileno())
        except FileExistsError as error:
            raise ValueError('tool_journal_exists') from error
        require(not journal.is_symlink() and _journal_file_identity(journal.lstat()) == identity,
                'tool_journal_identity_changed')
        _TOOL_JOURNAL_IDENTITIES[key] = identity
    else:
        require(key in _TOOL_JOURNAL_IDENTITIES and not journal.is_symlink() and journal.is_file() and
                _journal_file_identity(journal.lstat()) == _TOOL_JOURNAL_IDENTITIES[key],
                'tool_journal_identity_changed')
    return journal, _TOOL_JOURNAL_IDENTITIES[key]


def _append_tool_journal(journal, identity, record):
    require(not journal.is_symlink() and journal.is_file() and _journal_file_identity(journal.lstat()) == identity,
            'tool_journal_identity_changed')
    flags = os.O_WRONLY | os.O_APPEND | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    try:
        fd = os.open(journal, flags)
    except OSError as error:
        raise ValueError('tool_journal_open_failed') from error
    with os.fdopen(fd, 'ab', buffering=0) as stream:
        require(_journal_file_identity(os.fstat(stream.fileno())) == identity and not journal.is_symlink() and
                _journal_file_identity(journal.lstat()) == identity, 'tool_journal_identity_changed')
        stream.write(canonical(record) + b'\n'); os.fsync(stream.fileno())


def run_tool(command, work, operations, label, timeout=600, expected_error=None):
    require(isinstance(label, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,160}', label) is not None,
            'invalid_tool_step')
    require(not any(record.get('step', '').casefold() == label.casefold() for record in operations),
            'duplicate_tool_step_' + label)
    require(work.is_dir() and not work.is_symlink(), 'tool_work_dir_invalid')
    log = work / 'logs'
    if os.path.lexists(log):
        require(log.is_dir() and not log.is_symlink() and log.resolve().parent == work.resolve(), 'tool_log_dir_invalid')
    stdout_path, stderr_path = log / (label + '.stdout'), log / (label + '.stderr')
    names = {entry.name.casefold() for entry in log.iterdir()} if log.is_dir() else set()
    require(stdout_path.name.casefold() not in names and stderr_path.name.casefold() not in names,
            'tool_log_exists_' + label)
    journal, journal_identity = _reserve_tool_journal(work, operations)
    log.mkdir(exist_ok=True)
    require(log.is_dir() and not log.is_symlink() and log.resolve().parent == work.resolve(), 'tool_log_dir_invalid')
    # Reserve both names exclusively before executing the step. A collision or
    # write failure never overwrites evidence and never reports a successful run.
    with stdout_path.open('xb') as stdout_file, stderr_path.open('xb') as stderr_file:
        try:
            completed = subprocess.run([str(x) for x in command], capture_output=True, timeout=timeout, check=False)
            stdout, stderr, code, state = completed.stdout, completed.stderr, completed.returncode, 'EXITED'
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code, state = error.stdout or b'', error.stderr or b'', None, 'TIMEOUT'
        except OSError:
            stdout, stderr, code, state = b'', b'', None, 'LAUNCH_ERROR'
        stdout_file.write(stdout); stdout_file.flush(); os.fsync(stdout_file.fileno())
        stderr_file.write(stderr); stderr_file.flush(); os.fsync(stderr_file.fileno())
    record = {'step': label, 'state': state, 'exit_code': code,
              'stdout_sha256': sha(stdout), 'stderr_sha256': sha(stderr)}
    operations.append(record)
    _append_tool_journal(journal, journal_identity, record)
    require(state != 'TIMEOUT', 'tool_timeout_' + label)
    require(state != 'LAUNCH_ERROR', 'tool_launch_error_' + label)
    if expected_error is None:
        require(code == 0, 'tool_failed_' + label)
    else:
        require(code != 0 and expected_error.encode('utf-8') in stdout + stderr,
                'expected_rejection_missing_' + label)
    return stdout.decode('utf-8', 'strict')


def verify_tool_log_integrity(work, operations):
    """Verify the private journal and every step log before publication."""
    require(operations, 'tool_journal_empty')
    labels = [record.get('step') for record in operations]
    require(all(isinstance(label, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,160}', label)
                for label in labels) and len({label.casefold() for label in labels}) == len(labels),
            'tool_journal_duplicate_or_invalid_step')
    journal = work / 'operations.jsonl'
    require(work.resolve() in _TOOL_JOURNAL_IDENTITIES and journal.is_file() and not journal.is_symlink() and
            _journal_file_identity(journal.lstat()) == _TOOL_JOURNAL_IDENTITIES[work.resolve()], 'tool_journal_missing')
    rows = [json.loads(line) for line in journal.read_text(encoding='utf-8').splitlines()]
    require(rows == operations, 'tool_journal_record_mismatch')
    log = work / 'logs'
    require(log.is_dir() and not log.is_symlink() and log.resolve().parent == work.resolve(), 'tool_log_dir_invalid')
    expected = {label + '.' + stream for label in labels for stream in ('stdout', 'stderr')}
    actual = {path.name for path in log.iterdir()}
    require(actual == expected, 'tool_log_set_mismatch')
    for record in operations:
        for stream in ('stdout', 'stderr'):
            path = log / (record['step'] + '.' + stream)
            require(path.is_file() and not path.is_symlink() and sha(path.read_bytes()) == record[stream + '_sha256'],
                    'tool_log_hash_mismatch_' + record['step'])


def dynamic_contract(readelf, path, work, operations, label):
    symbols = run_tool([readelf, '--dyn-syms', '--wide', path], work, operations, label + '-symbols')
    dynamic = run_tool([readelf, '--dynamic', '--wide', path], work, operations, label + '-dynamic')
    reloc = run_tool([readelf, '--relocations', '--wide', path], work, operations, label + '-relocations')
    normalized = lambda text: '\n'.join(sorted(x.strip() for x in text.splitlines() if x.strip() and not x.startswith(('File:', 'Archive:')))).encode()
    needed = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic)
    soname = re.findall(r'\(SONAME\).*?\[([^\]]+)\]', dynamic)
    require(all('/' not in x and '\\' not in x for x in needed), 'invalid_needed')
    return {'symbols_sha256': sha(normalized(symbols)), 'dynamic_sha256': sha(normalized(dynamic)),
            'relocations_sha256': sha(normalized(reloc)), 'needed': needed, 'soname': soname}, symbols


def validate_static_section_payload(kind, size, payload):
    # NOBITS occupies virtual storage only; every other selected section must
    # actually contain exactly its advertised bytes in the readobj output.
    if kind == 'SHT_NOBITS':
        require(not payload, 'static_nobits_has_payload')
    else:
        require(len(payload) == size, 'static_section_size')


def static_contract(readobj, nm, ar, path, work, operations, label):
    members = run_tool([ar, 't', path], work, operations, label + '-members').splitlines()
    require(len(members) == 1 and len(set(members)) == len(members), 'static_archive_members')
    sections = run_tool([readobj, '--sections', '--section-data', path], work, operations, label + '-sections')
    blocks = re.findall(r'(?ms)^  Section \{\n(.*?)^  \}', sections)
    rows = []
    for block in blocks:
        match = re.search(r'(?m)^    Name: (\S+)', block)
        if not match: continue
        name = match.group(1)
        if 'SHF_ALLOC' not in block and name != '.ARM.attributes': continue
        size = int(re.search(r'(?m)^    Size: (\d+)', block).group(1))
        body = re.search(r'(?ms)^    SectionData \(\n(.*?)^    \)', block)
        pieces = [''.join(re.findall(r'\b[0-9A-F]{2,8}\b', line.split('|')[0].split(':', 1)[1]))
                  for line in body.group(1).splitlines() if ':' in line] if body else []
        payload = bytes.fromhex(''.join(pieces))
        typ = re.search(r'(?m)^    Type: (\S+)', block).group(1)
        validate_static_section_payload(typ, size, payload)
        flags = re.search(r'(?m)^    Flags \[ \((0x[0-9A-Fa-f]+)\)', block).group(1)
        align = int(re.search(r'(?m)^    AddressAlignment: (\d+)', block).group(1))
        rows.append([name, typ, flags, size, align, sha(payload)])
    require(rows, 'static_allocated_sections_missing')
    reloc = run_tool([readobj, '--relocations', path], work, operations, label + '-relocations')
    groups = re.findall(r'(?ms)^  Section \(\d+\) (\S+) \{\n(.*?)^  \}', reloc)
    relocs = [[name, [x.strip() for x in body.splitlines() if x.strip()]] for name, body in groups if '.debug' not in name]
    require(relocs, 'static_relocations_missing')
    symbols = run_tool([nm, '--defined-only', '--extern-only', '--format=posix', path], work, operations, label + '-symbols')
    defined = sorted(x.strip() for x in symbols.splitlines() if x.strip() and not x.rstrip().endswith(':'))
    require(not any('mgmem_pressure_test_' in line or 'managed_test_' in line for line in defined), 'test_static_export')
    return {'members': members, 'sections_sha256': sha(canonical(rows)), 'relocations_sha256': sha(canonical(relocs)),
            'symbols_sha256': sha(canonical(defined))}


def verify_static_debug_absent(readobj, path, work, operations, label):
    text = run_tool([readobj, '--sections', path], work, operations, label + '-sections')
    names = re.findall(r'(?m)^\s+Name:\s+(\S+)', text)
    require(names and not any(name == '.comment' or name.startswith(('.debug', '.zdebug')) for name in names),
            'static_debug_section_remains')


def native_names(source):
    names = set()
    for klass in ('NativeMemoryLease', 'NativePressureBackend', 'ManagedMemoryDomain'):
        text = (source / (JAVA_BASE + klass + '.java')).read_text(encoding='utf-8')
        methods = re.findall(r'\bprivate\s+static\s+native\s+[\w\[\]]+\s+(\w+)\s*\(', text)
        require(methods and len(methods) == len(set(methods)), 'java_native_declarations')
        require(all('_' not in name for name in methods), 'unsupported_jni_name_escape')
        names.update(JAVA_PREFIX + klass + '_' + name for name in methods)
    require(any(name.endswith('_nativeAttachPressure') for name in names), 'pressure_binding_missing')
    return names


def validate_jni_symbols(symbols, required):
    rows = re.findall(r'(?m)^\s*\d+:\s+[0-9a-fA-F]+\s+\d+\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)', symbols)
    require(rows, 'jni_symbol_table_unparseable')
    found = {name.split('@', 1)[0] for kind, binding, visibility, index, name in rows
             if kind == 'FUNC' and binding == 'GLOBAL' and visibility == 'DEFAULT' and index != 'UND'}
    require(required <= found, 'jni_export_missing')
    require(not any(('test_' in name.lower() or 'testing' in name.lower()) and name.startswith(JAVA_PREFIX)
                    for name in found), 'test_jni_export')
    require(not any(name.startswith(('mgmem_pressure_test_', 'managed_test_')) for name in found), 'test_native_export')
    require(not any(name.startswith('mgmem_pressure_') and index == 'UND' for _, _, _, index, name in rows),
            'unresolved_pressure_symbol')


def validate_keep_rules(rules):
    for name in (*REQUIRED_JAVA, 'ManagedMemoryDomain$Allocation', 'ManagedMemoryDomain$Lease',
                 'ManagedMemoryDomain$Pressure', 'ManagedMemoryDomain$ManagedException'):
        if name == 'MemoryPressureAdapter': continue
        expected = ('-keep class io.github.jichuo1.meguviz.memory.android.' + name + ' { *; }').encode()
        require(expected in rules, 'keep_rule_missing')


def validate_sdk_members(files, headers):
    required = {'include/mgmem/' + name for name in headers}
    required.update({'lib/libmgmem.so', 'lib/libmgmem_pressure.a', 'lib/libmgmem_pressure_jni.so',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryConfig.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryConfigVersion.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryTargets.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryTargets-release.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniABI.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets.cmake',
                     'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets-release.cmake',
                     'share/MeguvizMemory/licenses/llvm-NOTICE.txt',
                     'share/MeguvizMemory/licenses/LICENSE',
                     'share/MeguvizMemory/licenses/NOTICE'})
    require(set(files) == required, 'sdk_unexpected_or_missing_member')


def validate_output_identities(aar, sdk_zip, abi, comparison):
    aar_members, sdk_members = archive_members(aar), archive_members(sdk_zip)
    for name, label in (('libmgmem.so', 'core'), ('libmgmem_pressure_jni.so', 'jni')):
        aar_value = aar_members['jni/' + abi + '/' + name]
        sdk_value = sdk_members['lib/' + name]
        require(aar_value == sdk_value and ident(aar_value) == comparison[label]['candidate'],
                'sdk_aar_' + label + '_mismatch')
    require(ident(sdk_members['lib/libmgmem_pressure.a']) == comparison['static']['candidate'], 'sdk_static_mismatch')


def write_jni_consumer_fixture(work):
    """Small native consumer for package-target configure/link checks only."""
    fixture = work / 'android-jni-target-consumer'
    fixture.mkdir()
    (fixture / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(MeguvizMemoryJniTargetProbe LANGUAGES CXX)
if(PREDEFINED_JNI)
  add_library(MeguvizMemory::android_jni SHARED IMPORTED)
  set_target_properties(MeguvizMemory::android_jni PROPERTIES
    IMPORTED_CONFIGURATIONS RELEASE IMPORTED_LOCATION_RELEASE "${PREDEFINED_JNI}"
    INTERFACE_LINK_LIBRARIES "MeguvizMemory::mgmem")
endif()
if(REQUIRE_JNI)
  find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  if(RECHECK_SAME)
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_CORE_CONFIGURATION)
    set_property(TARGET MeguvizMemory::mgmem APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(MeguvizMemory::mgmem PROPERTIES IMPORTED_LOCATION_DEBUG "${OTHER_CORE_CONFIGURATION}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_JNI_CONFIGURATION)
    set_property(TARGET MeguvizMemory::android_jni APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(MeguvizMemory::android_jni PROPERTIES IMPORTED_LOCATION_DEBUG "${OTHER_JNI_CONFIGURATION}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_CORE_DIRECT)
    set_target_properties(MeguvizMemory::mgmem PROPERTIES IMPORTED_LOCATION_DEBUG "${OTHER_CORE_DIRECT}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_JNI_DIRECT)
    set_target_properties(MeguvizMemory::android_jni PROPERTIES IMPORTED_LOCATION_DEBUG "${OTHER_JNI_DIRECT}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_CORE_MAPPING)
    set_target_properties(MeguvizMemory::mgmem PROPERTIES MAP_IMPORTED_CONFIG_DEBUG OTHER
      IMPORTED_LOCATION_OTHER "${OTHER_CORE_MAPPING}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_JNI_MAPPING)
    set_target_properties(MeguvizMemory::android_jni PROPERTIES MAP_IMPORTED_CONFIG_DEBUG OTHER
      IMPORTED_LOCATION_OTHER "${OTHER_JNI_MAPPING}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(OTHER_PACKAGE_DIR)
    set(MeguvizMemory_DIR "${OTHER_PACKAGE_DIR}")
    find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
  endif()
  if(NOT TARGET MeguvizMemory::android_jni)
    message(FATAL_ERROR "jni_target_missing")
  endif()
  get_target_property(_links MeguvizMemory::android_jni INTERFACE_LINK_LIBRARIES)
  if(NOT "MeguvizMemory::mgmem" IN_LIST _links)
    message(FATAL_ERROR "jni_target_does_not_propagate_shared_core")
  endif()
  get_target_property(_jni MeguvizMemory::android_jni IMPORTED_LOCATION_RELEASE)
  get_target_property(_core MeguvizMemory::mgmem IMPORTED_LOCATION_RELEASE)
  if(NOT _jni OR NOT _core)
    message(FATAL_ERROR "imported_binary_location_missing")
  endif()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/imported-paths.txt" "${_jni}\\n${_core}\\n")
  add_library(jni_target_probe SHARED probe.cpp)
  target_link_libraries(jni_target_probe PRIVATE MeguvizMemory::android_jni)
  target_link_options(jni_target_probe PRIVATE -Wl,--no-undefined)
else()
  find_package(MeguvizMemory CONFIG REQUIRED)
  if(TARGET MeguvizMemory::android_jni)
    message(FATAL_ERROR "jni_target_unexpected")
  endif()
endif()
''', encoding='utf-8', newline='\n')
    (fixture / 'probe.cpp').write_text('''#include <mgmem/android_borrow_jni.h>
extern "C" mgmem_status probe(mgmem_domain *domain, uint64_t id) {
    mgmem_status status = mgmem_android_borrow_cancel_v1(id);
    return status == MGMEM_OK ? mgmem_domain_begin_close_v1(domain) : status;
}
''', encoding='utf-8', newline='\n')
    return fixture


def unpack_sdk_for_target_check(sdk_zip, destination):
    """Use the packaged bytes, not the pre-sanitization installation."""
    destination.mkdir()
    for name, data in archive_members(sdk_zip).items():
        pure = safe_relative(name)
        path = destination.joinpath(*pure.parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(data)


def verify_generator_bindings(records, files):
    for name, path in files.items():
        expected = records.get('tools/' + name + '.py')
        require(expected is not None and file_identity(path) == expected, 'generator_source_mismatch')


def publish_no_replace(source, destination):
    """Move a self-created candidate atomically without aliasing or replacement."""
    if os.name == 'nt':
        # Python uses MoveFile on Windows; an existing destination is an error.
        os.rename(source, destination)
        return
    require(sys.platform.startswith('linux'), 'atomic_noreplace_unsupported')
    libc = ctypes.CDLL(None, use_errno=True)
    rename = getattr(libc, 'renameat2', None)
    require(rename is not None, 'atomic_noreplace_unsupported')
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1) != 0:
        raise OSError(ctypes.get_errno(), 'atomic_noreplace_failed')


def workspace_directory(name):
    require(name in ('build', 'dist'), 'workspace_directory_invalid')
    path = ROOT / name
    path.mkdir(exist_ok=True)
    resolved = path.resolve(strict=True)
    require(resolved.is_relative_to(ROOT.resolve()), name + '_root_outside_workspace')
    return resolved


def fresh_work_directory(requested, build_root):
    # Check the caller's literal name before resolving: a dangling symlink is
    # still an existing entry, even if its resolved destination does not exist.
    raw = requested.absolute()
    require(not os.path.lexists(raw), 'work_dir_not_fresh_build_child')
    resolved = raw.resolve()
    require(resolved != build_root and resolved.is_relative_to(build_root) and not os.path.lexists(resolved),
            'work_dir_not_fresh_build_child')
    return resolved


def fresh_output_path(requested, dist, suffix, reason):
    raw = requested.absolute()
    require(not os.path.lexists(raw), reason)
    resolved = raw.resolve()
    require(resolved.parent == dist and resolved.name == raw.name and resolved.name.lower().endswith(suffix)
            and not os.path.lexists(resolved), reason)
    return resolved


def toolchain(args):
    ndk = args.ndk.resolve(strict=True)
    host = 'windows-x86_64' if os.name == 'nt' else ('darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64')
    llvm = ndk / 'toolchains/llvm/prebuilt' / host / 'bin'
    suffix = '.exe' if os.name == 'nt' else ''
    values = {'cmake': args.cmake.resolve(strict=True), 'ninja': args.ninja.resolve(strict=True),
              'javac': (args.jdk / 'bin' / ('javac' + suffix)).resolve(strict=True),
              'javap': (args.jdk / 'bin' / ('javap' + suffix)).resolve(strict=True),
              'android_jar': args.android_jar.resolve(strict=True),
              'android_toolchain': (ndk / 'build/cmake/android.toolchain.cmake').resolve(strict=True)}
    for name in ('llvm-strip', 'llvm-readelf', 'llvm-readobj', 'llvm-nm', 'llvm-ar', 'clang++', 'ld.lld'):
        values[name] = (llvm / (name + suffix)).resolve(strict=True)
    require(all(path.is_file() for path in values.values()), 'toolchain_file_missing')
    return values


def build_candidate(args, work, source, lock_bytes, records, markers):
    operations = []
    tools = toolchain(args)
    tool_hashes = {name: file_identity(path) for name, path in tools.items()}
    generator_files = {'build_android_candidate': Path(__file__).resolve(),
                       'package_feature_preview': Path(package_feature_preview.__file__).resolve(),
                       'source_lock': Path(source_lock.__file__).resolve()}
    verify_generator_bindings(records, generator_files)
    generator_hashes = {name: file_identity(path) for name, path in generator_files.items()}
    expected_jni = native_names(source)
    java_sources = sorted((source / 'adapters/android/java').rglob('*.java'))
    require(java_sources and all(not p.is_symlink() for p in java_sources), 'java_sources_invalid')
    classes = work / 'classes'; classes.mkdir()
    native_headers = work / 'jni-headers'; native_headers.mkdir()
    run_tool([tools['javac'], '--release', '8', '-g:none', '-classpath', tools['android_jar'], '-d', classes,
              '-h', native_headers,
              *java_sources], work, operations, 'compile-java')
    class_files = {p.relative_to(classes).as_posix(): p.read_bytes() for p in classes.rglob('*') if p.is_file()}
    base = 'io/github/jichuo1/meguviz/memory/android/'
    required_classes = {base + name + '.class' for name in REQUIRED_JAVA}
    required_classes.update(base + 'ManagedMemoryDomain$' + name + '.class' for name in ('Allocation', 'Lease', 'Pressure', 'ManagedException'))
    require(required_classes <= class_files.keys(), 'required_class_missing')
    header_names = ['io_github_jichuo1_meguviz_memory_android_' + name + '.h'
                    for name in ('NativeMemoryLease', 'NativePressureBackend', 'ManagedMemoryDomain')]
    require(all((native_headers / name).is_file() for name in header_names), 'jni_generated_header_missing')
    umbrella = native_headers / 'all-jni-generated.h'
    umbrella.write_text(''.join('#include "' + name + '"\n' for name in header_names), encoding='ascii', newline='\n')
    for klass in ('NativeMemoryLease', 'NativePressureBackend', 'ManagedMemoryDomain'):
        output = run_tool([tools['javap'], '-private', '-classpath', classes,
                           'io.github.jichuo1.meguviz.memory.android.' + klass], work, operations, 'javap-' + klass)
        methods = re.findall(r'\bprivate\s+static\s+native\s+[\w\[\]]+\s+(\w+)\s*\(', output)
        expected = {name[len(JAVA_PREFIX + klass + '_'):] for name in expected_jni
                    if name.startswith(JAVA_PREFIX + klass + '_')}
        require(set(methods) == expected and len(methods) == len(expected), 'class_native_mismatch')
    classes_jar = zip_bytes(class_files)
    require(archive_members(classes_jar) == class_files, 'class_jar_mismatch')
    rules = (source / 'adapters/android/consumer-rules.pro').read_bytes()
    notice = (source / 'third_party_notices/llvm-NOTICE.txt').read_bytes()
    validate_keep_rules(rules)
    require(notice, 'llvm_notice_missing')
    manifest_xml = (b'<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                    b'package="io.github.jichuo1.meguviz.memory.android"><uses-sdk android:minSdkVersion="27" /></manifest>\n')
    payloads = {}
    results = {}
    jni_target_fixture = write_jni_consumer_fixture(work)
    for abi in ABIS:
        core_build = work / ('core-build-' + abi); sdk = work / ('sdk-' + abi)
        jni_build = work / ('jni-build-' + abi)
        common = ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(tools['ninja']), '-DCMAKE_BUILD_TYPE=Release',
                  '-DCMAKE_TOOLCHAIN_FILE=' + str(tools['android_toolchain']), '-DANDROID_ABI=' + abi,
                  '-DANDROID_PLATFORM=android-27', '-DANDROID_STL=c++_static']
        run_tool([tools['cmake'], '-S', source, '-B', core_build, *common, '-DBUILD_SHARED_LIBS=ON',
                  '-DMGMEM_BUILD_TESTS=OFF', '-DMGMEM_BUILD_BENCHMARKS=OFF', '-DCMAKE_INSTALL_PREFIX=' + str(sdk)],
                 work, operations, abi + '-core-configure')
        run_tool([tools['cmake'], '--build', core_build, '--parallel', str(args.jobs)], work, operations, abi + '-core-build')
        run_tool([tools['cmake'], '--install', core_build], work, operations, abi + '-core-install')
        core_only_dir = sdk / 'lib/cmake/MeguvizMemory'
        for require_jni in (False, True):
            label = abi + ('-core-only-required-jni' if require_jni else '-core-only-no-jni')
            command = [tools['cmake'], '-S', jni_target_fixture, '-B', work / label, *common,
                       '-DMeguvizMemory_DIR=' + core_only_dir.as_posix(),
                       '-DREQUIRE_JNI=' + ('ON' if require_jni else 'OFF')]
            run_tool(command, work, operations, label,
                     expected_error='Android JNI is not installed for the requested Android ABI' if require_jni else None)
        run_tool([tools['cmake'], '-S', source / 'adapters/android', '-B', jni_build, *common,
                  '-DMeguvizMemory_DIR=' + (sdk / 'lib/cmake/MeguvizMemory').as_posix(),
                  '-DCMAKE_CXX_FLAGS=-include "' + umbrella.as_posix() + '"',
                  '-DCMAKE_INSTALL_PREFIX=' + str(sdk)],
                 work, operations, abi + '-jni-configure')
        run_tool([tools['cmake'], '--build', jni_build, '--parallel', str(args.jobs)], work, operations, abi + '-jni-build')
        run_tool([tools['cmake'], '--install', jni_build], work, operations, abi + '-jni-install')
        original_core = sdk / 'lib/libmgmem.so'; original_static = sdk / 'lib/libmgmem_pressure.a'
        original_jni = sdk / 'lib/libmgmem_pressure_jni.so'
        require(all(path.is_file() and not path.is_symlink() for path in (original_core, original_static, original_jni)), 'built_binary_missing')
        clean = work / ('clean-' + abi); clean.mkdir()
        clean_core = clean / 'libmgmem.so'; clean_jni = clean / 'libmgmem_pressure_jni.so'; clean_static = clean / 'libmgmem_pressure.a'
        for label, before, after in (('core', original_core, clean_core), ('jni', original_jni, clean_jni),
                                     ('static', original_static, clean_static)):
            run_tool([tools['llvm-strip'], '--strip-debug', '--remove-section=.comment', '-o', after, before],
                     work, operations, abi + '-' + label + '-strip')
        comparison = {}
        sanitized = {}
        for label, before, after in (('core', original_core, clean_core), ('jni', original_jni, clean_jni)):
            before_bytes, after_bytes = before.read_bytes(), after.read_bytes()
            load = verify_stripped_sections(before_bytes, after_bytes, abi)
            first, _ = dynamic_contract(tools['llvm-readelf'], before, work, operations, abi + '-' + label + '-original')
            second, symbols = dynamic_contract(tools['llvm-readelf'], after, work, operations, abi + '-' + label + '-sanitized')
            require(first == second, 'elf_dynamic_or_relocations_changed')
            allowed_system = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libandroid.so'}
            if label == 'jni':
                require(second['needed'].count('libmgmem.so') == 1, 'jni_core_dependency')
                require(set(second['needed']) <= allowed_system | {'libmgmem.so'}, 'jni_unexpected_dependency')
                validate_jni_symbols(symbols, expected_jni)
            else: require(set(second['needed']) <= allowed_system, 'core_unexpected_dependency')
            require(file_identity(before) == ident(before_bytes) and file_identity(after) == ident(after_bytes),
                    'elf_changed_during_validation')
            sanitized[label] = after_bytes
            comparison[label] = {'original': ident(before_bytes), 'candidate': ident(after_bytes), 'load': load, **second}
        static_original, static_clean = original_static.read_bytes(), clean_static.read_bytes()
        first = static_contract(tools['llvm-readobj'], tools['llvm-nm'], tools['llvm-ar'], original_static, work, operations, abi + '-static-original')
        second = static_contract(tools['llvm-readobj'], tools['llvm-nm'], tools['llvm-ar'], clean_static, work, operations, abi + '-static-sanitized')
        require(first == second, 'static_contract_changed')
        verify_static_debug_absent(tools['llvm-readobj'], clean_static, work, operations, abi + '-static-clean')
        require(file_identity(original_static) == ident(static_original) and file_identity(clean_static) == ident(static_clean),
                'static_changed_during_validation')
        comparison['static'] = {'original': ident(static_original), 'candidate': ident(static_clean), **second}
        headers_root = sdk / 'include/mgmem'
        source_headers = {p.relative_to(source / 'include/mgmem').as_posix(): p.read_bytes()
                          for p in (source / 'include/mgmem').rglob('*') if p.is_file()}
        sdk_headers = {p.relative_to(headers_root).as_posix(): p.read_bytes() for p in headers_root.rglob('*') if p.is_file()}
        require(source_headers == sdk_headers and source_headers, 'sdk_header_mismatch')
        sdk_files = {}
        for file in sdk.rglob('*'):
            require(not file.is_symlink(), 'sdk_symlink')
            if file.is_file(): sdk_files[file.relative_to(sdk).as_posix()] = file.read_bytes()
        validate_sdk_members(sdk_files, source_headers)
        require(sdk_files.get('share/MeguvizMemory/licenses/llvm-NOTICE.txt') == notice, 'sdk_notice_mismatch')
        require(sdk_files.get('share/MeguvizMemory/licenses/LICENSE') == (source / 'LICENSE').read_bytes(), 'sdk_license_mismatch')
        require(sdk_files.get('share/MeguvizMemory/licenses/NOTICE') == (source / 'NOTICE').read_bytes(), 'sdk_license_notice_mismatch')
        sdk_files['lib/libmgmem.so'] = sanitized['core']
        sdk_files['lib/libmgmem_pressure.a'] = static_clean
        sdk_files['lib/libmgmem_pressure_jni.so'] = sanitized['jni']
        sdk_zip = zip_bytes(sdk_files)
        aar_files = {'AndroidManifest.xml': manifest_xml, 'classes.jar': classes_jar, 'proguard.txt': rules,
                     'META-INF/NOTICE-LLVM.txt': notice, 'R.txt': b'',
                     'jni/' + abi + '/libmgmem.so': sanitized['core'],
                     'jni/' + abi + '/libmgmem_pressure_jni.so': sanitized['jni']}
        aar = zip_bytes(aar_files)
        validate_output_identities(aar, sdk_zip, abi, comparison)
        # Configure and link against the sanitized SDK as packaged, not the
        # unsanitized build tree or a manually assembled imported target.
        verify_sdk = work / ('jni-target-sdk-' + abi)
        unpack_sdk_for_target_check(sdk_zip, verify_sdk)
        verify_build = work / ('jni-target-build-' + abi)
        verify_dir = verify_sdk / 'lib/cmake/MeguvizMemory'
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', verify_build, *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON', '-DRECHECK_SAME=ON'],
                 work, operations, abi + '-jni-target-configure')
        imported = (verify_build / 'imported-paths.txt').read_text(encoding='utf-8').splitlines()
        require(len(imported) == 2 and Path(imported[0]).resolve() == (verify_sdk / 'lib/libmgmem_pressure_jni.so').resolve()
                and Path(imported[1]).resolve() == (verify_sdk / 'lib/libmgmem.so').resolve(),
                'jni_target_imported_path_mismatch')
        run_tool([tools['cmake'], '--build', verify_build, '--parallel', str(args.jobs)],
                 work, operations, abi + '-jni-target-build')
        target_dynamic, _ = dynamic_contract(tools['llvm-readelf'], verify_build / 'libjni_target_probe.so',
                                             work, operations, abi + '-jni-target-consumer')
        require(target_dynamic['needed'].count('libmgmem.so') == 1 and
                target_dynamic['needed'].count('libmgmem_pressure_jni.so') == 1,
                'jni_target_consumer_dependency_mismatch')
        # Keep pointer width equal so CMake's generic package-version check
        # cannot reject before the JNI component's own ABI guard is exercised.
        wrong_abi = 'x86' if abi == 'armeabi-v7a' else ('x86_64' if abi == 'arm64-v8a' else 'arm64-v8a')
        wrong_common = ['-DANDROID_ABI=' + wrong_abi if item.startswith('-DANDROID_ABI=') else item for item in common]
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-wrong-abi-' + abi), *wrong_common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON'],
                 work, operations, abi + '-jni-target-wrong-abi',
                 expected_error='Android JNI is not installed for the requested Android ABI')
        # Separate same-ABI prefixes must not reuse an imported core or JNI
        # target from the other package, even when their bytes are identical.
        other_sdk = work / ('jni-target-other-sdk-' + abi)
        unpack_sdk_for_target_check(sdk_zip, other_sdk)
        other_dir = other_sdk / 'lib/cmake/MeguvizMemory'
        rejection = 'Android JNI is not installed for the requested Android ABI'
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-core-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_PACKAGE_DIR=' + other_dir.as_posix()],
                 work, operations, abi + '-jni-target-mixed-core', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-jni-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DPREDEFINED_JNI=' + (other_sdk / 'lib/libmgmem_pressure_jni.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-jni', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-core-config-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_CORE_CONFIGURATION=' + (other_sdk / 'lib/libmgmem.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-core-config', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-jni-config-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_JNI_CONFIGURATION=' + (other_sdk / 'lib/libmgmem_pressure_jni.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-jni-config', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-core-direct-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_CORE_DIRECT=' + (other_sdk / 'lib/libmgmem.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-core-direct', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-jni-direct-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_JNI_DIRECT=' + (other_sdk / 'lib/libmgmem_pressure_jni.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-jni-direct', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-core-map-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_CORE_MAPPING=' + (other_sdk / 'lib/libmgmem.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-core-map', expected_error=rejection)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-mixed-jni-map-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + verify_dir.as_posix(), '-DREQUIRE_JNI=ON',
                  '-DOTHER_JNI_MAPPING=' + (other_sdk / 'lib/libmgmem_pressure_jni.so').as_posix()],
                 work, operations, abi + '-jni-target-mixed-jni-map', expected_error=rejection)
        # A malformed export can create a target that CMake cannot subsequently
        # remove. Even a default (no component) find must therefore fail.
        tampered_sdk = work / ('jni-target-bad-export-sdk-' + abi)
        unpack_sdk_for_target_check(sdk_zip, tampered_sdk)
        export_file = tampered_sdk / 'lib/cmake/MeguvizMemory/MeguvizMemoryAndroidJniTargets.cmake'
        export_text = export_file.read_text(encoding='utf-8')
        original_link = 'INTERFACE_LINK_LIBRARIES "MeguvizMemory::mgmem"'
        require(export_text.count(original_link) == 1, 'jni_target_export_format_mismatch')
        export_file.write_text(export_text.replace(original_link,
                               'INTERFACE_LINK_LIBRARIES "MeguvizMemory::mgmem;unexpected_extra"'),
                               encoding='utf-8', newline='\n')
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-bad-export-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + (tampered_sdk / 'lib/cmake/MeguvizMemory').as_posix(), '-DREQUIRE_JNI=OFF'],
                 work, operations, abi + '-jni-target-bad-export',
                 expected_error='Android JNI target is incompatible with this package')
        # A package with matching metadata but a different ELF machine is also
        # rejected at configure time. Mutate only this private test copy.
        mismatch_binary = other_sdk / 'lib/libmgmem_pressure_jni.so'
        mismatched = bytearray(mismatch_binary.read_bytes())
        mismatched[18:20] = (62 if abi != 'x86_64' else 183).to_bytes(2, 'little')
        mismatch_binary.write_bytes(mismatched)
        run_tool([tools['cmake'], '-S', jni_target_fixture, '-B', work / ('jni-target-wrong-elf-' + abi), *common,
                  '-DMeguvizMemory_DIR=' + other_dir.as_posix(), '-DREQUIRE_JNI=ON'],
                 work, operations, abi + '-jni-target-wrong-elf', expected_error=rejection)
        Scanner(markers).archive(aar, 'aar_' + abi)
        Scanner(markers).archive(sdk_zip, 'sdk_' + abi)
        payloads['android/' + abi + '/MeguvizMemory.aar'] = aar
        payloads['android/' + abi + '/MeguvizMemory-sdk.zip'] = sdk_zip
        results[abi] = {'aar': ident(aar), 'sdk': ident(sdk_zip), 'binaries': comparison,
                        'jni_native_declaration_count': len(expected_jni),
                        'same_sdk_aar_core': True, 'same_sdk_aar_jni': True, 'sdk_static_matches_validated': True,
                        'jni_cmake_target': {'same_abi_compile_link': 'PASS', 'core_only_without_jni': 'PASS',
                                             'missing_jni_component': 'REJECTED', 'wrong_abi': 'REJECTED',
                                             'same_abi_other_prefix_core': 'REJECTED',
                                             'same_abi_other_prefix_jni': 'REJECTED', 'wrong_elf': 'REJECTED',
                                             'other_prefix_configuration_core_and_jni': 'REJECTED',
                                             'unlisted_direct_configuration_core_and_jni': 'REJECTED',
                                             'other_prefix_mapping_core_and_jni': 'REJECTED',
                                             'bad_first_import_default_find': 'REJECTED',
                                             'same_prefix_repeat': 'PASS',
                                             'needed': target_dynamic['needed']}}
    verify_snapshot(source, records)
    require(all(file_identity(path) == tool_hashes[name] for name, path in tools.items()), 'toolchain_changed')
    require(all(file_identity(path) == generator_hashes[name] for name, path in generator_files.items()), 'generator_changed')
    verify_tool_log_integrity(work, operations)
    source_info = {'source_lock_sha256': sha(lock_bytes), 'local_source_set_sha256': sha(canonical(records)),
                   'local_source_count': len(records), 'source_root': 'VERIFIED_PRIVATE_SNAPSHOT',
                   'local_source_records': records,
                   'selected_build_sources': {name: records[name] for name in sorted(records) if name == 'CMakeLists.txt' or
                       name.startswith(('src/', 'include/', 'cmake/', 'adapters/android/', 'third_party_notices/'))}}
    report = {'schema_version': 1, 'state': 'SCOPED_ENGINEERING_CANDIDATE_STATIC_ONLY',
              'scope': 'SOURCE_SNAPSHOT_BUILD_SANITIZATION_AND_PACKAGE_ONLY', 'profiles': results,
              'toolchain': tool_hashes, 'generator': generator_hashes, 'operations': operations, 'classes_jar': ident(classes_jar),
              'jni_signature_check': 'JAVAC_GENERATED_HEADERS_FORCED_INTO_EACH_JNI_TRANSLATION_UNIT',
              'tool_logs': 'UNIQUE_EXCLUSIVE_AND_HASH_VERIFIED_IN_PRIVATE_WORK_DIRECTORY',
              'source': source_info, 'device_execution': 'NOT_RUN', 'r8_runtime': 'NOT_RUN',
              'performance': 'NOT_MEASURED', 'formal_release': 'NOT_QUALIFIED',
              'limits': ['No claim of Android ART behavior, 16 KiB loading, MTE or performance.',
                         'Load hashes, dynamic symbols, dependencies and relocations do not prove behavioral equivalence.',
                         'Snapshot and tool identities do not constitute a signed or complete build provenance attestation.',
                         'Bounded path and identifier scanning is not proof that all private information is absent.']}
    payloads['README.md'] = README
    payloads['SOURCE.json'] = json_bytes(source_info)
    payloads['MANIFEST.json'] = json_bytes(report)
    for name, data in payloads.items():
        if zipfile.is_zipfile(io.BytesIO(data)): Scanner(markers).archive(data, name)
        else: Scanner(markers).bytes(data, name)
    package = zip_bytes(payloads)
    Scanner(markers).archive(package, 'candidate')
    return package, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--source-lock', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--android-jar', type=Path, required=True)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--cmake', type=Path, required=True)
    parser.add_argument('--ninja', type=Path, required=True)
    parser.add_argument('--private-identifiers-file', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    require(1 <= args.jobs <= 16, 'invalid_jobs')
    source_root = args.source_root.resolve(strict=True)
    lock_path = args.source_lock.resolve(strict=True)
    require(source_root.is_dir() and lock_path.is_file(), 'source_or_lock_missing')
    build_root = workspace_directory('build')
    work = fresh_work_directory(args.work_dir, build_root)
    require(not source_root.is_relative_to(work) and not work.is_relative_to(source_root), 'work_overlaps_source')
    private = args.private_identifiers_file.resolve(strict=True)
    require(private.is_relative_to(build_root) and private.is_file() and not args.private_identifiers_file.is_symlink(), 'private_markers_outside_build')
    markers = [line.strip().lower() for line in private.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    require(markers and all(len(marker) >= 3 for marker in markers), 'invalid_private_markers')
    dist = workspace_directory('dist')
    out = fresh_output_path(args.output, dist, '.zip', 'output_not_fresh_dist_zip')
    work.mkdir(parents=True, exist_ok=False)
    status = work / 'STATUS.json'
    status.write_bytes(json_bytes({'state': 'RUNNING', 'output_written': False}))
    try:
        lock_bytes, records = verify_lock(source_root, lock_path)
        snapshot = work / 'source-snapshot'
        copy_snapshot(source_root, snapshot, records)
        package, report = build_candidate(args, work, snapshot, lock_bytes, records, markers)
        current_lock, current_records = verify_lock(source_root, lock_path)
        require(current_lock == lock_bytes and current_records == records, 'source_changed_during_build')
        (work / 'MANIFEST.public.json').write_bytes(json_bytes(report))
        temporary = work / 'candidate.zip'
        with temporary.open('xb') as stream:
            stream.write(package); stream.flush(); os.fsync(stream.fileno())
        # Atomic no-overwrite move avoids a private alias to published bytes.
        publish_no_replace(temporary, out)
        require(file_identity(out) == ident(package), 'published_identity_mismatch')
        status.write_bytes(json_bytes({'state': 'PASS_SCOPED_ENGINEERING_CANDIDATE',
                                      'output_written': True, 'sha256': sha(package)}))
        print(json.dumps({'state': 'PASS_SCOPED_ENGINEERING_CANDIDATE', 'bytes': len(package),
                          'sha256': sha(package), 'profiles': sorted(ABIS), 'device_executed': False}))
        return 0
    except BaseException as error:
        reason = str(error)
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', reason): reason = type(error).__name__
        status.write_bytes(json_bytes({'state': 'FAILED', 'reason': reason, 'output_written': out.exists()}))
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
