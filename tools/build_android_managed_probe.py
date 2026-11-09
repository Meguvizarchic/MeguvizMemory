#!/usr/bin/env python3
"""Build an isolated R8 managed-domain diagnostic APK; never invokes ADB.

The APK and its fresh diagnostic key stay in build/. Its input libraries are
read from an identified Android engineering candidate, not rebuilt here.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
from pathlib import Path
import zipfile

import build_android_candidate as common
import android_managed_round as managed_round

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'io.github.jichuo1.meguviz.memory.managedprobe'
INSTRUMENT = 'io.github.jichuo1.meguviz.memory.android.ManagedInstrumentation'
WITNESS = 'io.github.jichuo1.meguviz.memory.android.ManagedShrinkWitness'
JAVA_PACKAGE = 'io.github.jichuo1.meguviz.memory.android.'
NATIVE_CLASSES = ('NativeMemoryLease', 'NativePressureBackend', 'ManagedMemoryDomain')
MAX_MEMBER = 64 * 1024 * 1024
MAX_CANDIDATE = 256 * 1024 * 1024
MAX_EVIDENCE = 1024 * 1024


def digest(data):
    return hashlib.sha256(data).hexdigest()


def bounded_file(path, maximum):
    with path.open('rb') as stream:
        data = stream.read(maximum + 1)
        common.require(len(data) <= maximum and stream.read(1) == b'', 'input_file_limit')
        return data


def read_unique(archive, name):
    names = archive.namelist()
    common.require(len(names) == len(set(names)) and names.count(name) == 1, 'archive_member_missing_or_duplicate')
    info = archive.getinfo(name)
    common.require(info.file_size <= MAX_MEMBER and not info.is_dir(), 'archive_member_invalid')
    return archive.read(info)


def candidate_inputs(candidate, evidence, abi):
    candidate_bytes = bounded_file(candidate, MAX_CANDIDATE)
    evidence_bytes = bounded_file(evidence, MAX_EVIDENCE)
    record = managed_round.unique_json(evidence_bytes.decode('utf-8-sig'))
    common.require(record.get('state') == 'PASS_SCOPED_ENGINEERING_CANDIDATES' and
                   record.get('android', {}).get('sha256') == digest(candidate_bytes), 'candidate_identity_mismatch')
    profile = record['android']['profiles'][abi]
    with zipfile.ZipFile(io.BytesIO(candidate_bytes)) as outer:
        aar = read_unique(outer, 'android/' + abi + '/MeguvizMemory.aar')
        sdk = read_unique(outer, 'android/' + abi + '/MeguvizMemory-sdk.zip')
        source = managed_round.unique_json(read_unique(outer, 'SOURCE.json').decode('utf-8'))
    common.require(digest(aar) == profile['aar_sha256'] and digest(sdk) == profile['sdk_sha256'], 'candidate_member_mismatch')
    common.require(isinstance(record.get('source_commit'), str) and
                   re.fullmatch(r'[0-9a-f]{40}', record['source_commit']) is not None and
                   source.get('source_lock_sha256') == record.get('source_lock_sha256') and
                   source.get('local_source_set_sha256') == record.get('source_set_sha256') and
                   source.get('local_source_count') == record.get('source_record_count'),
                   'candidate_source_identity_mismatch')
    with zipfile.ZipFile(io.BytesIO(aar)) as archive:
        expected = {'AndroidManifest.xml', 'R.txt', 'classes.jar', 'proguard.txt', 'META-INF/NOTICE-LLVM.txt',
                    'jni/' + abi + '/libmgmem.so', 'jni/' + abi + '/libmgmem_pressure_jni.so'}
        common.require(set(archive.namelist()) == expected and len(archive.namelist()) == len(expected), 'aar_closure_invalid')
        classes = read_unique(archive, 'classes.jar')
        rules = read_unique(archive, 'proguard.txt')
        core = read_unique(archive, 'jni/' + abi + '/libmgmem.so')
        jni = read_unique(archive, 'jni/' + abi + '/libmgmem_pressure_jni.so')
    with zipfile.ZipFile(io.BytesIO(sdk)) as archive:
        common.require(read_unique(archive, 'lib/libmgmem.so') == core and
                       read_unique(archive, 'lib/libmgmem_pressure_jni.so') == jni, 'sdk_aar_library_mismatch')
    common.elf_parts(core, abi); common.elf_parts(jni, abi)
    common.validate_keep_rules(rules)
    records = source.get('local_source_records')
    common.require(isinstance(records, dict), 'candidate_source_records_missing')
    # The AAR is from a prior frozen source snapshot. Only these current source
    # files are used to derive JNI signatures; require exact candidate bytes.
    for klass in NATIVE_CLASSES:
        name = common.JAVA_BASE + klass + '.java'
        path = common.safe_file(ROOT, name)
        common.require(records.get(name) == common.file_identity(path), 'candidate_java_source_drift')
    return record, candidate_bytes, evidence_bytes, aar, sdk, classes, rules, core, jni


def native_descriptors(javap_output):
    pattern = (r'(?m)^\s*private\s+static\s+native\s+[^\n;]+?\s+(\w+)\([^\n]*\);\s*\r?\n'
               r'\s*descriptor:\s*(\S+)')
    pairs = re.findall(pattern, javap_output)
    common.require(pairs and len({name for name, _ in pairs}) == len(pairs), 'javap_native_declarations_invalid')
    return dict(pairs)


def validate_dex(dumped, expected):
    blocks = re.split(r'(?m)^Class #\d+\s+-\s*$', dumped)
    for owner, methods in expected.items():
        descriptor = 'L' + owner.replace('.', '/') + ';'
        found = [block for block in blocks if "Class descriptor  : '" + descriptor + "'" in block]
        common.require(len(found) == 1, 'dex_native_class_missing')
        for name, signature in methods.items():
            method = (r"(?ms)^\s*#\d+\s+:\s+\(in\s+" + re.escape(descriptor) +
                      r"\)\s*\n\s*name\s*:\s*'" + re.escape(name) +
                      r"'\s*\n\s*type\s*:\s*'" + re.escape(signature) +
                      r"'\s*\n\s*access\s*:\s*[^\n]*\bNATIVE\b")
            common.require(re.search(method, found[0]) is not None, 'dex_native_signature_missing')
    witness_descriptor = 'L' + WITNESS.replace('.', '/') + ';'
    common.require("Class descriptor  : '" + witness_descriptor + "'" not in dumped, 'shrink_witness_remains')


def diagnostic_key_input(path, build):
    if path is None: return None
    selected = path.resolve(strict=True)
    common.require(selected.is_relative_to(build.resolve()) and selected.is_file() and
                   not path.is_symlink(), 'diagnostic_key_must_remain_private_build_file')
    bounded_file(selected, MAX_EVIDENCE)
    return selected


def validate_copy_calls(dumped):
    descriptor = 'L' + INSTRUMENT.replace('.', '/') + ';'
    blocks = re.split(r'(?m)^Class #\d+\s+-\s*$', dumped)
    found = [block for block in blocks if "Class descriptor  : '" + descriptor + "'" in block]
    common.require(len(found) == 1 and len(re.findall(r'invoke-[^\n]*\.copyTo:', found[0])) >= 3,
                   'copy_to_instrumentation_calls_missing')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-zip', 'candidate-evidence', 'android-sdk', 'java-home', 'ndk', 'out-dir'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--abi', choices=('arm64-v8a', 'x86_64'), required=True)
    parser.add_argument('--diagnostic-keystore', type=Path, help='Optional existing private build/ managedprobe key; no uninstall or data clearing')
    parser.add_argument('--build-tools', default='37.0.0')
    parser.add_argument('--platform', default='android-37.0')
    args = parser.parse_args()
    common.require(re.fullmatch(r'\d+(?:\.\d+)*', args.build_tools) and
                   re.fullmatch(r'android-\d+(?:\.\d+)?', args.platform), 'sdk_version_invalid')
    build = common.workspace_directory('build')
    out = common.fresh_work_directory(args.out_dir, build)
    existing_key = diagnostic_key_input(args.diagnostic_keystore, build)
    existing_key_id = common.file_identity(existing_key) if existing_key is not None else None
    candidate = args.candidate_zip.resolve(strict=True)
    evidence = args.candidate_evidence.resolve(strict=True)
    common.require(candidate.is_file() and evidence.is_file() and candidate.is_relative_to(ROOT) and
                   evidence.is_relative_to(ROOT), 'candidate_inputs_outside_workspace')
    suffix = '.exe' if __import__('os').name == 'nt' else ''
    sdktools = args.android_sdk / 'build-tools' / args.build_tools
    jdk = args.java_home / 'bin'
    host = 'windows-x86_64' if suffix else 'linux-x86_64'
    llvm = args.ndk / 'toolchains/llvm/prebuilt' / host / 'bin'
    tools = {'java': jdk / ('java' + suffix), 'javac': jdk / ('javac' + suffix),
             'javap': jdk / ('javap' + suffix), 'jar': jdk / ('jar' + suffix),
             'keytool': jdk / ('keytool' + suffix), 'aapt2': sdktools / ('aapt2' + suffix),
             'zipalign': sdktools / ('zipalign' + suffix), 'dexdump': sdktools / ('dexdump' + suffix),
             'r8': sdktools / 'lib/d8.jar', 'apksigner': sdktools / 'lib/apksigner.jar',
             'android_jar': args.android_sdk / 'platforms' / args.platform / 'android.jar',
             'readelf': llvm / ('llvm-readelf' + suffix)}
    common.require(all(p.is_file() for p in tools.values()), 'required_tool_missing')
    common.require(managed_round.PACKAGE == PACKAGE and managed_round.INSTRUMENT == INSTRUMENT,
                   'probe_protocol_identity_mismatch')
    sources = [ROOT / 'tests/android_managed/AndroidManifest.xml',
               ROOT / 'tests/android_managed/ManagedInstrumentation.java',
               ROOT / 'tests/android_managed/ManagedShrinkWitness.java', Path(__file__).resolve(),
               ROOT / 'tools/android_managed_round.py', ROOT / 'tools/build_android_candidate.py',
               ROOT / 'tools/package_feature_preview.py', ROOT / 'tools/source_lock.py']
    common.require(all(p.is_file() and not p.is_symlink() for p in sources), 'probe_source_missing')
    tool_ids = {name: common.file_identity(path) for name, path in tools.items()}
    source_ids = {path.name: common.file_identity(path)['sha256'] for path in sources}
    record, candidate_bytes, evidence_bytes, aar, sdk, classes, rules, core, jni = candidate_inputs(candidate, evidence, args.abi)
    out.mkdir(parents=True, exist_ok=False)
    operations = []
    status = out / 'STATUS.json'
    status.write_bytes(common.json_bytes({'state': 'RUNNING', 'device_executed': False}))
    try:
        inputs = out / 'inputs'; inputs.mkdir()
        for name, data in (('MeguvizMemory.aar', aar), ('MeguvizMemory-sdk.zip', sdk), ('classes.jar', classes),
                           ('proguard.txt', rules), ('libmgmem.so', core), ('libmgmem_pressure_jni.so', jni)):
            with (inputs / name).open('xb') as stream: stream.write(data)
        native_expected = {}
        for klass in NATIVE_CLASSES:
            owner = JAVA_PACKAGE + klass
            output = common.run_tool([tools['javap'], '-private', '-s', '-classpath', inputs / 'classes.jar', owner],
                                     out, operations, 'javap-' + klass)
            native_expected[owner] = native_descriptors(output)
        common.require(native_expected[JAVA_PACKAGE + 'NativeMemoryLease'].get('nativeTransfer') == '(JJJJI)I',
                       'copy_to_native_declaration_missing')
        symbols = common.run_tool([tools['readelf'], '--dyn-syms', '--wide', inputs / 'libmgmem_pressure_jni.so'],
                                  out, operations, 'jni-symbols')
        expected_exports = {common.JAVA_PREFIX + klass + '_' + method
                            for klass in NATIVE_CLASSES for method in native_expected[JAVA_PACKAGE + klass]}
        common.validate_jni_symbols(symbols, expected_exports)
        compiled = out / 'classes'; compiled.mkdir()
        common.run_tool([tools['javac'], '--release', '8', '-classpath',
                         str(tools['android_jar']) + __import__('os').pathsep + str(inputs / 'classes.jar'),
                         '-d', compiled, sources[1], sources[2]], out, operations, 'javac-probe')
        probe_jar = out / 'probe.jar'
        common.run_tool([tools['jar'], '--create', '--file', probe_jar, '-C', compiled, '.'], out, operations, 'jar-probe')
        with zipfile.ZipFile(probe_jar) as archive:
            read_unique(archive, WITNESS.replace('.', '/') + '.class')
        dex = out / 'dex'; dex.mkdir()
        config = out / 'probe-r8.pro'; usage = out / 'r8-usage.txt'; mapping = out / 'r8-mapping.txt'
        config.write_text('-keep class ' + INSTRUMENT + ' { *; }\n-printusage ' + usage.resolve().as_posix() + '\n',
                          encoding='utf-8', newline='\n')
        common.run_tool([tools['java'], '-cp', tools['r8'], 'com.android.tools.r8.R8', '--release', '--min-api', '27',
                         '--lib', tools['android_jar'], '--pg-conf', inputs / 'proguard.txt', '--pg-conf', config,
                         '--pg-map-output', mapping, '--output', dex, inputs / 'classes.jar', probe_jar], out, operations, 'r8')
        dex_file = dex / 'classes.dex'
        common.require(sorted(p.name for p in dex.glob('*.dex')) == ['classes.dex'], 'dex_output_invalid')
        usage_text = usage.read_text(encoding='utf-8')
        common.require(re.search(r'(?m)^' + re.escape(WITNESS) + r'(?::.*)?$', usage_text) is not None,
                       'shrink_witness_not_removed')
        dumped = common.run_tool([tools['dexdump'], '-f', dex_file], out, operations, 'dexdump')
        validate_dex(dumped, native_expected)
        code_dump = common.run_tool([tools['dexdump'], '-d', dex_file], out, operations, 'dexdump-copy-calls')
        validate_copy_calls(code_dump)
        common.require("Class descriptor  : 'L" + INSTRUMENT.replace('.', '/') + ";'" in dumped,
                       'instrumentation_not_retained')
        unsigned = out / 'unsigned.apk'
        common.run_tool([tools['aapt2'], 'link', '-I', tools['android_jar'], '--manifest', sources[0],
                         '--min-sdk-version', '27', '--target-sdk-version', '33', '-o', unsigned], out, operations, 'aapt-link')
        with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_STORED) as package:
            package.write(dex_file, 'classes.dex')
            package.writestr('lib/' + args.abi + '/libmgmem.so', core)
            package.writestr('lib/' + args.abi + '/libmgmem_pressure_jni.so', jni)
        aligned = out / 'aligned.apk'
        common.run_tool([tools['zipalign'], '-P', '16', '-f', '4', unsigned, aligned], out, operations, 'zipalign')
        key = existing_key if existing_key is not None else out / 'diagnostic.keystore'
        if existing_key is None:
            common.run_tool([tools['keytool'], '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android',
                             '-alias', 'managedprobe', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '3650',
                             '-dname', 'CN=MeguvizMemory Managed Probe,O=MeguvizMemory', '-noprompt'], out, operations, 'diagnostic-key')
        apk = out / 'Managed-Probe.apk'
        common.run_tool([tools['java'], '-jar', tools['apksigner'], 'sign', '--ks', key, '--ks-pass', 'pass:android',
                         '--key-pass', 'pass:android', '--ks-key-alias', 'managedprobe', '--out', apk, aligned],
                        out, operations, 'sign')
        signature = common.run_tool([tools['java'], '-jar', tools['apksigner'], 'verify', '--print-certs', apk],
                                    out, operations, 'verify-signature')
        certs = re.findall(r'certificate SHA-256 digest:\s*([0-9a-fA-F]{64})', signature)
        common.require(len(certs) == 1, 'certificate_identity_invalid')
        common.run_tool([tools['zipalign'], '-c', '-P', '16', '-v', '4', apk], out, operations, 'verify-alignment')
        badging = common.run_tool([tools['aapt2'], 'dump', 'badging', apk], out, operations, 'verify-badging')
        common.require(re.findall(r"(?m)^package: name='([^']+)'", badging) == [PACKAGE] and
                       re.findall(r"(?m)^native-code: '([^']+)'\s*$", badging) == [args.abi],
                       'apk_package_or_abi_mismatch')
        tree = common.run_tool([tools['aapt2'], 'dump', 'xmltree', apk, '--file', 'AndroidManifest.xml'],
                               out, operations, 'verify-manifest')
        managed_round.verify_manifest_tree(tree)
        with zipfile.ZipFile(apk) as package:
            names = package.namelist()
            native_names = [name for name in names if name.startswith('lib/') and name.endswith('.so')]
            common.require(len(names) == len(set(names)) and set(native_names) == {
                'lib/' + args.abi + '/libmgmem.so', 'lib/' + args.abi + '/libmgmem_pressure_jni.so'}, 'apk_native_closure_invalid')
            common.require(read_unique(package, native_names[0]) == (core if native_names[0].endswith('/libmgmem.so') else jni) and
                           read_unique(package, native_names[1]) == (core if native_names[1].endswith('/libmgmem.so') else jni) and
                           read_unique(package, 'classes.dex') == dex_file.read_bytes(), 'apk_member_mismatch')
        common.require(bounded_file(candidate, MAX_CANDIDATE) == candidate_bytes and
                       bounded_file(evidence, MAX_EVIDENCE) == evidence_bytes and
                       all(common.file_identity(path) == tool_ids[name] for name, path in tools.items()) and
                       all(common.file_identity(path)['sha256'] == source_ids[path.name] for path in sources), 'build_inputs_changed')
        common.require(existing_key is None or common.file_identity(existing_key) == existing_key_id, 'diagnostic_key_changed')
        common.verify_tool_log_integrity(out, operations)
        receipt = {'schema_version': 1, 'state': 'PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION',
                   'scope': 'ISOLATED_MANAGED_DIAGNOSTIC_NOT_PRODUCTION_APK', 'device_executed': False,
                   'package_name': PACKAGE, 'instrumentation': INSTRUMENT, 'abi': args.abi,
                   'probe_scopes': list(managed_round.PROBE_SCOPES), 'diagnostic_key_reused': existing_key is not None,
                   'apk_sha256': digest(apk.read_bytes()), 'apk_bytes': apk.stat().st_size,
                   'certificate_sha256': certs[0].lower(), 'candidate_sha256': digest(candidate_bytes),
                   'candidate_source_commit': record['source_commit'], 'signing': 'PRIVATE_DIAGNOSTIC_ONLY',
                   'inputs': {'aar_sha256': digest(aar), 'shared_core_sha256': digest(core),
                              'shared_jni_sha256': digest(jni), 'classes_jar_sha256': digest(classes)},
                   'source_sha256': source_ids, 'tools': tool_ids,
                   'r8_evidence': {'dex_sha256': digest(dex_file.read_bytes()), 'instrumentation_retained': True,
                                   'managed_native_signatures_retained': True, 'native_method_count': len(expected_exports),
                                   'copy_to_native_signature': '(JJJJI)I', 'copy_to_calls_retained': True,
                                   'shrink_witness_removed': True, 'device_execution': 'NOT_RUN'},
                   'apk_manifest_readback': 'PASS_PACKAGE_INSTRUMENTATION_TARGET_AND_SDK',
                   'operations': operations,
                   'limits': ['Diagnostic R8 build does not prove ART, physical-device, real pressure or performance behavior.',
                              'Source and file hashes are not signed build provenance.',
                              'The diagnostic key and raw logs remain in the ignored build directory.']}
        with (out / 'receipt.json').open('xb') as stream: stream.write(common.json_bytes(receipt))
        status.write_bytes(common.json_bytes({'state': receipt['state'], 'device_executed': False}))
        print(json.dumps({'state': receipt['state'], 'abi': args.abi, 'apk_sha256': receipt['apk_sha256']}))
        return 0
    except BaseException as error:
        reason = str(error)
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', reason): reason = type(error).__name__
        status.write_bytes(common.json_bytes({'state': 'FAILED', 'reason': reason, 'device_executed': False}))
        raise


if __name__ == '__main__':
    raise SystemExit(main())
