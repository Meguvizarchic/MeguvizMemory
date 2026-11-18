#!/usr/bin/env python3
"""Verify and optionally package the scoped 2026-11-01 feature preview.

This is a local engineering preview, not a release or device qualification.
Only explicitly listed sanitized candidates and public metadata are admitted.
Use --verify-only for a read-only preflight; this program never publishes.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'artifacts/feature-development-20261101'
SANITIZED = ROOT / 'build/feature-20261101-sanitized-v2'
SHAREABLE = ROOT / 'build/feature-20261101/shareable'
ABIS = ('arm64-v8a', 'x86_64', 'armeabi-v7a')
MAX_MEMBER = 64 * 1024 * 1024
MAX_TOTAL = 256 * 1024 * 1024

PROOFS = {
    'evidence/host-receipt-v2.json': STAGE/'host-receipt-v2.json',
    'evidence/android/manifest.json': STAGE/'android-sanitized-v2/manifest.json',
    'evidence/android/sdk-archives.json': STAGE/'android-sanitized-v2/sdk-archives.json',
    'evidence/android/generic-path-scan.json': STAGE/'android-sanitized-v2/generic-path-scan.json',
    'evidence/android/independent-readelf.json': STAGE/'android-sanitized-v2/independent-readelf.json',
    'evidence/android/comment-removal-analysis.json': STAGE/'android-sanitized-v2/comment-removal-analysis.json',
    'evidence/android/borrow-r8-build.json': STAGE/'android-borrow-r8-build.json',
    'evidence/windows/binary-comparison.json': STAGE/'windows-sanitized/windows-binary-comparison.json',
    'evidence/windows/shareable-evidence.json': STAGE/'windows-sanitized/windows-shareable-evidence.json',
    'evidence/windows/path-scan.json': STAGE/'windows-sanitized/windows-shareable-extended-path-scan.json',
    'evidence/bridge/binary-comparison.json': STAGE/'bridge-sanitized/bridge-binary-comparison.json',
    'evidence/bridge/shareable-evidence.json': STAGE/'bridge-sanitized/bridge-shareable-evidence.json',
    'evidence/bridge/path-scan.json': STAGE/'bridge-sanitized/bridge-shareable-path-scan.json',
    'evidence/packed/feature-artifacts.json': ROOT/'artifacts/features-20261030/ARTIFACTS.json',
    'evidence/packed/functional-receipt.json': ROOT/'artifacts/functional-next-20261030/packed/receipt.json',
}

SOURCE_CHECKS = {
    'core_c_header': 'include/mgmem/mgmem.h',
    'core_implementation': 'src/mgmem.cpp',
    'checked_import_tests': 'tests/checked_import_tests.cpp',
    'checked_import_c_consumer': 'tests/checked_import_c.c',
    'borrow_header': 'include/mgmem/android_borrow_jni.h',
    'borrow_jni': 'adapters/android/native/borrow_jni.cpp',
    'borrow_java': 'adapters/android/java/io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.java',
    'borrow_jni_host_tests': 'tests/borrow_jni_host_tests.cpp',
    'borrow_java_test': 'tests/NativeMemoryLeaseJvmTest.java',
    'borrow_java_fixture': 'tests/borrow_jni_java_fixture.cpp',
    'bridge_header': 'adapters/compute/include/mgmem/compute_read_bridge.h',
    'bridge_source': 'adapters/compute/src/compute_read_bridge.cpp',
    'bridge_tests': 'adapters/compute/tests/read_bridge_tests.cpp',
    'bridge_fake_pre_admission': 'adapters/compute/tests/fake_pre_admission.cpp',
    'bridge_fake_close_race': 'adapters/compute/tests/fake_close_race.cpp',
    'core_cmake': 'CMakeLists.txt',
    'android_cmake': 'adapters/android/CMakeLists.txt',
    'bridge_cmake': 'adapters/compute/CMakeLists.txt',
    'android_keep_rules': 'adapters/android/consumer-rules.pro',
    'android_packager': 'tools/build_pressure_android.py',
    'receipt_generator': 'tools/record_feature_development.py',
}

README = '''# MeguvizMemory engineering feature preview

This archive contains three separate single-ABI Android AARs and native SDKs,
one Windows x64 Memory SDK, an optional Windows Compute CPU READ bridge, and an
existing standalone packed-reader JAR. The optional bridge requires separately
supplied, compatible real MeguvizCompute and MeguvizMemory SDKs; it does not
change the core or any application's dependency graph. It requires the checked
external-release extension and cannot be loaded against an older runtime that
does not export that symbol. See the bridge's source documentation for the
owner, map, failure, and checked-release limitations.

Source is maintained at https://github.com/jichuo1/MeguvizMemory . This preview
is identified by the enclosed file hashes and is not asserted to have been
built from a particular clean commit. Source fingerprints, test reports, and
binary transformation proofs each have different scopes and are not a signed
provenance attestation.

Evidence covers scoped host tests, installed host consumers and build/packaging
checks. The R8 diagnostic evidence is build-only: no device/ART/R8 execution is
claimed for this feature preview. No 16 KiB loader, MTE/HWASan, long-duration
device, product, performance, energy, BIL integration, or formal-release
qualification is asserted. The packed reader is an existing independent
module, not a new Memory performance gain. BIL is not integrated.

No APK, DEX input, signing key, external Compute SDK, private device identifier,
raw local log, or original unsanitized binary is intentionally included.
The path/identifier scan is bounded and cannot prove all secrets absent.
'''


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_workspace(path):
    resolved = path.resolve(strict=True)
    require(resolved.is_relative_to(ROOT.resolve()) and resolved.is_file(), 'input_outside_workspace_or_missing')
    return resolved.read_bytes()


def parsed(path):
    return json.loads(read_workspace(path).decode('utf-8-sig'))


def identity(data):
    return {'bytes': len(data), 'sha256': digest(data)}


def nested_wide(pattern, order):
    return (b''.join(bytes((value, 0)) for value in pattern) if order == 'le'
            else b''.join(bytes((0, value)) for value in pattern))


def generic_rules():
    roots = (b'users', b'home', b'mnt', b'tmp', b'var', b'opt', b'private',
             b'workspace', b'workspaces', b'builds', b'volumes')
    result = [re.compile(rb'(?i)(?<![A-Za-z0-9])[A-Z]:[\\/][\x20-\x7e]{1,180}'),
              re.compile(rb'(?i)/(?:'+b'|'.join(roots)+rb')/[\x20-\x7e]{1,180}')]
    for order in ('le', 'be'):
        letter = rb'[A-Z]\x00' if order == 'le' else rb'\x00[A-Z]'
        colon = re.escape(nested_wide(b':', order))
        slash = b'(?:'+re.escape(nested_wide(b'/', order))+b'|'+re.escape(nested_wide(b'\\', order))+b')'
        printable = rb'(?:[\x20-\x7e]\x00)' if order == 'le' else rb'(?:\x00[\x20-\x7e])'
        result.append(re.compile(b'(?i)'+letter+colon+slash+printable+b'{1,180}'))
        unix = re.escape(nested_wide(b'/', order))
        result.append(re.compile(b'(?i)'+unix+b'(?:'+b'|'.join(re.escape(nested_wide(x,order)) for x in roots)+b')'+unix+printable+b'{1,180}'))
    return result


class Scanner:
    def __init__(self, markers):
        self.markers = markers
        self.rules = generic_rules()
        self.count = 0
        self.total = 0

    def identifiers(self, data, label):
        lower = data.lower()
        for marker in self.markers:
            if any(marker.encode(encoding).lower() in lower for encoding in ('utf-8','utf-16le','utf-16be')):
                raise ValueError('private_identifier_in:' + label)

    def bytes(self, data, label):
        self.count += 1
        self.identifiers(data,label)
        if any(rule.search(data) for rule in self.rules):
            raise ValueError('machine_path_pattern_in:' + label)

    def archive(self, data, label, depth=0):
        require(depth <= 5, 'archive_depth_exceeded')
        # Known literal identifiers are safe to search even in compressed data.
        # Generic path regexes are limited to content/metadata/overlays to avoid
        # interpreting random compressed bytes as meaningful paths.
        self.identifiers(data,label+':archive_bytes')
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.bytes(archive.comment, label+':archive_comment')
            infos = archive.infolist()
            first=min((info.header_offset for info in infos),default=0)
            self.bytes(data[:first],label+':prefix')
            end=data.rfind(b'PK\x05\x06')
            require(end>=0 and end+22<=len(data), 'archive_end_missing:'+label)
            end_of_comment=end+22+int.from_bytes(data[end+20:end+22],'little')
            require(end_of_comment<=len(data), 'archive_comment_truncated:'+label)
            self.bytes(data[end_of_comment:],label+':trailing_bytes')
            names = [info.filename for info in infos]
            require(len(names)==len(set(names))==len({name.casefold() for name in names}), 'duplicate_archive_member:'+label)
            for info in infos:
                name = info.filename
                path = PurePosixPath(name)
                require('\\' not in name and not path.is_absolute() and '..' not in path.parts and
                        not re.match(r'^[A-Za-z]:', name), 'unsafe_archive_member:'+label)
                self.bytes(name.encode('utf-8'), label+':member_name')
                self.bytes(info.comment, label+':member_comment')
                self.bytes(info.extra, label+':member_extra')
                require(info.flag_bits & 1 == 0, 'encrypted_archive_member:'+label)
                off=info.header_offset
                require(off>=0 and off+30<=len(data) and data[off:off+4]==b'PK\x03\x04', 'local_header_missing:'+label)
                name_size,extra_size=struct.unpack_from('<HH',data,off+26)
                header_end=off+30+name_size+extra_size
                require(header_end<=len(data), 'local_header_truncated:'+label)
                self.bytes(data[off+30:off+30+name_size],label+':local_member_name')
                self.bytes(data[off+30+name_size:header_end],label+':local_member_extra')
                require(((info.external_attr >> 16) & 0o170000) != 0o120000, 'archive_symlink:'+label)
                if info.is_dir():
                    continue
                require(path.suffix.lower() not in {'.apk','.dex','.key','.pem','.p12','.pfx','.jks','.keystore'} and
                        path.name.lower() != '.env' and not path.name.lower().startswith('.env.'),
                        'forbidden_archive_member:'+label)
                require(info.file_size <= MAX_MEMBER, 'archive_member_size_exceeded:'+label)
                self.total += info.file_size
                require(self.total <= MAX_TOTAL, 'archive_total_size_exceeded')
                payload = archive.read(info)
                require(len(payload)==info.file_size, 'archive_member_size_mismatch:'+label)
                child = label+'/'+name
                if zipfile.is_zipfile(io.BytesIO(payload)):
                    self.archive(payload, child, depth+1)
                else:
                    self.bytes(payload, child)


def archive_members(data):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        infos=[info for info in archive.infolist() if not info.is_dir()]
        names=[info.filename for info in infos]
        require(len(names)==len(set(names))==len({name.casefold() for name in names}), 'duplicate_input_member')
        require(all(info.file_size<=MAX_MEMBER for info in infos) and
                sum(info.file_size for info in infos)<=MAX_TOTAL, 'input_archive_size_exceeded')
        return {info.filename: archive.read(info) for info in infos}


def check_elf(data, abi):
    expected = {'arm64-v8a':(2,183),'x86_64':(2,62),'armeabi-v7a':(1,40)}[abi]
    require(len(data)>=20 and data[:4]==b'\x7fELF' and data[5]==1 and
            (data[4],int.from_bytes(data[18:20],'little'))==expected, 'wrong_elf_abi:'+abi)


def check_current_sources(receipt):
    recorded = receipt['source_fingerprints_observed_at_recording']
    for key, name in SOURCE_CHECKS.items():
        require(identity(read_workspace(ROOT/name))==recorded[key], 'source_drift:'+key)


def collect():
    data = {name:read_workspace(path) for name,path in PROOFS.items()}
    proof = {name:json.loads(value.decode('utf-8-sig')) for name,value in data.items()}
    host = proof['evidence/host-receipt-v2.json']
    require(host['state']=='PASS_SCOPED_LOCAL_FEATURE_EVIDENCE' and
            all(host['suites'][name]['status']=='PASS' and host['suites'][name]['passed']==count and
                host['suites'][name]['total']==count for name,count in
                [('host',37),('host-asan',37),('borrow-host',2),('bridge-host',4)]) and
            host['installed_consumer']['status']=='PASS_SCOPED_LOCAL_INSTALLED_CONSUMER' and
            host['device_execution']=='NOT_EXECUTED' and host['formal_release']=='NOT_QUALIFIED' and
            host['performance_qualification']=='NOT_MEASURED' and host['BIL']=='NOT_INTEGRATED', 'host_evidence_scope')
    check_current_sources(host)
    android = proof['evidence/android/manifest.json']
    sdk_proof = proof['evidence/android/sdk-archives.json']
    privacy = proof['evidence/android/generic-path-scan.json']
    readelf = proof['evidence/android/independent-readelf.json']
    comments = proof['evidence/android/comment-removal-analysis.json']
    require(android['state']=='PASS_BINARY_CONTRACTS_PRIVACY_SCAN_NOT_DEVICE_EXECUTED' and
            sdk_proof['state']=='PASS_RECURSIVE_PRIVACY_AND_SDK_MEMBER_IDENTITY' and
            privacy['state']=='PASS' and not privacy['hits'] and readelf['state']=='PASS_INDEPENDENT_READELF' and
            comments['state']=='PASS_NONALLOC_COMMENT_OUTSIDE_LOAD_REMOVED_ON_COPY', 'android_proof_scope')
    for abi in ABIS:
        candidate = android['profiles'][abi]
        aar_path = SANITIZED/'aar'/abi/'MeguvizMemory-Android-Pressure-1.2.1.aar'
        sdk_path = SANITIZED/'archives'/('MeguvizMemory-1.2.1-android-'+abi+'-sanitized-sdk.zip')
        aar, sdk = read_workspace(aar_path), read_workspace(sdk_path)
        require(identity(aar)=={'bytes':candidate['AAR_candidate_bytes'],'sha256':candidate['AAR_candidate_sha256']} and
                identity(sdk)=={'bytes':sdk_proof['profiles'][abi]['bytes'],'sha256':sdk_proof['profiles'][abi]['sha256']} and
                privacy['outputs'][abi]=={'aar_sha256':digest(aar),'sdk_zip_sha256':digest(sdk)} and
                candidate['privacy_scan']=='NO_MATCH' and candidate['AAR_non_native_entries_unchanged'] is True and
                candidate['SDK_non_native_entries_unchanged'] is True, 'android_candidate_identity:'+abi)
        rd = readelf['profiles'][abi]
        require(all(rd[k]==v for k,v in {'program_headers':'IDENTICAL','needed_soname':'IDENTICAL',
                'aar_sdk_core':'BYTE_IDENTICAL','aar_jni':'BYTE_IDENTICAL'}.items()), 'readelf_scope:'+abi)
        for part in ('core','jni'):
            x=comments['profiles'][abi][part]
            require(x['candidate_sha256']==candidate[part]['candidate_sha256'] and
                    x['original_sha256']==candidate[part]['original_sha256'] and
                    x['comment_section_flags_alloc'] is False and x['comment_intersects_PT_LOAD'] is False and
                    x['candidate_comment_absent'] is True, 'comment_removal_scope:'+abi)
        aar_members, sdk_members = archive_members(aar), archive_members(sdk)
        core = aar_members['jni/'+abi+'/libmgmem.so']
        jni = aar_members['jni/'+abi+'/libmgmem_pressure_jni.so']
        require(sorted(n for n in aar_members if n.endswith('.so'))==sorted([
                'jni/'+abi+'/libmgmem.so','jni/'+abi+'/libmgmem_pressure_jni.so']) and
                core==sdk_members['lib/libmgmem.so'] and digest(core)==candidate['core']['candidate_sha256'] and
                digest(jni)==candidate['jni']['candidate_sha256'] and
                digest(sdk_members['lib/libmgmem_pressure.a'])==candidate['static_pressure']['candidate_sha256'],
                'android_native_members:'+abi)
        check_elf(core,abi); check_elf(jni,abi)
        require(b'NativeMemoryLease' in aar_members['proguard.txt'] and
                aar_members['META-INF/NOTICE-LLVM.txt']==sdk_members['share/MeguvizMemory/licenses/llvm-NOTICE.txt'] and
                'io/github/jichuo1/meguviz/memory/android/NativeMemoryLease.class' in archive_members(aar_members['classes.jar']),
                'android_java_or_notice_missing:'+abi)
        data['android/'+abi+'/MeguvizMemory-Android-Pressure.aar']=aar
        data['android/'+abi+'/MeguvizMemory-native-sdk.zip']=sdk
    r8=proof['evidence/android/borrow-r8-build.json']
    require(r8['state']=='PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION' and r8['device_executed'] is False and
            r8['inputs']['aar_sha256']==digest(data['android/arm64-v8a/MeguvizMemory-Android-Pressure.aar']), 'r8_build_scope')

    windows=proof['evidence/windows/shareable-evidence.json']
    win_cmp=proof['evidence/windows/binary-comparison.json']
    win_scan=proof['evidence/windows/path-scan.json']
    win=read_workspace(SHAREABLE/'MeguvizMemory-1.2.1-windows-x64-feature-preview.zip')
    require(windows['state']=='PASS_SCOPED_SANITIZED_INSTALLED_CONSUMERS' and
            win_cmp['state']=='PASS_SANITIZED_BINARY_COMPARISON_CONSUMER_PENDING' and
            win_cmp['source_preserved'] is True and win_cmp['untouched_files_byte_identical'] is True and
            windows['image_pressure_consumer']['status']=='PASS' and windows['checked_c_consumer']['status']=='PASS' and
            identity(win)=={'bytes':windows['sdk_zip']['bytes'],'sha256':windows['sdk_zip']['sha256']} and
            digest(data['evidence/windows/binary-comparison.json'])==windows['binary_comparison_receipt_sha256'] and
            win_scan['state']=='PASS_SCOPED_PATH_PATTERN_SCAN' and win_scan['archive_sha256']==digest(win) and
            not any(win_scan['finding_counts'].values()), 'windows_proof_scope')
    win_members=archive_members(win)
    require(len(win_members)==windows['sdk_zip']['entries'] and
            digest(win_members['lib/mgmem_pressure.lib'])==win_cmp['pressure_archive']['after_sha256'] and
            digest(win_members['bin/mgmem.dll'])==windows['image_pressure_consumer']['runtime_sha256']==
                host['binary_fingerprints_observed_at_recording']['host_installed_core']['sha256'] and
            b'mgmem_import_external_checked_v1' in win_members['include/mgmem/mgmem.h'], 'windows_sdk_members')
    data['windows/MeguvizMemory-native-sdk.zip']=win

    bridge=proof['evidence/bridge/shareable-evidence.json']
    br_cmp=proof['evidence/bridge/binary-comparison.json']
    br_scan=proof['evidence/bridge/path-scan.json']
    br=read_workspace(SHAREABLE/'MeguvizMemory-ComputeReadBridge-windows-x64-feature-preview.zip')
    require(bridge['state']=='PASS_SCOPED_SANITIZED_INSTALLED_BRIDGE' and
            br_cmp['state']=='PASS_SANITIZED_BINARY_COMPARISON_CONSUMER_PENDING' and
            br_cmp['source_preserved'] is True and br_cmp['untouched_files_byte_identical'] is True and
            identity(br)=={'bytes':bridge['sdk_zip']['bytes'],'sha256':bridge['sdk_zip']['sha256']} and
            digest(data['evidence/bridge/binary-comparison.json'])==bridge['binary_comparison_receipt_sha256'] and
            digest(data['evidence/bridge/path-scan.json'])==bridge['path_scan_receipt_sha256'] and
            br_scan['state']=='PASS_SCOPED_PATH_PATTERN_SCAN' and br_scan['archive_sha256']==digest(br) and
            not any(br_scan['finding_counts'].values()), 'bridge_proof_scope')
    br_members=archive_members(br)
    require(len(br_members)==bridge['sdk_zip']['entries'] and
            digest(br_members['lib/mgmem_compute_read_bridge.lib'])==br_cmp['bridge_archive']['after_sha256'] and
            b'mgmem_compute_import_read_v1' in br_members['include/mgmem/compute_read_bridge.h'] and
            bridge['installed_consumer']['cases']['normal']['status']=='PASS' and
            bridge['installed_consumer']['cases']['contract_violation_quarantine']['status']=='PASS' and
            bridge['installed_consumer']['quarantine_case']=='ISOLATED_PROCESS_NOT_NORMAL_RESOURCE_ZERO', 'bridge_sdk_members')
    data['windows-optional/MeguvizMemory-ComputeReadBridge-sdk.zip']=br

    old = proof['evidence/packed/feature-artifacts.json']
    packed = proof['evidence/packed/functional-receipt.json']
    jar = read_workspace(ROOT/'dist/meguviz-memory-packed-reader-0.1.0.jar')
    matching = [x for x in old['packages'] if x.get('path')=='dist/meguviz-memory-packed-reader-0.1.0.jar']
    require(len(matching)==1 and identity(jar)=={'bytes':matching[0]['bytes'],'sha256':matching[0]['sha256']} and
            identity(jar)=={'bytes':packed['jar']['bytes'],'sha256':packed['jar']['sha256']} and
            packed['status']=='PASS' and packed['BIL_modified'] is False, 'packed_identity_mismatch')
    for item in packed['sources']:
        require(digest(read_workspace(ROOT/item['path']))==item['sha256'], 'packed_source_drift')
    packed_members=archive_members(jar)
    base='io/github/jichuo1/meguviz/memory/packed/PackedDanmakuReader'
    require(set(packed_members)=={base+'.class',base+'$BudgetExceededException.class',base+'$Decision.class',base+'$ScanProvider.class'},
            'packed_production_members_mismatch')
    data['packed/meguviz-memory-packed-reader-0.1.0.jar']=jar
    return data


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-identifiers-file',type=Path,required=True,
                        help='Ignored local file of private markers; values are never printed')
    parser.add_argument('--output',type=Path,default=ROOT/'dist/MeguvizMemory-1.2.1-Feature-Preview-20261101.zip')
    parser.add_argument('--verify-only',action='store_true',help='Run all source, archive and privacy checks without writing')
    args=parser.parse_args()
    private_file=args.private_identifiers_file.resolve(strict=True)
    require(private_file.is_file() and private_file.is_relative_to((ROOT/'build').resolve()), 'private_identifiers_must_be_local_build_file')
    markers=[s.strip().lower() for s in private_file.read_text(encoding='utf-8-sig').splitlines() if s.strip()]
    require(markers and all(len(s)>=3 for s in markers), 'private_identifiers_missing_or_too_short')
    output=args.output.resolve()
    dist=(ROOT/'dist').resolve()
    require(dist.is_relative_to(ROOT.resolve()) and output.parent==dist and output.suffix.lower()=='.zip', 'output_must_be_direct_child_of_workspace_dist')
    if not args.verify_only:
        require(not output.exists(), 'output_already_exists')
    data=collect()
    scanner=Scanner(markers)
    for name,payload in data.items():
        scanner.bytes(name.encode(), 'package_member_name')
        if zipfile.is_zipfile(io.BytesIO(payload)):
            scanner.archive(payload,name)
        else:
            scanner.bytes(payload,name)
    scanner.bytes(README.encode(), 'README')
    manifest={'schema_version':1,'state':'ENGINEERING_PREVIEW_NOT_FORMAL_RELEASE',
              'source_repository':'https://github.com/jichuo1/MeguvizMemory',
              'source_commit_binding':'NOT_ESTABLISHED_CURRENT_SOURCE_HASHES_ARE_NOT_BUILD_PROVENANCE',
              'scope':'SCOPED_HOST_FUNCTIONAL_AND_SANITIZED_BUILD_ONLY',
              'device_ART_R8_execution':'NOT_QUALIFIED','R8_diagnostic':'BUILD_ONLY',
              'performance_energy_BIL_formal_release':'NOT_QUALIFIED',
              'optional_bridge':'REQUIRES_SEPARATE_REAL_COMPUTE_AND_MATCHING_MEMORY_SDK',
              'packed_reader':'EXISTING_INDEPENDENT_MODULE_NOT_NEW_MEMORY_PERFORMANCE_GAIN',
              'privacy':'BOUNDED_RECURSIVE_KNOWN_IDENTIFIERS_AND_GENERIC_PATH_SCAN',
              'files':{name:identity(payload) for name,payload in sorted(data.items())},
              'README.md':identity(README.encode()),
              'limits':['No test APK, DEX, signing key, raw log, private identifier or original unsanitized binary intentionally included.',
                        'Evidence and hashes do not alone prove exact build provenance or all behavior.',
                        'Privacy scanning is heuristic and not proof that all secrets are absent.']}
    manifest_bytes=(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n').encode()
    scanner.bytes(manifest_bytes,'manifest')
    image=io.BytesIO()
    with zipfile.ZipFile(image,'w',compression=zipfile.ZIP_DEFLATED) as package:
        def add(name,payload):
            info=zipfile.ZipInfo(name,(1980,1,1,0,0,0))
            info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=0o644 << 16
            package.writestr(info,payload)
        for name,payload in sorted(data.items()):add(name,payload)
        add('README.md',README.encode())
        add('MANIFEST.json',manifest_bytes)
    result=image.getvalue()
    Scanner(markers).archive(result,'final_bundle')
    if not args.verify_only:
        with output.open('xb') as file:
            file.write(result);file.flush();os.fsync(file.fileno())
    print(json.dumps({'state':'VERIFIED_READ_ONLY_PREFLIGHT' if args.verify_only else 'WRITTEN_LOCAL_PREVIEW',
                      'bytes':len(result),'sha256':digest(result),'entries':len(data)+2,
                      'source_commit_binding':'NOT_ESTABLISHED','device_execution':'NOT_QUALIFIED'}))


if __name__=='__main__':main()
