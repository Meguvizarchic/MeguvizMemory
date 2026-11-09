#!/usr/bin/env python3
"""Merge the three identified single-ABI AARs without rebuilding any member."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ABIS = {'arm64-v8a': (2, 183), 'armeabi-v7a': (1, 40), 'x86_64': (2, 62)}
COMMON = {'AndroidManifest.xml', 'R.txt', 'classes.jar', 'proguard.txt', 'META-INF/NOTICE-LLVM.txt'}
LIBS = ('libmgmem.so', 'libmgmem_pressure_jni.so')
MAX_ARCHIVE, MAX_MEMBER = 256 * 1024 * 1024, 64 * 1024 * 1024


def require(value, reason):
    if not value: raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(archive, name):
    names = archive.namelist()
    require(len(names) == len(set(names)) and names.count(name) == 1, 'archive_member_missing_or_duplicate')
    entry = archive.getinfo(name)
    require(not entry.is_dir() and entry.file_size <= MAX_MEMBER, 'archive_member_limit')
    return archive.read(name)


def members(candidate):
    result, common = {}, None
    with zipfile.ZipFile(io.BytesIO(candidate)) as outer:
        for abi, (elf_class, machine) in ABIS.items():
            aar_bytes = read(outer, 'android/' + abi + '/MeguvizMemory.aar')
            sdk_bytes = read(outer, 'android/' + abi + '/MeguvizMemory-sdk.zip')
            with zipfile.ZipFile(io.BytesIO(aar_bytes)) as aar, zipfile.ZipFile(io.BytesIO(sdk_bytes)) as sdk:
                expected = COMMON | {'jni/' + abi + '/' + name for name in LIBS}
                require(set(aar.namelist()) == expected, 'aar_member_closure_mismatch')
                selected = {name: read(aar, name) for name in COMMON}
                require(common is None or selected == common, 'non_jni_members_differ')
                if common is None: common = selected
                require({name for name in sdk.namelist() if name.endswith('.so')} ==
                        {'lib/' + name for name in LIBS}, 'sdk_native_closure_mismatch')
                for name in LIBS:
                    member = 'jni/' + abi + '/' + name
                    data = read(aar, member)
                    require(data == read(sdk, 'lib/' + name), 'sdk_aar_native_mismatch')
                    require(len(data) >= 20 and data[:6] == b'\x7fELF' + bytes((elf_class, 1)) and
                            int.from_bytes(data[18:20], 'little') == machine, 'native_architecture_mismatch')
                    result[member] = data
    return {**common, **result}


def merge(candidate, expected_sha, output):
    require(re.fullmatch(r'[0-9a-f]{64}', expected_sha) is not None, 'invalid_candidate_sha256')
    with Path(candidate).open('rb') as stream: data = stream.read(MAX_ARCHIVE + 1)
    require(len(data) <= MAX_ARCHIVE and sha(data) == expected_sha, 'candidate_identity_mismatch')
    output = Path(output).absolute()
    require(output.suffix == '.aar' and output.parent.resolve() == (ROOT / 'dist').resolve(), 'output_must_be_dist_aar')
    require(output.parent.resolve().is_relative_to(ROOT.resolve()), 'output_must_remain_in_workspace')
    require(not output.exists(), 'output_already_exists')
    selected = members(data)
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='universal-aar-', suffix='.pending', dir=output.parent)
    pending = Path(name)
    with os.fdopen(fd, 'w+b') as stream:
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for member in sorted(selected):
                info = zipfile.ZipInfo(member, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, selected[member])
        stream.flush(); os.fsync(stream.fileno())
    with zipfile.ZipFile(pending) as archive:
        require(set(archive.namelist()) == set(selected) and
                all(read(archive, member) == value for member, value in selected.items()), 'merged_member_readback_failed')
    if os.name == 'nt':
        os.rename(pending, output)  # Windows rename atomically rejects an existing destination.
    else:
        os.link(pending, output)  # Atomic no-replace publication on the same filesystem.
        pending.unlink()
    return {'state': 'PASS_MERGED_CANDIDATE_BYTES_ONLY', 'candidate_sha256': expected_sha,
            'aar_sha256': sha(output.read_bytes()), 'aar_bytes': output.stat().st_size,
            'abis': list(ABIS), 'members': {name: sha(value) for name, value in sorted(selected.items())},
            'inherited_candidate_member_bytes': True, 'rebuilt_native_libraries': False,
            'apk_built_from_universal_aar': 'NOT_RUN', 'device_execution': 'NOT_RUN_FOR_UNIVERSAL_AAR'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--candidate-sha256', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(merge(args.candidate, args.candidate_sha256, args.output), indent=2))


if __name__ == '__main__': main()
