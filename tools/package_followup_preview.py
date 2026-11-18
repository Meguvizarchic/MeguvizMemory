#!/usr/bin/env python3
"""Verify or assemble the scoped follow-up engineering preview.

This tool never calls ADB or publishes. It reads explicitly selected sanitized
artifacts and metadata only; sanitizer scratch directories are never walked.
Verification is the default. Writing needs --write and a fresh dist filename.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import zipfile

from package_feature_preview import Scanner, archive_members, identity, read_workspace, require
from source_lock import EXTERNAL_DIRECTORIES, LOCAL_DIRECTORIES, scoped_files

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'artifacts/continuation-next-20261101'
ANDROID = ROOT / 'build/followup-20261101-android-sanitized-v1'
ABIS = ('arm64-v8a', 'x86_64', 'armeabi-v7a')
LOCK_DEFAULT = ROOT / 'artifacts/followup-delivery-20261101/SOURCE_LOCK.json'
OUTPUT_DEFAULT = ROOT / 'dist/MeguvizMemory-1.2.1-Followup-Preview-20261101.zip'

ANDROID_META = {
    'sanitizer_manifest': 'sanitizer_manifest.json',
    'sanitizer_script': 'sanitizer_script.py.txt',
    'privacy_and_archives': 'privacy_and_archives.json',
    'class_and_header': 'class_and_header.json',
    'comment_removal': 'comment_removal.json',
    'source_before': 'source_before.json',
    'r8_build': 'r8_build.json',
    'r8_public_methods': 'r8_public_methods.json',
    'aar_build_arm64-v8a': 'aar_build_arm64-v8a.json',
    'aar_build_x86_64': 'aar_build_x86_64.json',
    'aar_build_armeabi-v7a': 'aar_build_armeabi-v7a.json',
}
HOST_SOURCES = {
    'cpp_header': 'include/mgmem/memory.hpp',
    'cpp_real_test': 'tests/cpp_checked_import_tests.cpp',
    'cpp_fake_test': 'tests/cpp_checked_fake_tests.cpp',
    'cpp_cmake': 'tests/cpp_checked/CMakeLists.txt',
    'native_lease_java': 'adapters/android/java/io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.java',
    'borrow_jvm_test': 'tests/NativeMemoryLeaseJvmTest.java',
    'borrow_jvm_fixture': 'tests/borrow_jni_java_fixture.cpp',
    'java_image': 'consumers/java_image/java/io/github/jichuo1/meguviz/memory/image/JavaImage.java',
    'java_image_native': 'consumers/java_image/native/image_jni.cpp',
    'java_image_verify': 'consumers/java_image/verify_host.py',
}
HOST_STUB_REQUIRED_SOURCES = frozenset((
    'adapters/android/java/io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.java',
    'adapters/android/native/borrow_jni.cpp',
    'include/mgmem/android_borrow_jni.h',
    'include/mgmem/mgmem.h',
    'src/mgmem.cpp',
    'tests/android_borrow/borrow_fixture.cpp',
    'tests/android_borrow/host_stub/android/app/Instrumentation.java',
    'tests/android_borrow/host_stub/android/os/Bundle.java',
    'tests/android_borrow/host_stub/android/os/Looper.java',
    'tests/android_borrow/host_stub/CMakeLists.txt',
    'tests/android_borrow/host_stub/HostProbe.java',
    'tests/android_borrow/host_stub/org/json/JSONObject.java',
    'tests/android_borrow/host_stub/run_host_probe.py',
    'tests/android_borrow/host_stub/stub.cpp',
    'tests/android_borrow/NativeBorrowInstrumentation.java',
    'tools/android_borrow_round.py',
))
HOST_STUB_POSTCI_EXTRA_SOURCES = frozenset((
    'include/mgmem/pressure.h',
    'tests/android_borrow/host_stub/test_host_launcher.py',
))
CONSUMER_SOURCES = (
    'consumers/java_image/CMakeLists.txt',
    'consumers/java_image/README.md',
    'consumers/java_image/runtime/CMakeLists.txt',
    'consumers/java_image/native/image_jni.cpp',
    'consumers/java_image/java/io/github/jichuo1/meguviz/memory/image/JavaImage.java',
    'consumers/java_image/verify_host.py',
    'consumers/java_image/run_host_ci.py',
    'adapters/android/native/pressure_jni.cpp',
    'adapters/android/native/borrow_jni.cpp',
    'adapters/android/java/io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.java',
)
BRIDGE_SOURCES = (
    'adapters/compute/CMakeLists.txt', 'adapters/compute/README.md',
    'adapters/compute/cmake/MeguvizMemoryComputeReadBridgeConfig.cmake.in',
    'adapters/compute/include/mgmem/compute_read_bridge.h',
    'adapters/compute/src/compute_read_bridge.cpp',
    'adapters/compute/tests/read_bridge_tests.cpp',
    'adapters/compute/tests/fake_pre_admission.cpp',
    'adapters/compute/tests/fake_close_race.cpp',
    'adapters/compute/tests/installed_consumer/CMakeLists.txt',
)
CONTRACT_DOCUMENTS = (
    'docs/CPP_CHECKED_IMPORT.md', 'docs/ANDROID_NATIVE_BORROW.md', 'docs/EXTERNAL_IMPORT.md',
)

README = '''# MeguvizMemory follow-up engineering preview

Three single-ABI Android AARs and native SDKs and a Windows shared-core SDK
are engineering candidates. They are not a formal release. The independent
Java image consumer source is included under source/; its host JNI must be
built separately from source and linked to the supplied shared core. It is
not the Android AAR native library. The complete repository is needed for the
consumer's full test driver and historical C++ reference. The optional Compute
READ bridge requires an independently supplied compatible Compute CPU SDK and
the Memory checked-release extension. Its source and default tests are included; a prebuilt
bridge is included only if independently tested against this exact Windows
core binary. ABI minor 1 alone does not imply the checked extension exists.
The Windows core and host JNI use the MSVC dynamic runtime; the target system
must provide a compatible Microsoft Visual C++ runtime. This package retains
the supplied LLVM notice. If no project LICENSE file is present at packaging
time, the project's license information remains unspecified; no license is
invented here.

Source: https://github.com/jichuo1/MeguvizMemory . The source lock records
observed hashes and Git state, not a signed attestation or proof of build
provenance. Host tests, sanitized binary comparisons and R8 build inspection
have distinct scopes. No Android/ART device run, 16 KiB loader, MTE/HWASan,
long-duration device, product integration, performance, energy or formal
release qualification is claimed. BIL is not integrated. The packed-reader
JAR is an existing independent module, not new Memory performance gain.
The v2 Android instrumentation has a host-JVM/stub check, an offline mocked
runner check and an R8 build check; none of these is Android/ART execution.

No diagnostic APK, DEX input, signing key, private device identifier, raw
local log, external Compute SDK or original unsanitized binary is included.
The bounded privacy/path scan cannot prove absence of every possible secret.
'''


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decoded(data: bytes):
    return json.loads(data.decode('utf-8-sig'))


def decoded_unique(data: bytes):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'borrow_v2_host_duplicate_json_key')
            result[key] = value
        return result

    def nonfinite(_):
        raise ValueError('borrow_v2_host_nonfinite_json_value')

    return json.loads(data.decode('utf-8-sig'), object_pairs_hook=pairs, parse_constant=nonfinite)


def file_data(path: Path) -> bytes:
    return read_workspace(path)


def file_sha(path: Path) -> str:
    return sha(file_data(path))


def checked_relative(path: str) -> Path:
    pure = PurePosixPath(path)
    require(path and not pure.is_absolute() and '..' not in pure.parts and '\\' not in path,
            'invalid_source_path')
    return ROOT / path


def verify_source_lock(path: Path) -> bytes:
    raw = file_data(path)
    lock = decoded(raw)
    require(lock.get('schema_version') == 1 and lock.get('scope') == 'SCOPED_SOURCE_TREE_LOCK',
            'source_lock_scope')
    records = lock.get('files')
    require(isinstance(records, list) and lock.get('file_count') == len(records), 'source_lock_count')
    canonical = json.dumps(records, ensure_ascii=False, separators=(',', ':'), sort_keys=True).encode('utf-8')
    require(sha(canonical) == lock.get('source_set_sha256'), 'source_lock_canonical_hash')
    roots = {'MeguvizMemory': (ROOT, LOCAL_DIRECTORIES),
             'DexMeguviz': (ROOT / 'build/locked-inputs/DexMeguviz', EXTERNAL_DIRECTORIES)}
    observed = {}
    for item in records:
        require(isinstance(item, dict) and item.get('root') in roots, 'source_lock_unknown_root')
        name = item.get('path')
        pure = PurePosixPath(name) if isinstance(name, str) else None
        require(pure is not None and not pure.is_absolute() and '..' not in pure.parts and '\\' not in name,
                'source_lock_path')
        key = (item['root'], name)
        require(key not in observed, 'source_lock_duplicate')
        base = roots[item['root']][0]
        target = (base / name).resolve(strict=True)
        require(target.is_relative_to(base.resolve()) and target.is_file() and not (base / name).is_symlink(),
                'source_lock_input')
        payload = target.read_bytes()
        require(identity(payload) == {'bytes': item['bytes'], 'sha256': item['sha256']},
                'source_lock_drift')
        observed[key] = True
    for alias, (base, directories) in roots.items():
        if alias == 'DexMeguviz' and lock.get('external_parser_source') != 'LOCKED':
            continue
        require(base.is_dir(), 'source_lock_root_missing')
        expected = {(alias, item.relative_to(base).as_posix())
                    for item in scoped_files(base, directories, path.resolve())}
        require(expected == {key for key in observed if key[0] == alias}, 'source_lock_set_drift')
    return raw


def check_elf(data: bytes, abi: str) -> None:
    expected = {'arm64-v8a': (2, 183), 'x86_64': (2, 62), 'armeabi-v7a': (1, 40)}[abi]
    require(len(data) >= 20 and data[:4] == b'\x7fELF' and data[5] == 1 and
            int.from_bytes(data[16:18], 'little') == 3 and
            (data[4], int.from_bytes(data[18:20], 'little')) == expected, 'wrong_elf:' + abi)


def expected_source_hashes(hashes: dict) -> None:
    require(isinstance(hashes, dict) and hashes, 'recorded_source_hashes_missing')
    for name, value in hashes.items():
        require(isinstance(name, str) and isinstance(value, str) and len(value) == 64 and
                file_sha(checked_relative(name)) == value, 'recorded_source_drift')


def check_android(data: dict) -> None:
    folder = STAGE / 'android-build'
    index_bytes = file_data(folder / 'metadata-index.json')
    index = decoded(index_bytes)
    require(index.get('state') == 'PASS_BOUNDED_PRIVATE_IDENTIFIER_AND_PATH_PATTERNS' and
            index.get('scope') == 'SANITIZED_METADATA_ONLY' and
            set(index.get('files', {})) == set(ANDROID_META), 'android_metadata_index')
    metadata = {}
    for alias, name in ANDROID_META.items():
        raw = file_data(folder / name)
        entry = index['files'][alias]
        require(identity(raw) == {'bytes': entry['bytes'], 'sha256': entry['sha256']},
                'android_metadata_identity:' + alias)
        metadata[alias] = raw
        data['evidence/android/' + name] = raw
    data['evidence/android/metadata-index.json'] = index_bytes
    manifest = decoded(metadata['sanitizer_manifest'])
    privacy = decoded(metadata['privacy_and_archives'])
    classes = decoded(metadata['class_and_header'])
    comments = decoded(metadata['comment_removal'])
    source_before = decoded(metadata['source_before'])
    require(manifest.get('state') == 'PASS_BINARY_CONTRACTS_PRIVACY_SCAN_NOT_DEVICE_EXECUTED' and
            manifest.get('inputs_unchanged') is True and
            privacy.get('state') == 'PASS_BOUNDED_RECURSIVE_PATH_SCAN_AND_MEMBER_IDENTITIES' and
            classes.get('state') == 'PASS_AAR_RULES_NOTICE_CURRENT_CLASS_HEADER' and
            comments.get('state') == 'PASS_COMMENT_SECTION_NOT_LOADED_AND_REMOVED', 'android_proof_scope')
    expected_source_hashes(source_before)
    java = ROOT / HOST_SOURCES['native_lease_java']
    cpp = ROOT / HOST_SOURCES['cpp_header']
    require(classes['java_source_sha256'] == file_sha(java) and
            classes['cpp_header_sha256'] == file_sha(cpp), 'android_current_source_mismatch')
    for abi in ABIS:
        a = file_data(ANDROID / 'aar' / abi / 'MeguvizMemory-Android-Pressure-1.2.1.aar')
        s = file_data(ANDROID / 'archives' / ('MeguvizMemory-1.2.1-android-' + abi + '-followup-sdk.zip'))
        p, q, c = manifest['profiles'][abi], privacy['profiles'][abi], classes['profiles'][abi]
        require(identity(a) == {'bytes': p['AAR_candidate_bytes'], 'sha256': p['AAR_candidate_sha256']} and
                sha(a) == q['AAR_sha256'] == c['aar_sha256'] and
                identity(s) == {'bytes': q['SDK_archive_bytes'], 'sha256': q['SDK_archive_sha256']} and
                all(c[k] is True for k in ('current_class_match', 'cpp_header_match', 'keep_rules_match', 'llvm_notice_match')) and
                p['AAR_non_native_entries_unchanged'] is True and p['SDK_non_native_entries_unchanged'] is True and
                p['privacy_scan'] == 'NO_MATCH' and q['AAR_and_SDK_core_identical'] is True,
                'android_candidate_identity:' + abi)
        am, sm = archive_members(a), archive_members(s)
        names = ['jni/' + abi + '/libmgmem.so', 'jni/' + abi + '/libmgmem_pressure_jni.so']
        require(sorted(k for k in am if k.endswith('.so')) == sorted(names), 'android_aar_closure:' + abi)
        core, jni = am[names[0]], am[names[1]]
        check_elf(core, abi); check_elf(jni, abi)
        require(core == sm['lib/libmgmem.so'] and sha(core) == p['core']['candidate_sha256'] and
                sha(jni) == p['jni']['candidate_sha256'] and
                sha(sm['lib/libmgmem_pressure.a']) == p['static_pressure']['candidate_sha256'] and
                sm['include/mgmem/memory.hpp'] == file_data(cpp) and
                sm['include/mgmem/mgmem.h'] == file_data(ROOT / 'include/mgmem/mgmem.h') and
                am['proguard.txt'] == file_data(ROOT / 'adapters/android/consumer-rules.pro') and
                am['META-INF/NOTICE-LLVM.txt'] == sm['share/MeguvizMemory/licenses/llvm-NOTICE.txt'],
                'android_aar_sdk_members:' + abi)
        jar = archive_members(am['classes.jar'])
        klass = 'io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.class'
        require(klass in jar and sha(jar[klass]) == classes['class_sha256'], 'android_java_class:' + abi)
        for part, payload in (('core', core), ('jni', jni)):
            item = comments['profiles'][abi][part]
            require(item['candidate_sha256'] == sha(payload) and item['allocated'] is False and
                    item['intersects_load_file_bytes'] is False and item['candidate_comment_absent'] is True and
                    p[part]['PT_LOAD']['machine'] == {'arm64-v8a':183,'x86_64':62,'armeabi-v7a':40}[abi] and
                    bool(p[part]['PT_LOAD']['masked_load_sha256']) and bool(p[part]['dynsym_sha256']),
                    'android_binary_proof:' + abi)
        build = decoded(metadata['aar_build_' + abi])
        require(build.get('status') == 'PASS' and build.get('abi') == abi and
                build.get('device_executed') is False and build.get('performance_qualified') is False,
                'android_build_scope:' + abi)
        originals = [x for x in build['artifacts'] if x['path'].endswith('.aar')]
        require(len(originals) == 1 and originals[0]['sha256'] == p['AAR_original_sha256'],
                'android_original_identity:' + abi)
        data['android/' + abi + '/MeguvizMemory-Android-Pressure.aar'] = a
        data['android/' + abi + '/MeguvizMemory-native-sdk.zip'] = s
    r8, methods = decoded(metadata['r8_build']), decoded(metadata['r8_public_methods'])
    arm = data['android/arm64-v8a/MeguvizMemory-Android-Pressure.aar']
    arm_members = archive_members(arm)
    r8_inputs, r8_report = r8['inputs'], r8['r8_evidence']
    require(r8.get('state') == 'PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION' and
            r8.get('device_executed') is False and r8_inputs['aar_sha256'] == sha(arm) and
            r8_inputs['classes_jar_sha256'] == sha(arm_members['classes.jar']) and
            r8_inputs['shared_core_sha256'] == sha(arm_members['jni/arm64-v8a/libmgmem.so']) and
            r8_inputs['shared_jni_sha256'] == sha(arm_members['jni/arm64-v8a/libmgmem_pressure_jni.so']) and
            r8_report['aar_proguard_sha256'] == sha(arm_members['proguard.txt']) and
            r8_report['diagnostic_rule_scope'] == 'INSTRUMENTATION_CLASS_ONLY_NO_NATIVE_MEMORY_LEASE_KEEP' and
            r8_report['lease_class_name_and_native_signatures_retained_in_dex'] is True and
            r8_report['without_aar_rules_control']['lease_name_preserved'] is False and
            r8_report['device_execution'] == 'NOT_RUN' and
            methods.get('state') == 'PASS_R8_DEX_PUBLIC_METHODS_BUILD_ONLY' and
            methods['apk_sha256'] == r8['apk_sha256'] and
            methods['build_receipt_sha256'] == sha(metadata['r8_build']) and
            methods['java_source_sha256'] == file_sha(java) and
            all(methods['methods'][n]['signature'] == '()I' and
                'PUBLIC' in methods['methods'][n]['access'] for n in ('closeState', 'closeFailureStatus')),
            'r8_build_scope')


def check_host(data: dict) -> None:
    host = file_data(STAGE / 'host-dev-checks.json')
    consumer = file_data(STAGE / 'java-image-ci-host.json')
    prior_consumer = file_data(STAGE / 'java-image-host.json')
    h, c = decoded(host), decoded(consumer)
    require(h.get('state') == 'PASS_SCOPED_HOST_DEVELOPMENT_CHECKS' and
            h['suites']['cpp_checked']['passed'] == 10 and h['suites']['borrow_host']['passed'] == 2 and
            h['java_image']['receipt_sha256'] == sha(prior_consumer) and
            h['java_image']['positive_cases'] == 12 and h['java_image']['negative_cases'] == 8 and
            c.get('state') == 'PASS_INSTALLED_SHARED_HOST_JAVA_IMAGE' and
            c.get('functional_cases') == 12 and c.get('negative_cases') == 8 and
            c.get('scope') == 'HOST_FUNCTIONAL_NO_ANDROID_NO_PERFORMANCE', 'host_scope')
    for key, name in HOST_SOURCES.items():
        require(file_sha(checked_relative(name)) == h['observed_source_sha256'][key], 'host_source_drift:' + key)
    expected_source_hashes(c['source_sha256'])
    for path in CONSUMER_SOURCES:
        data['source/' + path] = file_data(checked_relative(path))
    for path in BRIDGE_SOURCES:
        data['source/' + path] = file_data(checked_relative(path))
    for path in CONTRACT_DOCUMENTS:
        data['source/' + path] = file_data(checked_relative(path))
    licenses = [path for path in (ROOT / 'LICENSE', ROOT / 'LICENSE.txt') if path.is_file()]
    require(len(licenses) <= 1, 'multiple_project_license_files')
    if licenses:
        data['source/' + licenses[0].name] = file_data(licenses[0])
    for name in ('README.md', 'host-dev-checks.json', 'java-image-host.json', 'java-image-ci-host.json',
                 'jni-close/receipt.json', 'jni-close/NativeMemoryLease.java.txt',
                 'jni-close/NativeMemoryLeaseJvmTest.java.txt'):
        data['evidence/host/' + name] = file_data(STAGE / name)


def check_packed(data: dict) -> None:
    old_path = ROOT / 'artifacts/features-20261030/ARTIFACTS.json'
    receipt_path = ROOT / 'artifacts/functional-next-20261030/packed/receipt.json'
    old_raw, rec_raw = file_data(old_path), file_data(receipt_path)
    old, rec = decoded(old_raw), decoded(rec_raw)
    jar = file_data(ROOT / 'dist/meguviz-memory-packed-reader-0.1.0.jar')
    matching = [x for x in old['packages'] if x.get('path') == 'dist/meguviz-memory-packed-reader-0.1.0.jar']
    require(len(matching) == 1 and identity(jar) == {'bytes': matching[0]['bytes'], 'sha256': matching[0]['sha256']} and
            identity(jar) == {'bytes': rec['jar']['bytes'], 'sha256': rec['jar']['sha256']} and
            rec['status'] == 'PASS' and rec['BIL_modified'] is False, 'packed_identity')
    for entry in rec['sources']:
        require(file_sha(checked_relative(entry['path'])) == entry['sha256'], 'packed_source_drift')
    base = 'io/github/jichuo1/meguviz/memory/packed/PackedDanmakuReader'
    require(set(archive_members(jar)) == {base + x for x in
            ('.class', '$BudgetExceededException.class', '$Decision.class', '$ScanProvider.class')}, 'packed_members')
    data['packed/meguviz-memory-packed-reader-0.1.0.jar'] = jar
    data['evidence/packed/feature-artifacts.json'] = old_raw
    data['evidence/packed/functional-receipt.json'] = rec_raw


def check_borrow_v2(data: dict, host_stub_receipt: Path | None = None) -> None:
    """Keep host stubs, mocked runner and R8 build as separate scopes."""
    base = ROOT / 'artifacts/followup-delivery-20261101/android-borrow-v2'
    host_name = 'host-receipt-selected.json' if host_stub_receipt is not None else 'host-receipt-final.json'
    host_path = host_stub_receipt if host_stub_receipt is not None else base / 'host-receipt-final.json'
    names = (host_name, 'runner-offline-receipt-final.json', 'r8-build-receipt-final.json')
    payloads = {host_name: file_data(host_path), **{name: file_data(base / name) for name in names[1:]}}
    host = decoded_unique(payloads[host_name])
    runner, r8 = (decoded(payloads[name]) for name in names[1:])
    instrumentation = ROOT / 'tests/android_borrow/NativeBorrowInstrumentation.java'
    fixture = ROOT / 'tests/android_borrow/borrow_fixture.cpp'
    lease = ROOT / HOST_SOURCES['native_lease_java']
    require(isinstance(host, dict), 'borrow_v2_host_scope_or_source')
    report = host.get('report', {})
    require(isinstance(report, dict) and
            report.get('state') == 'PASS_HOST_REAL_JVM_JNI_ANDROID_STUBS_NOT_ART' and
            report.get('suite') == 'android-native-borrow-v2' and
            type(report.get('checks')) is int and report['checks'] == 62 and
            type(report.get('normal_backing_bytes')) is int and report['normal_backing_bytes'] == 0 and
            type(report.get('quarantined_backing_bytes')) is int and report['quarantined_backing_bytes'] == 32 and
            type(report.get('quarantined_owner_pins')) is int and report['quarantined_owner_pins'] == 2 and
            report.get('quarantine_reclaimed') is False,
            'borrow_v2_host_scope_or_source')
    if host.get('state') == 'PASS_HOST_REAL_JVM_JNI_ANDROID_STUBS_NOT_ART':
        require(host.get('scope') == 'LOCAL_HOST_STUB_SINGLE_TEST_DLL_NO_DEVICE', 'borrow_v2_host_scope_or_source')
        sources = host.get('sources', [])
        require(isinstance(sources, list) and
                all(isinstance(x, dict) and isinstance(x.get('path'), str) and isinstance(x.get('sha256'), str) for x in sources) and
                len(sources) == len({x['path'] for x in sources}), 'borrow_v2_host_sources')
        source_hashes = {x['path']: x['sha256'] for x in sources}
        required_sources = HOST_STUB_REQUIRED_SOURCES
    else:
        ctest = host.get('ctest', {})
        dependency = host.get('dependency_checks', {})
        require(host.get('schema_version') == 1 and host.get('state') == 'PASS_SCOPED_JDK17_HOST_STUB_POST_CI' and
                host.get('scope') == 'HOST_ONLY_REAL_JVM_JNI_ANDROID_STUBS_NOT_ART' and
                type(report.get('java_specification')) is int and report['java_specification'] == 17 and
                report.get('native_access_explicit') is False and isinstance(ctest, dict) and
                type(ctest.get('passed')) is int and ctest['passed'] == 2 and
                type(ctest.get('total')) is int and ctest['total'] == 2 and
                ctest.get('cases') == ['android_borrow_v2_host_stub', 'android_borrow_host_launcher_unit'] and
                type(ctest.get('launcher_unit_checks')) is int and ctest['launcher_unit_checks'] == 8 and
                isinstance(dependency, dict) and dependency.get('cpp_and_java_inputs_not_newer_than_outputs') is True and
                dependency.get('launcher_inputs_not_newer_than_log') is True and
                dependency.get('targeted_ninja_dry_run', {}).get('mgmem_borrow_fixture', {}).get('result') == 'NO_WORK' and
                dependency.get('targeted_ninja_dry_run', {}).get('borrow_probe_java_classes', {}).get('result') == 'NO_WORK',
                'borrow_v2_host_scope_or_source')
        source_hashes = host.get('observed_source_sha256')
        require(isinstance(source_hashes, dict), 'borrow_v2_host_sources')
        required_sources = HOST_STUB_REQUIRED_SOURCES | HOST_STUB_POSTCI_EXTRA_SOURCES
    require(required_sources.issubset(source_hashes), 'borrow_v2_host_sources_missing')
    expected_source_hashes(source_hashes)
    require(source_hashes.get('tests/android_borrow/NativeBorrowInstrumentation.java') == file_sha(instrumentation) and
            source_hashes.get('tests/android_borrow/borrow_fixture.cpp') == file_sha(fixture) and
            source_hashes.get(HOST_SOURCES['native_lease_java']) == file_sha(lease), 'borrow_v2_host_sources_missing')
    require(runner.get('state') == 'PASS_OFFLINE_SYNTHETIC_NO_DEVICE' and
            runner.get('scope') == 'OFFLINE_SYNTHETIC_NO_DEVICE' and
            runner['runner_sha256'] == file_sha(ROOT / 'tools/android_borrow_round.py') and
            runner['tests_sha256'] == file_sha(ROOT / 'tools/test_android_borrow_round.py') and
            {'v2_close_states_and_separate_quarantine', 'v1_not_promoted'}.issubset(runner.get('coverage', [])),
            'borrow_v2_runner_scope_or_source')
    arm = data['android/arm64-v8a/MeguvizMemory-Android-Pressure.aar']
    members = archive_members(arm)
    inputs = r8['inputs']
    require(r8.get('state') == 'PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION' and r8.get('device_executed') is False and
            r8.get('abi') == 'arm64-v8a' and inputs['aar_sha256'] == sha(arm) and
            inputs['classes_jar_sha256'] == sha(members['classes.jar']) and
            inputs['shared_core_sha256'] == sha(members['jni/arm64-v8a/libmgmem.so']) and
            inputs['shared_jni_sha256'] == sha(members['jni/arm64-v8a/libmgmem_pressure_jni.so']) and
            r8['source_sha256']['NativeBorrowInstrumentation.java'] == file_sha(instrumentation) and
            r8['source_sha256']['borrow_fixture.cpp'] == file_sha(fixture) and
            r8['source_sha256']['build_android_borrow_probe.py'] == file_sha(ROOT / 'tools/build_android_borrow_probe.py') and
            r8['r8_evidence']['lease_class_name_and_native_signatures_retained_in_dex'] is True and
            r8['r8_evidence']['device_execution'] == 'NOT_RUN', 'borrow_v2_r8_scope_or_source')
    for name, payload in payloads.items():
        data['evidence/android-borrow-v2/' + name] = payload


def check_windows(data: dict, args) -> str:
    """Bind Windows candidate to its independently produced proof files."""
    require(all((args.windows_sdk_zip, args.windows_jni_zip, args.windows_evidence, args.windows_comparison,
                 args.windows_path_scan)),
            'windows_candidate_and_all_proofs_required')
    archive, jni_archive, ev_raw, cmp_raw, scan_raw = (file_data(x) for x in
        (args.windows_sdk_zip, args.windows_jni_zip, args.windows_evidence, args.windows_comparison, args.windows_path_scan))
    ev, cmp, scan = decoded(ev_raw), decoded(cmp_raw), decoded(scan_raw)
    require(ev.get('state') == 'PASS_SCOPED_SANITIZED_WINDOWS_SDK_AND_HOST_JNI' and
            ev.get('scope') == 'HOST_INSTALL_CONSUMERS_AND_JVM_NO_DEVICE_NO_PERFORMANCE_NO_RELEASE' and
            identity(archive) == {'bytes': ev['sdk_zip']['bytes'], 'sha256': ev['sdk_zip']['sha256']} and
            identity(jni_archive) == {'bytes': ev['host_jni_zip']['bytes'], 'sha256': ev['host_jni_zip']['sha256']} and
            sha(cmp_raw) == ev['sanitization_proof_sha256'] and
            cmp.get('source_preserved') is True and cmp.get('untouched_files_byte_identical') is True and
            cmp.get('private_marker_scan') == 'PASS_ASCII_UTF16LE' and
            ev.get('privacy') == 'BOUNDED_RECURSIVE_KNOWN_IDENTIFIERS_AND_GENERIC_PATH_SCAN_PASSED' and
            scan.get('state') == 'PASS_SCOPED_PATH_AND_PRIVATE_IDENTIFIER_SCAN' and
            scan.get('scope') == 'WINDOWS_SDK_HOST_JNI_AND_PROOF_INPUTS_ONLY' and
            scan['files']['sdk_zip'] == identity(archive) and scan['files']['host_jni_zip'] == identity(jni_archive) and
            scan['files']['sdk_evidence'] == identity(ev_raw) and scan['files']['binary_comparison'] == identity(cmp_raw) and
            ev.get('jni_imports_matching_shared_core') is True and
            ev['installed_tests']['count'] == 10 and ev['host_jvm']['state'] == 'PASS' and
            ev['host_jvm']['jni_loaded_path'] == 'OBSERVED_JVM_LIBRARY_LOG', 'windows_proof_scope')
    members = archive_members(archive)
    jni_members = archive_members(jni_archive)
    proof_dir = ROOT / 'artifacts/followup-delivery-20261101/windows'
    jvm_test = file_data(proof_dir / 'NativeMemoryLeaseJvmTest.java.txt')
    jvm_cmake = file_data(proof_dir / 'installed-consumer-CMakeLists.txt')
    java_class = 'io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.class'
    host_jar = jni_members['java/NativeMemoryLease.jar']
    host_classes = archive_members(host_jar)
    android_classes = decoded(data['evidence/android/class_and_header.json'])
    expected_sdk = {'bin/mgmem.dll', 'bin/mgmem_tlb_owned.exe', 'lib/mgmem.lib', 'lib/mgmem_pressure.lib',
        'include/mgmem/android_borrow_jni.h', 'include/mgmem/android_pressure_jni.h', 'include/mgmem/memory.hpp',
        'include/mgmem/mgmem.h', 'include/mgmem/pressure.h', 'lib/cmake/MeguvizMemory/MeguvizMemoryConfig.cmake',
        'lib/cmake/MeguvizMemory/MeguvizMemoryConfigVersion.cmake',
        'lib/cmake/MeguvizMemory/MeguvizMemoryTargets-release.cmake',
        'lib/cmake/MeguvizMemory/MeguvizMemoryTargets.cmake'}
    require(set(members) == expected_sdk and set(jni_members) == {'bin/mgmem_pressure_jni.dll',
            'lib/mgmem_pressure_jni.lib', 'java/NativeMemoryLease.jar', 'README.md'} and
            sha(members['lib/mgmem_pressure.lib']) == cmp['pressure_archive']['after_sha256'] and
            members['include/mgmem/memory.hpp'] == file_data(ROOT / 'include/mgmem/memory.hpp') and
            members['include/mgmem/mgmem.h'] == file_data(ROOT / 'include/mgmem/mgmem.h') and
            members['include/mgmem/android_borrow_jni.h'] == file_data(ROOT / 'include/mgmem/android_borrow_jni.h') and
            sha(members['bin/mgmem.dll']) == ev['core_dll_sha256'] and
            sha(jni_members['bin/mgmem_pressure_jni.dll']) == ev['jni_dll_sha256'] and
            sha(host_jar) == ev['java_jar_sha256'] and java_class in host_classes and
            sha(host_classes[java_class]) == android_classes['class_sha256'] and
            file_sha(ROOT / HOST_SOURCES['native_lease_java']) == ev['java_source_sha256'] and
            file_sha(ROOT / HOST_SOURCES['cpp_header']) == ev['cpp_header_sha256'] and
            file_sha(ROOT / HOST_SOURCES['borrow_jvm_test']) == ev['host_jvm']['test_source_sha256'] and
            sha(jvm_test) == ev['host_jvm']['transformed_test_source_sha256'] and
            scan['files']['jni_test_source'] == identity(jvm_test) and scan['files']['jni_test_cmake'] == identity(jvm_cmake) and
            b'mgmem_import_external_checked_v1' in members['include/mgmem/mgmem.h'], 'windows_sdk_members')
    data['windows/MeguvizMemory-native-sdk.zip'] = archive
    data['windows/MeguvizMemory-host-jni.zip'] = jni_archive
    data['evidence/windows/shareable-evidence.json'] = ev_raw
    data['evidence/windows/binary-comparison.json'] = cmp_raw
    data['evidence/windows/path-scan.json'] = scan_raw
    data['evidence/windows/NativeMemoryLeaseJvmTest.java.txt'] = jvm_test
    data['evidence/windows/installed-consumer-CMakeLists.txt'] = jvm_cmake
    return sha(members['bin/mgmem.dll'])


def check_bridge(data: dict, args, windows_core_sha: str) -> bool:
    values = (args.bridge_sdk_zip, args.bridge_evidence, args.bridge_test_source, args.bridge_test_cmake)
    if not any(values):
        return False
    require(all(values) and all((args.bridge_sanitization_evidence, args.bridge_comparison, args.bridge_path_scan)),
            'bridge_candidate_and_all_proofs_required')
    archive, identity_raw, test_source, test_cmake, ev_raw, cmp_raw, scan_raw = (file_data(x) for x in (
        args.bridge_sdk_zip, args.bridge_evidence, args.bridge_test_source, args.bridge_test_cmake, args.bridge_sanitization_evidence,
        args.bridge_comparison, args.bridge_path_scan))
    run, ev, cmp, scan = decoded(identity_raw), decoded(ev_raw), decoded(cmp_raw), decoded(scan_raw)
    windows_evidence = decoded(data['evidence/windows/shareable-evidence.json'])
    windows_scan = decoded(data['evidence/windows/path-scan.json'])
    require(run.get('state') == 'PASS_SCOPED_INSTALLED_BRIDGE_WITH_NEW_CORE' and
            ev.get('state') == 'PASS_SCOPED_SANITIZED_INSTALLED_BRIDGE' and
            windows_scan['files']['bridge_evidence'] == identity(identity_raw) and
            windows_scan['files']['bridge_test_source'] == identity(test_source) and
            windows_scan['files']['bridge_test_cmake'] == identity(test_cmake) and
            windows_evidence['bridge_new_core']['receipt_sha256'] == sha(identity_raw) and
            windows_evidence['bridge_new_core']['loaded_core_identity'] == 'OS_MODULE_PATH_AND_FILE_HASH_MATCH' and
            windows_evidence['bridge_new_core']['core_sha256'] == windows_core_sha and
            identity(archive) == {'bytes': ev['sdk_zip']['bytes'], 'sha256': ev['sdk_zip']['sha256']} and
            sha(cmp_raw) == ev['binary_comparison_receipt_sha256'] and sha(scan_raw) == ev['path_scan_receipt_sha256'] and
            scan.get('state') == 'PASS_SCOPED_PATH_PATTERN_SCAN' and scan.get('archive_sha256') == sha(archive) and
            not any(scan['finding_counts'].values()) and cmp.get('source_preserved') is True and
            cmp.get('untouched_files_byte_identical') is True, 'bridge_proof_scope')
    c = run['installed_consumer']
    require(c.get('loaded_core_identity') == 'OS_MODULE_PATH_AND_FILE_HASH_MATCH' and
            c['test_source_sha256'] == sha(test_source) and
            c['core_sha256'] == windows_core_sha and c['cases']['normal']['exit_code'] == 0 and
            c['cases']['violation']['exit_code'] == 0 and c['cases']['wrong_core_control']['exit_code'] == 3 and
            c['violation_case'] == 'ISOLATED_PROCESS_QUARANTINE_NOT_NORMAL_RESOURCE_ZERO', 'bridge_not_proven_against_core')
    old_host = decoded(file_data(ROOT / 'artifacts/feature-development-20261101/host-receipt-v2.json'))
    for key, path in (('bridge_source', BRIDGE_SOURCES[4]), ('bridge_header', BRIDGE_SOURCES[3]),
                      ('bridge_cmake', BRIDGE_SOURCES[0]), ('bridge_tests', BRIDGE_SOURCES[5]),
                      ('bridge_fake_pre_admission', BRIDGE_SOURCES[6]),
                      ('bridge_fake_close_race', BRIDGE_SOURCES[7])):
        require(identity(file_data(ROOT / path)) == old_host['source_fingerprints_observed_at_recording'][key],
                'bridge_recorded_source_drift:' + key)
    members = archive_members(archive)
    require(len(members) == ev['sdk_zip']['entries'] and
            sha(members['lib/mgmem_compute_read_bridge.lib']) == cmp['bridge_archive']['after_sha256'] == c['bridge_sha256'] and
            members['include/mgmem/compute_read_bridge.h'] == file_data(ROOT / BRIDGE_SOURCES[3]),
            'bridge_members')
    data['windows-optional/MeguvizMemory-ComputeReadBridge-sdk.zip'] = archive
    data['evidence/bridge/new-core-identity.json'] = identity_raw
    data['evidence/bridge/read_bridge_attested.cpp.txt'] = test_source
    data['evidence/bridge/attested-CMakeLists.txt'] = test_cmake
    data['evidence/bridge/sanitization-evidence.json'] = ev_raw
    data['evidence/bridge/binary-comparison.json'] = cmp_raw
    data['evidence/bridge/path-scan.json'] = scan_raw
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-identifiers-file', type=Path, required=True)
    parser.add_argument('--source-lock', type=Path, default=LOCK_DEFAULT)
    parser.add_argument('--output', type=Path, default=OUTPUT_DEFAULT)
    parser.add_argument('--host-stub-receipt', type=Path,
                        help='Select another workspace host-stub receipt; default retains the historical path')
    parser.add_argument('--write', action='store_true', help='Write a fresh dist ZIP after all checks')
    for name in ('windows-sdk-zip', 'windows-jni-zip', 'windows-evidence', 'windows-comparison', 'windows-path-scan',
                 'bridge-sdk-zip', 'bridge-evidence', 'bridge-test-source', 'bridge-test-cmake'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--bridge-sanitization-evidence', type=Path,
                        default=ROOT/'artifacts/feature-development-20261101/bridge-sanitized/bridge-shareable-evidence.json')
    parser.add_argument('--bridge-comparison', type=Path,
                        default=ROOT/'artifacts/feature-development-20261101/bridge-sanitized/bridge-binary-comparison.json')
    parser.add_argument('--bridge-path-scan', type=Path,
                        default=ROOT/'artifacts/feature-development-20261101/bridge-sanitized/bridge-shareable-path-scan.json')
    args = parser.parse_args()
    private = args.private_identifiers_file.resolve(strict=True)
    require(private.is_file() and private.is_relative_to((ROOT / 'build').resolve()), 'private_identifiers_scope')
    markers = [x.strip().lower() for x in private.read_text(encoding='utf-8-sig').splitlines() if x.strip()]
    require(markers and all(len(x) >= 3 for x in markers), 'private_identifiers_invalid')
    out = args.output.resolve()
    dist = (ROOT / 'dist').resolve()
    require(dist.is_relative_to(ROOT.resolve()) and out.parent == dist and out.suffix.lower() == '.zip', 'output_scope')
    if args.write:
        require(not out.exists(), 'output_already_exists')
    lock = verify_source_lock(args.source_lock)
    data = {'evidence/SOURCE_LOCK.json': lock}
    check_host(data)
    check_android(data)
    check_borrow_v2(data, args.host_stub_receipt)
    core_sha = check_windows(data, args)
    bridge_included = check_bridge(data, args, core_sha)
    check_packed(data)
    scanner = Scanner(markers)
    for name, payload in data.items():
        scanner.bytes(name.encode('utf-8'), 'member_name')
        if zipfile.is_zipfile(io.BytesIO(payload)):
            scanner.archive(payload, name)
        else:
            scanner.bytes(payload, name)
    scanner.bytes(README.encode('utf-8'), 'readme')
    manifest = {'schema_version': 1, 'state': 'ENGINEERING_PREVIEW_NOT_FORMAL_RELEASE',
        'scope': 'SANITIZED_BUILD_AND_SCOPED_HOST_CHECKS_NO_DEVICE',
        'source_commit_binding': 'NOT_ESTABLISHED_HASHES_NOT_BUILD_PROVENANCE',
        'host_stub_receipt': {'selection': 'EXPLICIT' if args.host_stub_receipt is not None else 'HISTORICAL_DEFAULT',
            'sha256': sha(data['evidence/android-borrow-v2/' +
                               ('host-receipt-selected.json' if args.host_stub_receipt is not None else 'host-receipt-final.json')])},
        'android_art_r8_runtime': 'NOT_QUALIFIED', 'performance_energy_bil_release': 'NOT_QUALIFIED',
        'compute_bridge': 'BINARY_HOST_INSTALLED_WITH_IDENTICAL_CORE_FILE_SOURCE_ID_RECORDED' if bridge_included else 'SOURCE_ONLY',
        'project_license': 'FILE_INCLUDED' if any(k in data for k in ('source/LICENSE', 'source/LICENSE.txt')) else 'NOT_SPECIFIED_NO_LICENSE_FILE',
        'packed_reader': 'EXISTING_SEPARATE_MODULE_NOT_NEW_PERFORMANCE_GAIN',
        'privacy': 'BOUNDED_RECURSIVE_IDENTIFIER_AND_PATH_SCAN_NOT_COMPLETE_SECRET_PROOF',
        'files': {name: identity(payload) for name, payload in sorted(data.items())},
        'README.md': identity(README.encode('utf-8'))}
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
    scanner.bytes(manifest_bytes, 'manifest')
    image = io.BytesIO()
    with zipfile.ZipFile(image, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        def add(name, payload):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            package.writestr(info, payload)
        for name, payload in sorted(data.items()):
            add(name, payload)
        add('README.md', README.encode('utf-8'))
        add('MANIFEST.json', manifest_bytes)
    bundle = image.getvalue()
    Scanner(markers).archive(bundle, 'final_bundle')
    if args.write:
        with out.open('xb') as file:
            file.write(bundle)
            file.flush()
            os.fsync(file.fileno())
    print(json.dumps({'state': 'WRITTEN_LOCAL_PREVIEW' if args.write else 'VERIFIED_READ_ONLY_PREFLIGHT',
                      'bytes': len(bundle), 'sha256': sha(bundle), 'entries': len(data) + 2,
                      'bridge_binary': bridge_included, 'device_execution': 'NOT_QUALIFIED'}))
    return 0


if __name__ == '__main__':
    main()
