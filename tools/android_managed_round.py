#!/usr/bin/env python3
"""Freeze or run one isolated Android managed-domain and synthetic-pressure functional probe.

The freeze command never invokes ADB. The run command uses an explicitly
selected device and only the diagnostic package named below. Raw command output
and pulled APKs remain in a fresh ignored build/ directory. Observations are
not a shared-device lock, and freezer samples are not continuous observation.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'artifacts/managed-device-20261101'
PACKAGE = 'io.github.jichuo1.meguviz.memory.managedprobe'
INSTRUMENT = 'io.github.jichuo1.meguviz.memory.android.ManagedInstrumentation'
COMPONENT = PACKAGE + '/' + INSTRUMENT
REPORT_SUITE = 'android-managed-pressure-v1'
COPY_REPORT_SUITE = 'android-managed-copy-arena-v1'
PROBE_SCOPES = ('pressure-v1', 'copy-arena-v1')
MIN_CHECKS = 302
MAX_CHECKS = 1000000
BUSY = re.compile(r'gpudiagnostic|androidx\.test|uiautomator|am instrument|'
                  r'dexmeguviz|dexdash|meguviz|mgmem|'
                  r'(?:^|[ /._-])(?:ncnn|rife|mgv|mgc)[^ ]*(?:bench|test)|' +
                  re.escape(PACKAGE), re.I)
SHA = re.compile(r'^[0-9a-f]{64}$')
SERIAL = re.compile(r'^[A-Za-z0-9._:-]{1,160}$')
ANDROID_PATH = re.compile(r'^/data/app/[A-Za-z0-9_./=+~-]+\.apk$')
UUID = re.compile(r'^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$')


class Blocked(RuntimeError):
    pass


def require(condition, reason):
    if not condition:
        raise Blocked(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    with Path(path).open('rb') as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
        return value.hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def workspace_file(path):
    path = Path(path).resolve()
    require(path.is_relative_to(ROOT) and path.is_file(), 'workspace_input_required')
    return path


def identity(path):
    path = workspace_file(path)
    return {'path': path.relative_to(ROOT).as_posix(), 'bytes': path.stat().st_size, 'sha256': digest(path)}


def atomic_receipt(path, value):
    """Fsync a new pending file; do not overwrite a failed predecessor."""
    path = Path(path)
    pending = path.with_suffix('.pending')
    data = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')
    try:
        with pending.open('xb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as error:
        raise Blocked('receipt_pending_already_exists') from error
    except OSError as error:
        raise Blocked('receipt_pending_write_failed') from error
    end = time.monotonic() + .9
    while True:
        try:
            os.replace(pending, path)
            return
        except PermissionError as error:
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise Blocked('receipt_replace_permission_denied') from error
            time.sleep(min(.025, remaining))
        except OSError as error:
            raise Blocked('receipt_replace_failed') from error


def unique_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate_json_key')
            result[key] = value
        return result
    def constant(_):
        raise ValueError('nonfinite_json_value')
    return json.loads(text, object_pairs_hook=pairs, parse_constant=constant)


def scope_suite(scope):
    require(scope in PROBE_SCOPES, 'invalid_probe_scope')
    return COPY_REPORT_SUITE if scope == 'copy-arena-v1' else REPORT_SUITE


def verify_feature_scope(built, scope):
    scope_suite(scope)
    if scope == 'copy-arena-v1':
        evidence = built.get('r8_evidence', {})
        require(scope in built.get('probe_scopes', []) and
                evidence.get('copy_to_native_signature') == '(JJJJI)I' and
                evidence.get('copy_to_calls_retained') is True, 'copy_arena_build_evidence_missing')


def instrument_command(adb, serial, user_id, scope):
    scope_suite(scope)
    command = [str(adb), '-s', serial, 'shell', 'am', 'instrument', '-w', '-r', '--user', user_id]
    if scope == 'copy-arena-v1': command += ['-e', 'probe_scope', scope]
    return command + [COMPONENT]


def parse_report(stdout, transport_code, probe_scope='pressure-v1'):
    require(transport_code == 0, 'instrument_transport_not_successful')
    text = stdout.decode('utf-8', 'strict') if isinstance(stdout, bytes) else stdout
    text = text.replace('\r\n', '\n')
    require(text.endswith('\n') and len([x for x in text.splitlines() if x.strip()]) == 2,
            'instrument_output_truncated_or_unexpected')
    results = list(re.finditer(r'(?m)^INSTRUMENTATION_RESULT: report_json=([^\n]*)$', text))
    codes = list(re.finditer(r'(?m)^INSTRUMENTATION_CODE: ([^\n]*)$', text))
    require(len(results) == len(codes) == 1 and results[0].end() < codes[0].start(), 'instrument_result_count_or_order')
    try:
        report = unique_json(results[0].group(1))
    except (ValueError, TypeError) as error:
        raise Blocked('instrument_report_invalid_json') from error
    require(isinstance(report, dict) and report.get('suite') == scope_suite(probe_scope) and type(report.get('checks')) is int,
            'instrument_report_identity')
    if report.get('status') == 'FAIL':
        require(codes[0].group(1) == '1' and set(report) == {'suite', 'status', 'checks', 'stage'} and
                0 <= report['checks'] <= MAX_CHECKS and report['stage'] in {
                    'start', 'system_create', 'arena_create', 'system_budget', 'arena_budget',
                    'system_close', 'arena_close', 'callback_setup', 'callback_concurrent_close'} |
                    ({'system_copy_arena', 'arena_copy_arena'} if probe_scope == 'copy-arena-v1' else set()),
                'instrument_failure_report_invalid')
        return report
    expected_fields = {'suite', 'status', 'checks', 'case_count', 'final_backing_bytes', 'callback_origin',
                       'queued_implies_applied', 'performance_qualified'}
    extended = probe_scope == 'copy-arena-v1'
    if extended: expected_fields |= {'copy_to_cases', 'arena_same_shape_allocations'}
    require(codes[0].group(1) == '0' and set(report) == expected_fields and report.get('status') == 'PASS' and
        (1100 if extended else MIN_CHECKS) <= report['checks'] <= MAX_CHECKS and
        type(report['case_count']) is int and report['case_count'] == (5 if extended else 3) and
        type(report['final_backing_bytes']) is int and report['final_backing_bytes'] == 0 and
        report['callback_origin'] == 'synthetic' and report['queued_implies_applied'] is False and
        report['performance_qualified'] is False, 'instrument_report_scope_or_checks')
    if extended:
        require(type(report['copy_to_cases']) is int and report['copy_to_cases'] == 2 and
                type(report['arena_same_shape_allocations']) is int and report['arena_same_shape_allocations'] == 96,
                'instrument_feature_marker_mismatch')
    return report


def parse_gate(ps, activity):
    lines = [line.strip() for line in ps.splitlines() if line.strip()]
    require(lines and re.fullmatch(r'PID\s+NAME\s+ARGS', lines[0]) is not None,
            'process_inventory_header_missing')
    rows = [line.split(None, 2) for line in lines[1:]]
    require(all(len(row) >= 2 and row[0].isdigit() and row[1] for row in rows) and
            len({row[0] for row in rows}) == len(rows) and
            len(rows) >= 5 and any(row[1] == 'system_server' for row in rows) and
            any(row[1] == 'init' for row in rows), 'process_inventory_unparseable')
    busy = [row for row in rows if BUSY.search(' '.join(row[1:]))]
    active = []
    for line in activity.splitlines():
        if not re.search(r'(?i)activeinstrumentation', line):
            continue
        empty = re.search(r'\bmActiveInstrumentation\s*[:=]\s*(?:null|None|\[\]|\{\}|0)\s*$', line)
        if not empty:
            active.append(line)
    require('ACTIVITY MANAGER ACTIVITIES (dumpsys activity activities)' in activity and
            'ACTIVITY MANAGER RUNNING PROCESSES (dumpsys activity processes)' in activity and
            not re.search(r'(?i)permission\s+denial|permission\s+denied|can.t find service|error dumping service', activity),
            'activity_inventory_unparseable')
    return {'parsed_processes': len(rows), 'matching_test_processes': len(busy),
            'active_instrumentation': len(active),
            'result': 'PASS_OBSERVATIONAL_ONLY' if not busy and not active else 'BLOCKED'}


def parse_package_inventory(text):
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    require(bool(lines) and all(re.fullmatch(r'package:[A-Za-z0-9_.]+', line) for line in lines),
            'package_inventory_unparseable')
    require(len(lines) == len(set(lines)), 'package_inventory_ambiguous')
    return ('package:' + PACKAGE) in lines


def parse_package_user_states(text, user_ids, installed):
    # ADB raw stdout uses CRLF on this device; preserve the raw log and normalize only parsing.
    text = text.replace('\r\n', '\n')
    # Queries also contains bare User lines. Only the single package record in
    # the Packages section describes per-user installation and data state.
    package_header = r'(?m)^  Package \[' + re.escape(PACKAGE) + r'\] \([^\n]*\):[ \t]*$'
    starts = list(re.finditer(package_header, text))
    require(len(starts) == 1 and 'Packages:' in text[:starts[0].start()], 'package_user_state_unparseable')
    body = text[starts[0].end():]
    end = re.search(r'(?m)^(?:\S|  Package \[)', body)
    if end: body = body[:end.start()]
    rows = re.findall(r'(?m)^    User (\d+):[ \t]+([^\n]+)$', body)
    require(len(rows) == len(user_ids) and {uid for uid, _ in rows} == set(user_ids),
            'package_user_state_unparseable')
    result = {}
    for uid, line in rows:
        fields = [part.split('=', 1) for part in line.split()]
        require(uid not in result and all(len(part) == 2 for part in fields) and
                len({part[0] for part in fields}) == len(fields), 'package_user_state_unparseable')
        fields = dict(fields)
        require(all(fields.get(name) in ('true', 'false') for name in ('installed', 'stopped', 'notLaunched')) and
                re.fullmatch(r'0|[1-9][0-9]*', fields.get('ceDataInode', '')) is not None,
                'package_user_state_unparseable')
        is_installed = fields['installed'] == 'true'
        require(is_installed == installed[uid], 'package_user_installation_disagrees')
        empty = (not is_installed and fields['ceDataInode'] == '0' and
                 fields['stopped'] == 'true' and fields['notLaunched'] == 'true')
        result[uid] = 'INSTALLED' if is_installed else 'UNINSTALLED_EMPTY' if empty else 'RETAINED_OR_UNKNOWN'
    return result


def parse_package_paths(text):
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    require(all(line.startswith('package:') for line in lines), 'package_path_unparseable')
    paths = [line[len('package:'):] for line in lines]
    require(len(paths) <= 1 and all(ANDROID_PATH.fullmatch(path) for path in paths), 'package_path_ambiguous')
    return paths


def safe_reason(error):
    value = str(error)
    if isinstance(error, Blocked) and re.fullmatch(r'[A-Za-z0-9_.-]{1,120}', value):
        return value
    if isinstance(error, subprocess.TimeoutExpired):
        return 'transport_timeout'
    return 'unexpected_' + type(error).__name__


def verify_manifest_tree(tree):
    lines = tree.splitlines()
    elements = [(i, match.group(1)) for i, line in enumerate(lines)
                if (match := re.match(r'^\s*E: ([A-Za-z0-9_-]+) \(', line))]
    require([kind for _, kind in elements] == ['manifest', 'uses-sdk', 'application', 'instrumentation'],
            'local_apk_manifest_scope_mismatch')
    def direct_attrs(index):
        indent = len(lines[index]) - len(lines[index].lstrip())
        attrs = {}
        for line in lines[index + 1:]:
            level = len(line) - len(line.lstrip())
            if line.strip() and level <= indent:
                break
            if level != indent + 2:
                continue
            match = re.match(r'^\s*A: http://schemas\.android\.com/apk/res/android:([A-Za-z0-9_]+)\(0x[0-9a-f]+\)=(.+)$', line)
            if match:
                require(match.group(1) not in attrs, 'local_apk_manifest_duplicate_attribute')
                attrs[match.group(1)] = match.group(2)
        return attrs
    manifest = direct_attrs(elements[0][0])
    sdk = direct_attrs(elements[1][0])
    require(sdk == {'minSdkVersion': '27', 'targetSdkVersion': '33'}, 'local_apk_sdk_mismatch')
    application = direct_attrs(elements[2][0])
    instrument = direct_attrs(elements[3][0])
    require(set(manifest) <= {'compileSdkVersion', 'compileSdkVersionCodename'} and
            set(application) == {'label', 'debuggable', 'allowBackup'} and
            application['debuggable'] == 'true' and application['allowBackup'] == 'false' and
            set(instrument) == {'name', 'targetPackage', 'functionalTest'} and
            re.match(r'^"' + re.escape(INSTRUMENT) + r'"(?:\s|$)', instrument['name']) is not None and
            re.match(r'^"' + re.escape(PACKAGE) + r'"(?:\s|$)', instrument['targetPackage']) is not None and
            instrument['functionalTest'] == 'true', 'local_apk_instrumentation_target_mismatch')


def verify_local(apk, aar, sdk_core, build_receipt, abi, expected_certificate, aapt, java, signer, probe_scope='pressure-v1'):
    built = unique_json(build_receipt.read_text(encoding='utf-8-sig'))
    require(built.get('state') == 'PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION' and
            built.get('device_executed') is False and built.get('package_name') == PACKAGE and
            built.get('instrumentation') == INSTRUMENT and built.get('abi') == abi,
            'build_receipt_scope_mismatch')
    inputs = built.get('inputs', {})
    r8 = built.get('r8_evidence', {})
    verify_feature_scope(built, probe_scope)
    require(built.get('apk_sha256') == digest(apk) and built.get('apk_bytes') == apk.stat().st_size and
            built.get('certificate_sha256') == expected_certificate and inputs.get('aar_sha256') == digest(aar),
            'build_receipt_identity_mismatch')
    require(r8.get('instrumentation_retained') is True and r8.get('managed_native_signatures_retained') is True and
            r8.get('shrink_witness_removed') is True, 'r8_build_evidence_missing')
    with zipfile.ZipFile(apk) as app, zipfile.ZipFile(aar) as library:
        require(len(app.namelist()) == len(set(app.namelist())) and
                len(library.namelist()) == len(set(library.namelist())), 'duplicate_package_member')
        libs = ['libmgmem.so', 'libmgmem_pressure_jni.so']
        require(sorted(x for x in app.namelist() if x.endswith('.so')) ==
                sorted('lib/' + abi + '/' + x for x in libs) and
                sorted(x for x in library.namelist() if x.endswith('.so')) ==
                sorted('jni/' + abi + '/' + x for x in libs), 'native_closure_mismatch')
        regular = {'AndroidManifest.xml', 'resources.arsc', 'classes.dex'} | {'lib/' + abi + '/' + x for x in libs}
        signatures = [x for x in app.namelist() if x.startswith('META-INF/')]
        require(set(app.namelist()) == regular | set(signatures) and
                'META-INF/MANIFEST.MF' in signatures and
                len([x for x in signatures if re.fullmatch(r'META-INF/[A-Z0-9_-]+\.SF', x)]) == 1 and
                len([x for x in signatures if re.fullmatch(r'META-INF/[A-Z0-9_-]+\.(?:RSA|DSA|EC)', x)]) == 1 and
                len(signatures) == 3, 'apk_unexpected_package_members')
        core, jni = (app.read('lib/' + abi + '/' + name) for name in libs)
        machine = {'arm64-v8a': 183, 'x86_64': 62}[abi]
        require(all(len(item) >= 20 and item[:6] == b'\x7fELF\x02\x01' and
                    int.from_bytes(item[18:20], 'little') == machine for item in (core, jni)), 'native_architecture_mismatch')
        require(core == library.read('jni/' + abi + '/' + libs[0]) == sdk_core.read_bytes() and
                jni == library.read('jni/' + abi + '/' + libs[1]) and
                sha(core) == inputs.get('shared_core_sha256') and sha(jni) == inputs.get('shared_jni_sha256') and
                sha(library.read('classes.jar')) == inputs.get('classes_jar_sha256') and
                sha(app.read('classes.dex')) == r8.get('dex_sha256'), 'apk_member_identity_mismatch')
    def host(command):
        result = subprocess.run([str(x) for x in command], capture_output=True, timeout=30, check=False)
        require(result.returncode == 0, 'local_apk_inspection_failed')
        return result.stdout.decode('utf-8', 'replace')
    cert = host([java, '-jar', signer, 'verify', '--print-certs', apk])
    certificates = re.findall(r'certificate SHA-256 digest:\s*([0-9a-fA-F]{64})', cert)
    require(len(certificates) == 1 and certificates[0].lower() == expected_certificate, 'local_apk_signer_mismatch')
    badging = host([aapt, 'dump', 'badging', apk])
    require(re.findall(r"(?m)^package: name='([^']+)'", badging) == [PACKAGE] and
            re.findall(r"(?m)^native-code: '([^']+)'\s*$", badging) == [abi], 'local_apk_package_or_abi_mismatch')
    versions = [line.strip() for line in badging.splitlines()
                if re.match(r'^\s*(?:minSdkVersion|targetSdkVersion)\b', line, re.I)]
    require(versions == ["minSdkVersion:'27'", "targetSdkVersion:'33'"], 'local_apk_badging_sdk_mismatch')
    tree = host([aapt, 'dump', 'xmltree', apk, '--file', 'AndroidManifest.xml'])
    verify_manifest_tree(tree)
    return built



def source_files():
    return {
        'AndroidManifest.xml': ROOT / 'tests/android_managed/AndroidManifest.xml',
        'ManagedInstrumentation.java': ROOT / 'tests/android_managed/ManagedInstrumentation.java',
        'ManagedShrinkWitness.java': ROOT / 'tests/android_managed/ManagedShrinkWitness.java',
        'build_android_managed_probe.py': ROOT / 'tools/build_android_managed_probe.py',
        'android_managed_round.py': Path(__file__).resolve(),
        'build_android_candidate.py': ROOT / 'tools/build_android_candidate.py',
        'package_feature_preview.py': ROOT / 'tools/package_feature_preview.py',
        'source_lock.py': ROOT / 'tools/source_lock.py',
    }


def verify_build_sources(built):
    sources = source_files()
    require(isinstance(built.get('source_sha256'), dict) and set(sources).issubset(built['source_sha256']),
            'build_source_list_mismatch')
    require(all(digest(path) == built['source_sha256'][name] for name, path in sources.items()), 'build_source_drift')
    return sources


def freeze(args):
    probe_scope = getattr(args, 'probe_scope', 'pressure-v1')
    expected_suite = scope_suite(probe_scope)
    require(SERIAL.fullmatch(args.serial) is not None, 'invalid_explicit_serial')
    require(27 <= args.api <= 100 and args.page_bytes in (4096, 16384), 'invalid_device_profile')
    require(15 <= args.timeout_seconds <= 300, 'invalid_timeout')
    output = (Path(args.output) if args.output is not None else
              ROOT / 'build' / 'managed-device-plans' / secrets.token_hex(16) / 'plan.json').resolve()
    in_legacy = output.is_relative_to(BASE.resolve())
    build_root = (ROOT / 'build').resolve()
    in_private = output.is_relative_to(build_root) and not in_legacy
    require((in_legacy or in_private) and not output.exists(), 'fresh_scoped_plan_required')
    if in_private:
        require(output.parent != build_root and not output.parent.exists(), 'fresh_private_plan_directory_required')
    apk, aar, sdk_core, build = map(workspace_file, (args.apk, args.aar, args.sdk_core, args.build_receipt))
    tool_paths = {'adb': Path(args.adb).resolve(), 'aapt2': Path(args.aapt2).resolve(),
                  'java': Path(args.java).resolve(), 'apksigner': Path(args.apksigner).resolve()}
    require(all(p.is_file() for p in tool_paths.values()), 'local_tool_missing')
    preliminary = unique_json(build.read_text(encoding='utf-8-sig'))
    abi, cert = preliminary.get('abi'), preliminary.get('certificate_sha256')
    require(abi in ('arm64-v8a', 'x86_64') and isinstance(cert, str) and SHA.fullmatch(cert), 'invalid_build_identity')
    built = verify_local(apk, aar, sdk_core, build, abi, cert, tool_paths['aapt2'], tool_paths['java'], tool_paths['apksigner'], probe_scope)
    source_paths = verify_build_sources(built)
    files = [identity(path) for path in [apk, aar, sdk_core, build, ROOT / 'tools/test_android_managed_round.py', *source_paths.values()]]
    require(len({entry['path'] for entry in files}) == len(files), 'duplicate_frozen_input')
    value = {'schema_version': 1, 'state': 'FROZEN_BEFORE_DEVICE_EXECUTION',
             'scope': 'OWN_R8_MANAGED_SYNTHETIC_PRESSURE_NO_BIL_NO_PERFORMANCE', 'frozen_at_utc': utc(),
             'expected_report_suite': expected_suite,
             'device_serial_sha256': sha(args.serial.encode()), 'device': {'api': args.api, 'abi': abi, 'page_bytes': args.page_bytes},
             'timeout_seconds': args.timeout_seconds, 'package': PACKAGE, 'component': COMPONENT,
             'certificate_sha256': cert, 'apk_sha256': digest(apk), 'aar_sha256': digest(aar),
             'core_sha256': built['inputs']['shared_core_sha256'], 'jni_sha256': built['inputs']['shared_jni_sha256'],
             'build_receipt_sha256': digest(build),
             'tools': {name: digest(path) for name, path in tool_paths.items()}, 'files': files,
             'plan_rules': ['One explicitly selected serial; before/after process observations do not provide a shared lock.',
                 'Observed own cgroup freeze or timeout is INCOMPLETE. No sample means NOT_OBSERVED, not proof of no freeze.',
                 'Instrumentation starts in the background without a foreground bootstrap or restart workaround.',
                 'No remote force-stop; transport termination does not prove remote process termination.',
                 'No uninstall, data clearing, global freezer/power policy change, or unrelated process termination.',
                 'Only exact diagnostic APK package may be installed; existing package must have the same signer.',
                 'Source and artifact hashes identify frozen inputs; they do not independently prove build provenance.']}
    if probe_scope == 'copy-arena-v1':
        value['probe_scope'] = probe_scope
        value['scope'] = 'OWN_R8_MANAGED_COPY_ARENA_WITH_SYNTHETIC_PRESSURE_NO_BIL_NO_PERFORMANCE'
    output.parent.mkdir(parents=True, exist_ok=not in_private)
    with output.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
    print(json.dumps({'state': value['state'], 'plan_sha256': digest(output), 'apk_sha256': value['apk_sha256'],
                      'plan_path': output.relative_to(ROOT).as_posix(), 'plan_visibility': 'PRIVATE_DO_NOT_PUBLISH'}))
    return output


def validate_plan(plan, args):
    probe_scope = getattr(args, 'probe_scope', 'pressure-v1')
    require(plan.get('probe_scope', 'pressure-v1') == probe_scope, 'plan_probe_scope_mismatch')
    expected_suite = scope_suite(probe_scope)
    require(plan.get('state') == 'FROZEN_BEFORE_DEVICE_EXECUTION' and plan.get('schema_version') == 1 and
            plan.get('expected_report_suite') == expected_suite and
            plan.get('package') == PACKAGE and plan.get('component') == COMPONENT and
            SERIAL.fullmatch(args.serial) is not None and
            sha(args.serial.encode()) == plan.get('device_serial_sha256'), 'plan_target_mismatch')
    require(15 <= plan.get('timeout_seconds', 0) <= 300 and plan.get('device', {}).get('abi') in ('arm64-v8a', 'x86_64'), 'plan_profile_invalid')
    files = plan.get('files')
    require(isinstance(files, list) and len(files) >= 10 and len({x.get('path') for x in files}) == len(files), 'plan_files_invalid')
    for item in files:
        path = workspace_file(ROOT / item['path'])
        require(path.stat().st_size == item['bytes'] and digest(path) == item['sha256'], 'frozen_input_drift')
    paths = {'apk': workspace_file(args.apk), 'aar': workspace_file(args.aar),
             'sdk_core': workspace_file(args.sdk_core), 'build': workspace_file(args.build_receipt)}
    locked = {item['path']: item for item in files}
    required_sources = [*source_files().values(), ROOT / 'tools/test_android_managed_round.py']
    require(all(path.resolve().relative_to(ROOT).as_posix() in locked for path in required_sources), 'plan_missing_required_source')
    require(all(path.relative_to(ROOT).as_posix() in locked for path in paths.values()), 'plan_does_not_lock_inputs')
    require(digest(paths['apk']) == plan['apk_sha256'] and digest(paths['aar']) == plan['aar_sha256'] and
            digest(paths['sdk_core']) == plan['core_sha256'] and digest(paths['build']) == plan['build_receipt_sha256'] and
            Path(__file__).resolve().relative_to(ROOT).as_posix() in locked, 'plan_primary_identity_mismatch')
    tool_paths = {'adb': Path(args.adb).resolve(), 'aapt2': Path(args.aapt2).resolve(),
                  'java': Path(args.java).resolve(), 'apksigner': Path(args.apksigner).resolve()}
    require(set(plan.get('tools', {})) == set(tool_paths) and
            all(path.is_file() and digest(path) == plan['tools'][name] for name, path in tool_paths.items()), 'frozen_tool_drift')
    built = verify_local(paths['apk'], paths['aar'], paths['sdk_core'], paths['build'], plan['device']['abi'],
                         plan['certificate_sha256'], tool_paths['aapt2'], tool_paths['java'], tool_paths['apksigner'], probe_scope)
    verify_build_sources(built)
    return paths, tool_paths


class Device:
    def __init__(self, args, out, private, receipt):
        self.args, self.out, self.private, self.receipt = args, out, private, receipt
        self.index = 0
        self.deadline = None

    def command(self, label, parts, timeout=20, allow_nonzero=False):
        self.index += 1
        if self.deadline is not None:
            timeout = min(timeout, max(.05, self.deadline - time.monotonic()))
        try:
            result = subprocess.run([str(self.args.adb), '-s', self.args.serial, *map(str, parts)],
                                    capture_output=True, timeout=timeout, check=False)
            stdout, stderr, code, timed_out = result.stdout, result.stderr, result.returncode, False
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code, timed_out = error.stdout or b'', error.stderr or b'', None, True
        stem = f'{self.index:04d}-{label}'
        (self.private / (stem + '.stdout')).write_bytes(stdout)
        (self.private / (stem + '.stderr')).write_bytes(stderr)
        self.receipt['commands'].append({'name': label, 'exit_code': code, 'timed_out': timed_out,
                                         'stdout_sha256': sha(stdout), 'stderr_sha256': sha(stderr)})
        require(not timed_out, label + '_timeout')
        require(allow_nonzero or code == 0, label + '_failed')
        require(not re.search(rb'(?im)^\s*(?:error:|permission denial|permission denied|unknown command)', stdout + b'\n' + stderr), label + '_error')
        return code, stdout.decode('utf-8', 'replace'), stderr.decode('utf-8', 'replace')

    def gate(self, label):
        _, ps, _ = self.command(label + '-processes', ['shell', 'ps', '-A', '-o', 'PID,NAME,ARGS'])
        _, activities, _ = self.command(label + '-activity', ['shell', 'dumpsys', 'activity'])
        result = parse_gate(ps, activities)
        result['name'] = label
        self.receipt['gates'].append(result)
        require(result['result'] == 'PASS_OBSERVATIONAL_ONLY', label + '_other_test_or_own_process_present')

    def pid(self):
        code, text, _ = self.command('own-pid', ['shell', 'pidof', PACKAGE], timeout=4, allow_nonzero=True)
        parts = text.split()
        require(code in (0, 1) and (not parts or all(x.isdigit() for x in parts)), 'own_pid_unparseable')
        require(len(parts) <= 1, 'own_pid_ambiguous')
        return parts[0] if parts else None

    def freeze_sample(self, pid, uid):
        expected = '/uid_' + uid + '/pid_' + pid
        _, cgroup, _ = self.command('own-cgroup', ['shell', 'run-as', PACKAGE, 'cat', '/proc/' + pid + '/cgroup'], timeout=4)
        require('0::' + expected in cgroup.splitlines(), 'own_cgroup_identity_unavailable')
        base = '/sys/fs/cgroup' + expected
        _, mode, _ = self.command('own-freeze', ['shell', 'cat', base + '/cgroup.freeze'], timeout=4)
        _, events, _ = self.command('own-events', ['shell', 'cat', base + '/cgroup.events'], timeout=4)
        fields = dict(line.split(None, 1) for line in events.splitlines() if len(line.split(None, 1)) == 2)
        require(mode.strip() in ('0', '1') and fields.get('frozen') in ('0', '1'), 'own_cgroup_state_unavailable')
        return {'mode': mode.strip(), 'frozen': fields['frozen'], 'cgroup_path_sha256': sha(expected.encode()),
                'pid_sha256': sha(pid.encode()), 'observed_at_utc': utc()}

    def process_identity(self, pid, uid):
        _, stat, _ = self.command('own-process-stat', ['shell', 'run-as', PACKAGE, 'cat', '/proc/' + pid + '/stat'], timeout=4)
        _, status, _ = self.command('own-process-status', ['shell', 'run-as', PACKAGE, 'cat', '/proc/' + pid + '/status'], timeout=4)
        end = stat.rfind(')')
        fields = stat[end + 1:].split() if end >= 0 else []
        match = re.search(r'(?m)^Uid:\s*(\d+)(?:\s|$)', status)
        require(stat.startswith(pid + ' (') and len(fields) >= 20 and fields[19].isdigit() and
                match is not None and match.group(1) == uid, 'own_process_identity_unavailable')
        return sha((pid + ':' + uid + ':' + fields[19]).encode())


def run(args):
    require(SERIAL.fullmatch(args.serial) is not None, 'invalid_explicit_serial')
    plan_path = workspace_file(args.plan)
    require(SHA.fullmatch(args.plan_sha256) is not None and digest(plan_path) == args.plan_sha256,
            'frozen_plan_sha256_mismatch')
    plan = unique_json(plan_path.read_text(encoding='utf-8-sig'))
    paths, tools = validate_plan(plan, args)  # No ADB before all frozen/local checks.
    out, private = Path(args.out_dir).resolve(), Path(args.private_dir).resolve()
    require(out.is_relative_to(BASE.resolve()) and not out.exists(), 'fresh_scoped_receipt_directory_required')
    require(private.is_relative_to((ROOT / 'build').resolve()) and not private.exists(), 'fresh_private_directory_required')
    out.mkdir(parents=True); private.mkdir(parents=True)
    receipt = {'schema_version': 1, 'state': 'PREPARING', 'scope': plan['scope'],
               'plan_sha256': digest(plan_path), 'device_serial_sha256': sha(args.serial.encode()),
               'apk_sha256': plan['apk_sha256'], 'aar_sha256': plan['aar_sha256'],
               'core_sha256': plan['core_sha256'], 'jni_sha256': plan['jni_sha256'],
               'certificate_sha256': plan['certificate_sha256'], 'started_at_utc': utc(),
               'gates': [], 'commands': [], 'cgroup_samples': [], 'freezer_observation': 'NOT_OBSERVED',
               'report_validated': False, 'normal_domain_closed_proven': False, 'shared_device_lock': False,
               'remote_process_state': 'UNKNOWN_POSTFLIGHT_NOT_COMPLETED',
               'raw_outputs': 'PRIVATE_IGNORED_BUILD_ONLY', 'performance': 'NOT_MEASURED', 'BIL': 'NOT_INTEGRATED'}
    save = lambda: atomic_receipt(out / 'receipt.json', receipt)
    save()
    device = Device(args, out, private, receipt)
    child = None; stdout_file = None; stderr_file = None; target_pid = None; target_identity = None; user_id = None
    start = None; stop_reason = None
    try:
        device.gate('initial')
        _, api, _ = device.command('api', ['shell', 'getprop', 'ro.build.version.sdk'])
        _, abi, _ = device.command('abi', ['shell', 'getprop', 'ro.product.cpu.abi'])
        _, page, _ = device.command('page', ['shell', 'getconf', 'PAGESIZE'])
        require(api.strip().isdigit() and page.strip().isdigit() and
                {'api': int(api.strip()), 'abi': abi.strip(), 'page_bytes': int(page.strip())} == plan['device'], 'device_profile_mismatch')
        receipt['device'] = plan['device']
        _, boot, _ = device.command('boot-before', ['shell', 'cat', '/proc/sys/kernel/random/boot_id'])
        require(UUID.fullmatch(boot.strip()) is not None, 'boot_identity_unavailable')
        receipt['boot_id_sha256'] = sha(boot.strip().encode())
        _, current, _ = device.command('current-user', ['shell', 'am', 'get-current-user'])
        user_id = current.strip(); require(user_id.isdigit(), 'current_user_unavailable')
        receipt['android_user_sha256'] = sha(user_id.encode())
        _, users_text, _ = device.command('user-inventory', ['shell', 'pm', 'list', 'users'])
        user_lines = [line.strip() for line in users_text.splitlines() if line.strip()]
        require(user_lines and user_lines[0] == 'Users:', 'user_inventory_unparseable')
        matches = [re.fullmatch(r'UserInfo\{(\d+):[^}]*\}(?:\s+.*)?', line) for line in user_lines[1:]]
        require(matches and all(matches) and len(matches) <= 32, 'user_inventory_unparseable')
        user_ids = [match.group(1) for match in matches]
        require(user_id in user_ids and len(set(user_ids)) == len(user_ids), 'user_inventory_ambiguous')
        presence, installed = {}, {}
        for candidate in user_ids:
            _, listed, _ = device.command('package-inventory-user', ['shell', 'pm', 'list', 'packages', '--user', candidate, '-u'])
            _, actual, _ = device.command('installed-package-inventory-user', ['shell', 'pm', 'list', 'packages', '--user', candidate])
            presence[candidate] = parse_package_inventory(listed)
            installed[candidate] = parse_package_inventory(actual)
            require(not installed[candidate] or presence[candidate], 'package_inventory_disagrees')
        states = {}
        if any(presence.values()):
            _, dumped, _ = device.command('package-user-state', ['shell', 'dumpsys', 'package', PACKAGE])
            states = parse_package_user_states(dumped, user_ids, installed)
        listed_own = installed[user_id]
        other_user_package = any(state != 'UNINSTALLED_EMPTY' for uid, state in states.items() if uid != user_id)
        receipt['android_user_count'] = len(user_ids)
        receipt['other_user_package_present'] = other_user_package
        receipt['known_package_user_count'] = sum(presence.values())
        receipt['installed_package_user_count'] = sum(installed.values())
        receipt['other_user_known_empty_count'] = sum(uid != user_id and state == 'UNINSTALLED_EMPTY'
                                                     for uid, state in states.items())
        code, existing, stderr = device.command('package-path-before', ['shell', 'pm', 'path', PACKAGE], allow_nonzero=True)
        paths_before = parse_package_paths(existing)
        require(code in (0, 1) and not stderr.strip(), 'existing_package_path_ambiguous')
        require(bool(paths_before) == listed_own, 'existing_package_state_ambiguous')
        if paths_before:
            installed_before = private / 'installed-before.apk'
            device.command('pull-existing-own-apk', ['pull', paths_before[0], installed_before], timeout=60)
            signer = subprocess.run([str(tools['java']), '-jar', str(tools['apksigner']), 'verify', '--print-certs', str(installed_before)],
                                    capture_output=True, timeout=30, check=False)
            (private / 'existing-signature.stdout').write_bytes(signer.stdout)
            (private / 'existing-signature.stderr').write_bytes(signer.stderr)
            certificates = re.findall(rb'certificate SHA-256 digest:\s*([0-9a-fA-F]{64})', signer.stdout)
            require(signer.returncode == 0 and len(certificates) == 1 and
                    certificates[0].decode().lower() == plan['certificate_sha256'], 'existing_package_signer_mismatch')
            receipt['existing_package'] = {'state': 'SAME_SIGNER', 'apk_sha256': digest(installed_before)}
        else:
            receipt['existing_package'] = {'state': 'ABSENT_FROM_OBSERVED_USER_INVENTORY'}
        device.gate('before-install')
        if not paths_before or digest(installed_before) != plan['apk_sha256']:
            require(not other_user_package, 'same_package_present_for_other_user')
            install_args = ['install'] + (['-r'] if paths_before else []) + ['--user', user_id, paths['apk']]
            receipt['installation'] = 'REQUEST_IN_FLIGHT_OUTCOME_UNKNOWN'
            save()
            _, result, _ = device.command('install-own-diagnostic', install_args, timeout=120)
            require(re.search(r'(?m)^Success\s*$', result) is not None, 'install_not_confirmed')
            receipt['installation'] = 'COMMAND_SUCCESS_INSTALLED_BYTES_NOT_YET_VERIFIED'
            save()
        else:
            receipt['installation'] = 'EXACT_APK_ALREADY_INSTALLED_NO_UPDATE'
        _, current_path, _ = device.command('package-path-after-install', ['shell', 'pm', 'path', PACKAGE])
        values = parse_package_paths(current_path)
        require(len(values) == 1, 'installed_package_path_unavailable')
        installed = private / 'installed-after.apk'
        device.command('pull-installed-own-apk', ['pull', values[0], installed], timeout=60)
        require(digest(installed) == plan['apk_sha256'], 'installed_apk_bytes_mismatch')
        receipt['installed_apk_sha256'] = digest(installed)
        if receipt['installation'] != 'EXACT_APK_ALREADY_INSTALLED_NO_UPDATE':
            receipt['installation'] = 'OWN_PACKAGE_INSTALLED_OR_SAME_SIGNER_UPDATED_BYTES_VERIFIED_NO_DATA_CLEAR'
        _, uid, _ = device.command('own-uid', ['shell', 'run-as', PACKAGE, 'id', '-u'])
        uid = uid.strip(); require(uid.isdigit(), 'own_uid_unavailable')
        device.gate('before-instrumentation')
        save()
        stdout_file = (private / 'instrumentation.stdout').open('wb')
        stderr_file = (private / 'instrumentation.stderr').open('wb')
        child = subprocess.Popen(instrument_command(args.adb, args.serial, user_id,
                                  plan.get('probe_scope', 'pressure-v1')), stdout=stdout_file, stderr=stderr_file)
        start = time.monotonic(); device.deadline = start + plan['timeout_seconds']
        receipt['state'] = 'RUNNING'; save()
        while True:
            # Completion first observed after the deadline is incomplete even
            # if the remote process exited while a sampling command was busy.
            if time.monotonic() >= device.deadline:
                stop_reason = 'TIMEOUT'
                break
            if child.poll() is not None:
                break
            try:
                pid = device.pid()
                if pid:
                    if target_pid and target_pid != pid:
                        stop_reason = 'OWN_PROCESS_CHANGED'; break
                    target_pid = pid
                    identity = device.process_identity(pid, uid)
                    if target_identity and identity != target_identity:
                        stop_reason = 'OWN_PROCESS_CHANGED'; break
                    if not target_identity:
                        target_identity = identity
                        receipt['own_process_identity_sha256'] = target_identity
                    sample = device.freeze_sample(pid, uid)
                    sample['elapsed_seconds'] = round(time.monotonic() - start, 3)
                    receipt['cgroup_samples'].append(sample)
                    if sample['mode'] == '1' or sample['frozen'] == '1':
                        stop_reason = 'OWN_CGROUP_FROZEN'; break
            except Blocked as error:
                receipt['freezer_observation_error'] = safe_reason(error)
            if child.poll() is None:
                time.sleep(.3)
        device.deadline = None
        if stop_reason:
            receipt['state'] = 'ENVIRONMENT_FROZEN_INCOMPLETE' if stop_reason == 'OWN_CGROUP_FROZEN' else 'TIMEOUT_OR_PROCESS_CHANGE_INCOMPLETE'
            receipt['stop_reason'] = stop_reason
            # A process/package check is not a cross-session lock. Never force-stop.
            receipt['own_stop'] = 'NOT_REQUESTED_SHARED_DEVICE_NO_LOCK'
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired: receipt['local_transport'] = 'TERMINATION_UNCONFIRMED'
                receipt['remote_process_state'] = 'UNKNOWN_LOCAL_TRANSPORT_ONLY'
        if child.poll() is None:
            raise Blocked('instrument_transport_still_running')
        stdout_file.close(); stderr_file.close(); stdout_file = stderr_file = None
        stdout = (private / 'instrumentation.stdout').read_bytes()
        stderr = (private / 'instrumentation.stderr').read_bytes()
        receipt['transport'] = {'exit_code': child.returncode, 'stdout_sha256': sha(stdout), 'stderr_sha256': sha(stderr),
                                'elapsed_seconds': round(time.monotonic() - start, 3)}
        if receipt['cgroup_samples']:
            receipt['freezer_observation'] = 'FROZEN_OBSERVED' if any(s['mode'] == '1' or s['frozen'] == '1' for s in receipt['cgroup_samples']) else 'DISCRETE_SAMPLES_UNFROZEN_NOT_CONTINUOUS'
        if not stop_reason:
            require(not stderr.strip(), 'instrument_stderr_nonempty')
            receipt['android_report'] = parse_report(stdout, child.returncode, plan.get('probe_scope', 'pressure-v1'))
            receipt['report_validated'] = True
            receipt['normal_domain_closed_proven'] = receipt['android_report']['status'] == 'PASS'
            receipt['state'] = 'REPORT_' + receipt['android_report']['status'] + '_POSTFLIGHT_PENDING'
        device.gate('after')
        _, boot_after, _ = device.command('boot-after', ['shell', 'cat', '/proc/sys/kernel/random/boot_id'])
        require(UUID.fullmatch(boot_after.strip()) and sha(boot_after.strip().encode()) == receipt['boot_id_sha256'], 'device_boot_changed')
        _, current_after, _ = device.command('current-user-postflight', ['shell', 'am', 'get-current-user'])
        require(current_after.strip() == user_id, 'device_user_changed')
        _, post_path, _ = device.command('package-path-postflight', ['shell', 'pm', 'path', PACKAGE])
        post_values = parse_package_paths(post_path)
        require(post_values == values, 'installed_package_changed')
        final = private / 'installed-postflight.apk'
        device.command('pull-postflight-own-apk', ['pull', post_values[0], final], timeout=60)
        require(digest(final) == plan['apk_sha256'], 'postflight_apk_identity_mismatch')
        receipt['postflight_installed_apk_sha256'] = digest(final)
        receipt['remote_process_state'] = 'NO_MATCHING_TESTS_OBSERVED_NO_SHARED_LOCK'
        if not stop_reason and receipt['android_report']['status'] == 'FAIL':
            receipt['state'] = 'FAIL_FUNCTIONAL_REPORT_POSTFLIGHT_COMPLETE'
        elif not stop_reason:
            receipt['state'] = 'PASS_FUNCTIONAL_POSTFLIGHT_FREEZER_OBSERVED' if receipt['cgroup_samples'] else 'PASS_FUNCTIONAL_POSTFLIGHT_FREEZER_NOT_OBSERVED'
    except BaseException as error:
        # Preserve any already-validated report, but do not promote an incomplete
        # postflight to a completed run. Never expose exception text with paths.
        receipt['state'] = 'INCOMPLETE_OR_BLOCKED'
        receipt['reason'] = safe_reason(error)
        if receipt.get('installation') in ('REQUEST_IN_FLIGHT_OUTCOME_UNKNOWN', 'COMMAND_SUCCESS_INSTALLED_BYTES_NOT_YET_VERIFIED'):
            receipt['installation_outcome'] = 'UNKNOWN_REQUIRES_READ_ONLY_RECHECK'
        if child is not None and child.poll() is None:
            # The safe stop requires independent PID/instrumentation identity.
            # On an unexpected failure leave device state unknown; stop only
            # the local ADB transport rather than risking another session.
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: receipt['local_transport'] = 'TERMINATION_UNCONFIRMED'
            receipt['remote_process_state'] = 'UNKNOWN_NO_FORCE_STOP_ON_UNEXPECTED_ERROR'
    finally:
        if stdout_file and not stdout_file.closed: stdout_file.close()
        if stderr_file and not stderr_file.closed: stderr_file.close()
        receipt['finished_at_utc'] = utc()
        try:
            save()
        except Exception as error:
            marker = {'state': 'INCOMPLETE_RECEIPT_PUBLICATION', 'reason': safe_reason(error),
                      'plan_sha256': receipt['plan_sha256'], 'device_serial_sha256': receipt['device_serial_sha256'],
                      'remote_process_state': receipt.get('remote_process_state', 'UNKNOWN'),
                      'last_complete_sha256': digest(out / 'receipt.json') if (out / 'receipt.json').is_file() else None,
                      'pending_sha256': digest(out / 'receipt.pending') if (out / 'receipt.pending').is_file() else None}
            try: atomic_receipt(out / 'receipt.failure.json', marker)
            except Exception: pass
            print(json.dumps({'state': marker['state'], 'reason': marker['reason']}))
            return 2
    print(json.dumps({'state': receipt['state'], 'freezer_observation': receipt['freezer_observation'],
                      'cgroup_samples': len(receipt['cgroup_samples']), 'reason': receipt.get('reason')}))
    return 0 if receipt['state'].startswith('PASS_FUNCTIONAL_POSTFLIGHT_') else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    def common(p):
        for name in ('apk', 'aar', 'sdk-core', 'build-receipt', 'adb', 'aapt2', 'java', 'apksigner'):
            p.add_argument('--' + name, type=Path, required=True)
        p.add_argument('--serial', required=True)
        p.add_argument('--probe-scope', choices=PROBE_SCOPES, default='pressure-v1')
    freezer = sub.add_parser('freeze', help='Freeze exact local inputs without ADB')
    common(freezer)
    freezer.add_argument('--api', type=int, required=True)
    freezer.add_argument('--page-bytes', type=int, required=True)
    freezer.add_argument('--timeout-seconds', type=int, default=90)
    freezer.add_argument('--output', type=Path, help='Optional fresh build plan path; defaults to a fresh ignored build directory')
    runner = sub.add_parser('run', help='Run only the frozen own-package probe')
    common(runner)
    runner.add_argument('--plan', type=Path, required=True)
    runner.add_argument('--plan-sha256', required=True, help='SHA-256 previously emitted by freeze; not recomputed at dispatch')
    runner.add_argument('--out-dir', type=Path, required=True)
    runner.add_argument('--private-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.action == 'freeze':
            freeze(args)
            return 0
        return run(args)
    except (Blocked, OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        print(json.dumps({'state': 'INCOMPLETE_OR_BLOCKED', 'reason': safe_reason(error)}))
        return 2


if __name__ == '__main__':
    sys.exit(main())
