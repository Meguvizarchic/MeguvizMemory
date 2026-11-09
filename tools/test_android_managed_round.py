#!/usr/bin/env python3
"""Offline fake-device tests. Never runs ADB or modifies a physical device."""
import hashlib
import json
from pathlib import Path
import secrets
import zipfile
from types import SimpleNamespace
from unittest import mock
import android_managed_round as runner

PASS = {'suite':'android-managed-pressure-v1', 'status':'PASS', 'checks':302, 'case_count':3,
        'final_backing_bytes':0, 'callback_origin':'synthetic', 'queued_implies_applied':False,
        'performance_qualified':False}
FAIL = {'suite':'android-managed-pressure-v1','status':'FAIL','checks':10,'stage':'system_close'}
ACTIVITY = ('ACTIVITY MANAGER ACTIVITIES (dumpsys activity activities)\n'
            'ACTIVITY MANAGER RUNNING PROCESSES (dumpsys activity processes)\n')
PROCESSES = ('PID NAME ARGS\n1 init /init\n2 system_server system_server\n'
             '3 zygote zygote\n4 logd logd\n5 servicemanager servicemanager\n')
def output(value=PASS, code=0):
    return ('INSTRUMENTATION_RESULT: report_json=' + json.dumps(value,separators=(',',':')) +
            '\nINSTRUMENTATION_CODE: ' + str(code) + '\n')

def reject(value, code=0):
    try: runner.parse_report(value, code)
    except (runner.Blocked, UnicodeDecodeError): return
    raise AssertionError('bad report accepted')

def parsers():
    assert runner.parse_report(output(), 0) == PASS
    assert runner.parse_report(output(FAIL, 1), 0) == FAIL
    for bad in (output() + output(), output()[:-1], output(PASS, 1), output(FAIL, 0),
                output().replace('"checks":302','"checks":301'),
                output().replace('"checks":302','"checks":1000001'),
                output().replace('"checks":302','"checks":true'),
                output().replace('"checks":302','"checks":302,"checks":302'),
                output().replace('"checks":302','"checks":NaN'),
                output().replace('"case_count":3','"case_count":true'),
                output().replace('"callback_origin":"synthetic"','"callback_origin":"system"'),
                output().replace('"performance_qualified":false','"performance_qualified":0'),
                output().replace('"final_backing_bytes":0','"final_backing_bytes":1'),
                output().replace('INSTRUMENTATION_CODE: 0','INSTRUMENTATION_FAILED: crashed\nINSTRUMENTATION_CODE: 0'),
                output().replace('android-managed-pressure-v1','android-native-borrow-v2')):
        reject(bad)
    reject(output(), 1); reject(b'\xff')
    assert runner.parse_gate(PROCESSES, ACTIVITY)['result'] == 'PASS_OBSERVATIONAL_ONLY'
    assert runner.parse_gate(PROCESSES, ACTIVITY + 'mActiveInstrumentation=[]\n')['result'] == 'PASS_OBSERVATIONAL_ONLY'
    for ps, act in ((PROCESSES+'6 DexDash test\n', ACTIVITY),
                    (PROCESSES, ACTIVITY+'ActiveInstrumentation{other}\n'),
                    (PROCESSES, ACTIVITY+'unknownActiveInstrumentationFormat\n')):
        assert runner.parse_gate(ps, act)['result'] == 'BLOCKED'
    for ps, act in (('',ACTIVITY),(PROCESSES,''),(PROCESSES,ACTIVITY+'Permission Denial\n'),
                    (PROCESSES+'unexpected malformed row\n',ACTIVITY), (PROCESSES+'1 duplicate init\n',ACTIVITY)):
        try: runner.parse_gate(ps, act)
        except runner.Blocked: pass
        else: raise AssertionError('bad inventory accepted')
    for bad in ('package:/outside/base.apk\n','package:/data/app/a.apk\npackage:/data/app/b.apk\n'):
        try: runner.parse_package_paths(bad)
        except runner.Blocked: pass
        else: raise AssertionError('ambiguous path accepted')

def user_package_states():
    installed = {'0': True, '999': False}
    dump = ('Packages:\n  Package [' + runner.PACKAGE + '] (fixture):\n'
            '    User 0: ceDataInode=32 installed=true stopped=true notLaunched=false\n'
            '    User 999: ceDataInode=0 installed=false stopped=true notLaunched=true\n'
            'Queries:\n    User 0:\n    User 999:\n')
    result = runner.parse_package_user_states(dump, ['0', '999'], installed)
    assert result == {'0': 'INSTALLED', '999': 'UNINSTALLED_EMPTY'}
    raw_crlf = dump.replace('\n', '\r\n').encode('utf-8')
    assert runner.parse_package_user_states(raw_crlf.decode('utf-8'), ['0', '999'], installed) == result
    for change in (lambda x:x.replace('ceDataInode=0', 'ceDataInode=42'),
                   lambda x:x.replace('installed=false stopped=true', 'installed=false stopped=false'),
                   lambda x:x.replace('notLaunched=true', 'notLaunched=false')):
        assert runner.parse_package_user_states(change(dump), ['0', '999'], installed)['999'] == 'RETAINED_OR_UNKNOWN'
    for bad in (dump.replace('installed=false', 'installed=false installed=true'),
                dump.replace('ceDataInode=0 ', ''), dump.replace('notLaunched=true', 'notLaunched=unknown'),
                dump.replace('installed=false', 'installed=true'), dump.replace('User 999: ceDataInode=0', 'User 0: ceDataInode=0'),
                dump.replace('Package [' + runner.PACKAGE, 'Package [other.package')):
        for raw in (bad.encode('utf-8'), bad.replace('\n', '\r\n').encode('utf-8')):
            try: runner.parse_package_user_states(raw.decode('utf-8'), ['0', '999'], installed)
            except runner.Blocked: pass
            else: raise AssertionError('uncertain per-user package state accepted')


def feature_scope():
    report = dict(PASS, suite=runner.COPY_REPORT_SUITE, checks=1200, case_count=5,
                  copy_to_cases=2, arena_same_shape_allocations=96)
    assert runner.parse_report(output(report), 0, 'copy-arena-v1') == report
    reject(output(report))
    for changed in (dict(report, copy_to_cases=True), dict(report, copy_to_cases=1),
                    dict(report, arena_same_shape_allocations=95), dict(report, case_count=3),
                    dict(report, checks=302), PASS):
        try: runner.parse_report(output(changed), 0, 'copy-arena-v1')
        except runner.Blocked: pass
        else: raise AssertionError('new scope accepted incomplete coverage')
    built = {'probe_scopes': ['pressure-v1', 'copy-arena-v1'], 'r8_evidence': {
        'copy_to_native_signature': '(JJJJI)I', 'copy_to_calls_retained': True}}
    runner.verify_feature_scope(built, 'copy-arena-v1')
    try: runner.verify_feature_scope({}, 'copy-arena-v1')
    except runner.Blocked: pass
    else: raise AssertionError('new scope accepted old build')
    command = runner.instrument_command('adb', 'fake-device', '0', 'copy-arena-v1')
    assert command[-4:] == ['-e', 'probe_scope', 'copy-arena-v1', runner.COMPONENT]
    assert '-e' not in runner.instrument_command('adb', 'fake-device', '0', 'pressure-v1')
    try: runner.validate_plan({'probe_scope': 'copy-arena-v1'}, SimpleNamespace(probe_scope='pressure-v1'))
    except runner.Blocked as error: assert str(error) == 'plan_probe_scope_mismatch'
    else: raise AssertionError('plan scope replacement accepted')


def atomic(work):
    target = work / 'atomic.json'; target.write_bytes(b'old')
    original = runner.os.replace; calls = []
    def transient(source, destination):
        calls.append(1)
        assert target.read_bytes() == b'old'
        if len(calls) < 3: raise PermissionError('private-machine-path')
        original(source, destination)
    with mock.patch.object(runner.os, 'replace', side_effect=transient):
        runner.atomic_receipt(target, {'state': 'PASS'})
    assert len(calls) == 3 and json.loads(target.read_text()) == {'state': 'PASS'}
    prior = target.read_bytes(); pending = target.with_suffix('.pending')
    pending.write_bytes(b'previous pending')
    try:
        runner.atomic_receipt(target, {'state': 'OTHER'})
    except runner.Blocked as error:
        assert str(error) == 'receipt_pending_already_exists'
    else:
        raise AssertionError('existing pending was overwritten')
    assert target.read_bytes() == prior and pending.read_bytes() == b'previous pending'
    denied = work / 'denied.json'; denied.write_bytes(b'old-denied')
    ticks = iter([0, .5, 1])
    with mock.patch.object(runner.os, 'replace', side_effect=PermissionError('private-machine-path')), \
         mock.patch.object(runner.time, 'monotonic', side_effect=lambda: next(ticks)), \
         mock.patch.object(runner.time, 'sleep', return_value=None):
        try: runner.atomic_receipt(denied, {'state': 'NEW'})
        except runner.Blocked as error: assert str(error) == 'receipt_replace_permission_denied'
        else: raise AssertionError('persistent denial was accepted')
    assert denied.read_bytes() == b'old-denied' and denied.with_suffix('.pending').is_file()


class FakeProcess:
    def __init__(self, stdout, stderr, frozen, fail=False):
        self.stdout, self.stderr, self.frozen = stdout, stderr, frozen
        self.code = None if frozen else 0
        stdout.write(output(FAIL, 1).encode() if fail else output().encode()); stdout.flush()
    @property
    def returncode(self): return self.code
    def poll(self): return self.code
    def wait(self, timeout):
        self.code = 0
        return self.code
    def terminate(self): self.code = -1


class FakeDevice:
    calls = []
    sample_frozen = False
    expected_apk = b''
    other_busy = False
    installation_timeout = False
    other_user_package = False
    other_user_state = 'installed'
    postflight_bad = False
    gate_block = False
    installed_before = None
    signature_mismatch = False
    def __init__(self, args, out, private, receipt):
        self.receipt = receipt; self.installed = False; self.deadline = None
    def gate(self, label):
        if self.gate_block and label == 'before-install': raise runner.Blocked('before-install_other_test')
        self.receipt['gates'].append({'name': label, 'result': 'PASS_OBSERVATIONAL_ONLY'})
    def pid(self): return '1234' if self.sample_frozen else None
    def process_identity(self, pid, uid): return 'f' * 64
    def freeze_sample(self, pid, uid):
        return {'mode': '1' if self.sample_frozen else '0', 'frozen': '1' if self.sample_frozen else '0',
                'pid_sha256': 'a' * 64, 'cgroup_path_sha256': 'b' * 64, 'observed_at_utc': 'fixture'}
    def command(self, label, parts, **_):
        self.calls.append((label, [str(x) for x in parts]))
        if label == 'install-own-diagnostic' and self.installation_timeout:
            raise runner.Blocked('install-own-diagnostic_timeout')
        if label == 'user-inventory' and self.other_user_package:
            return 0, 'Users:\n UserInfo{0:Owner:13} running\n UserInfo{10:Other:30}\n', ''
        if label == 'package-inventory-user' and self.other_user_package and '10' in parts:
            return 0, 'package:com.android.settings\npackage:' + runner.PACKAGE + '\n', ''
        if label == 'installed-package-inventory-user':
            present = self.installed_before is not None if '0' in parts else self.other_user_state == 'installed'
            return 0, 'package:com.android.settings\n' + ('package:' + runner.PACKAGE + '\n' if present else ''), ''
        if label == 'package-user-state':
            current = 'true' if self.installed_before is not None else 'false'
            data = '32' if self.installed_before is not None else '0'
            launched = 'false' if self.installed_before is not None else 'true'
            text = 'Packages:\n  Package [' + runner.PACKAGE + '] (fixture):\n'
            text += '    User 0: ceDataInode=' + data + ' installed=' + current + ' stopped=true notLaunched=' + launched + '\n'
            if self.other_user_package:
                state = self.other_user_state
                text += '    User 10: ceDataInode=' + ('32' if state in ('installed', 'retained') else '0')
                text += ' installed=' + ('true' if state == 'installed' else 'false') + ' stopped=true'
                if state != 'unknown': text += ' notLaunched=' + ('false' if state in ('installed', 'retained') else 'true')
                text += '\n'
            return 0, text + 'Queries:\n    User 0:\n', ''
        values = {
            'api': '33\n', 'abi': 'arm64-v8a\n', 'page': '4096\n',
            'boot-before': '12345678-1234-1234-1234-123456789abc\n',
            'boot-after': '12345678-1234-1234-1234-123456789abc\n', 'current-user': '0\n', 'current-user-postflight': '0\n',
            'user-inventory': 'Users:\n UserInfo{0:Owner:13} running\n',
            'package-inventory-user': 'package:com.android.settings\n', 'own-uid': '10123\n',
            'install-own-diagnostic': 'Success\n',
            'package-path-after-install': 'package:/data/app/own/base.apk\n',
            'package-path-postflight': 'package:/data/app/own/base.apk\n',
            'cleanup-processes': PROCESSES + '1234 ' + runner.PACKAGE + ' app\n' + ('555 DexDash other\n' if self.other_busy else ''),
            'cleanup-activity': ACTIVITY + 'ActiveInstrumentation{' + runner.PACKAGE + '}\n',
            'stop-only-own-diagnostic': '',
        }
        if label == 'package-inventory-user' and self.installed_before is not None and ('0' in parts):
            return 0, 'package:com.android.settings\npackage:' + runner.PACKAGE + '\n', ''
        if label == 'package-path-before':
            return (0, 'package:/data/app/own/base.apk\n', '') if self.installed_before is not None else (1, '', '')
        if label == 'pull-existing-own-apk':
            Path(parts[-1]).write_bytes(self.installed_before)
            return 0, '', ''
        if label in ('pull-installed-own-apk', 'pull-postflight-own-apk'):
            Path(parts[-1]).write_bytes(b'changed' if self.postflight_bad and label == 'pull-postflight-own-apk' else self.expected_apk)
            return 0, '', ''
        assert label in values, label
        return 0, values[label], ''


def synthetic_run(work, frozen=False, timeout=False, fail=False, late_exit=False, other_busy=False, install_timeout=False, other_user=False, postflight_bad=False, gate_block=False, existing=None, bad_signer=False, other_state='installed'):
    suffix = str(frozen) + str(timeout) + str(fail) + str(late_exit) + str(other_busy) + str(install_timeout) + str(other_user) + str(postflight_bad) + str(gate_block) + str(existing) + str(bad_signer) + other_state
    serial = 'synthetic-device'; apk = work / ('apk-' + suffix); apk.write_bytes(b'synthetic-apk')
    plan_path = work / ('plan-' + suffix + '.json')
    plan = {'scope': 'SYNTHETIC_NO_DEVICE', 'apk_sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
            'aar_sha256': 'a' * 64, 'core_sha256': 'b' * 64, 'jni_sha256': 'c' * 64,
            'certificate_sha256': 'd' * 64, 'device': {'api': 33, 'abi': 'arm64-v8a', 'page_bytes': 4096},
            'timeout_seconds': 0 if timeout else 15}
    plan_path.write_text(json.dumps(plan))
    args = SimpleNamespace(serial=serial, plan=plan_path, apk=apk, aar=apk, sdk_core=apk, build_receipt=apk,
                           plan_sha256=hashlib.sha256(plan_path.read_bytes()).hexdigest(),
                           adb=work / 'NO_ADB', out_dir=work / ('receipts-' + suffix),
                           private_dir=work / ('private-' + suffix))
    FakeDevice.calls = []; FakeDevice.sample_frozen = frozen; FakeDevice.expected_apk = apk.read_bytes()
    FakeDevice.other_busy = other_busy; FakeDevice.installation_timeout = install_timeout; FakeDevice.other_user_package = other_user
    FakeDevice.other_user_state = other_state
    FakeDevice.postflight_bad = postflight_bad; FakeDevice.gate_block = gate_block
    FakeDevice.installed_before = (apk.read_bytes() if existing == 'same' else b'older-apk' if existing else None)
    clock = {'launched': False, 'start_seen': False}
    def fake_popen(command, stdout, stderr):
        assert str(args.adb) == str(command[0])
        clock['launched'] = True
        return FakeProcess(stdout, stderr, frozen or timeout, fail)
    with mock.patch.object(runner, 'BASE', work), mock.patch.object(runner, 'Device', FakeDevice), \
         mock.patch.object(runner, 'validate_plan', return_value=({'apk': apk}, {'java':'fake-java','apksigner':'fake-signer'})), \
         mock.patch.object(runner.subprocess, 'run', return_value=SimpleNamespace(returncode=0,
             stdout=('certificate SHA-256 digest: '+ ('e' if bad_signer else 'd')*64).encode(), stderr=b'')), \
         mock.patch.object(runner.subprocess, 'Popen', side_effect=fake_popen):
        expected_hash = args.plan_sha256
        args.plan_sha256 = '0' * 64
        try: runner.run(args)
        except runner.Blocked as error: assert str(error) == 'frozen_plan_sha256_mismatch'
        else: raise AssertionError('modified plan hash was accepted')
        assert not FakeDevice.calls and not args.out_dir.exists() and not args.private_dir.exists()
        args.plan_sha256 = expected_hash
        if late_exit:
            def now():
                if clock['launched'] and not clock['start_seen']:
                    clock['start_seen'] = True
                    return 100.0
                return 116.0 if clock['launched'] else 100.0
            with mock.patch.object(runner.time, 'monotonic', side_effect=now):
                result = runner.run(args)
        else:
            result = runner.run(args)
    value = json.loads((args.out_dir / 'receipt.json').read_text(encoding='utf-8'))
    assert serial not in json.dumps(value)
    assert all('uninstall' not in ' '.join(parts) and 'clear' not in ' '.join(parts) for _, parts in FakeDevice.calls)
    if other_user and other_state == 'unknown':
        assert result == 2 and value['reason'] == 'package_user_state_unparseable'
        assert not [x for x in FakeDevice.calls if x[0] == 'install-own-diagnostic']
    elif bad_signer:
        assert result == 2 and value['reason'] == 'existing_package_signer_mismatch'
        assert not [x for x in FakeDevice.calls if x[0] == 'install-own-diagnostic']
    elif postflight_bad:
        assert result == 2 and value['state'] == 'INCOMPLETE_OR_BLOCKED'
        assert value['reason'] == 'postflight_apk_identity_mismatch' and value['report_validated']
    elif gate_block:
        assert result == 2 and value['reason'] == 'before-install_other_test'
        assert not [x for x in FakeDevice.calls if x[0] == 'install-own-diagnostic']
    elif other_user and other_state != 'empty' and existing != 'same':
        assert result == 2 and value['state'] == 'INCOMPLETE_OR_BLOCKED'
        assert value['reason'] == 'same_package_present_for_other_user' and value['other_user_package_present']
        assert not [x for x in FakeDevice.calls if x[0] == 'install-own-diagnostic']
    elif install_timeout:
        assert result == 2 and value['state'] == 'INCOMPLETE_OR_BLOCKED'
        assert value['installation_outcome'] == 'UNKNOWN_REQUIRES_READ_ONLY_RECHECK'
        assert value['remote_process_state'] == 'UNKNOWN_POSTFLIGHT_NOT_COMPLETED'
        assert not [x for x in FakeDevice.calls if x[0] == 'stop-only-own-diagnostic']
    elif frozen:
        assert result == 2 and value['state'] == 'ENVIRONMENT_FROZEN_INCOMPLETE'
        assert value['freezer_observation'] == 'FROZEN_OBSERVED' and not value['normal_domain_closed_proven']
        stops = [x for x in FakeDevice.calls if x[0] == 'stop-only-own-diagnostic']
        assert not stops and value['own_stop'] == 'NOT_REQUESTED_SHARED_DEVICE_NO_LOCK'
    elif timeout or late_exit:
        assert result == 2 and value['state'] == 'TIMEOUT_OR_PROCESS_CHANGE_INCOMPLETE'
        assert value['stop_reason'] == 'TIMEOUT' and not value['normal_domain_closed_proven']
        assert not [x for x in FakeDevice.calls if x[0] == 'stop-only-own-diagnostic']
    elif fail:
        assert result == 2 and value['state'] == 'FAIL_FUNCTIONAL_REPORT_POSTFLIGHT_COMPLETE'
        assert not value['normal_domain_closed_proven'] and value['android_report']['status'] == 'FAIL'
    else:
        assert result == 0 and value['state'] == 'PASS_FUNCTIONAL_POSTFLIGHT_FREEZER_NOT_OBSERVED'
        assert value['normal_domain_closed_proven'] and value['freezer_observation'] == 'NOT_OBSERVED'
        if existing == 'same':
            assert value['installation'] == 'EXACT_APK_ALREADY_INSTALLED_NO_UPDATE'
            assert not [x for x in FakeDevice.calls if x[0] == 'install-own-diagnostic']
        if existing == 'different':
            assert any('-r' in x[1] for x in FakeDevice.calls if x[0] == 'install-own-diagnostic')


def local_artifacts(work):
    """Exercise the actual APK/AAR checks using synthetic zip bytes and fake local tools."""
    folder = work / 'artifacts'; folder.mkdir()
    elf = bytearray(24); elf[:6] = b'\x7fELF\x02\x01'; elf[18:20] = (183).to_bytes(2, 'little')
    core = bytes(elf) + b'core'; jni = bytes(elf) + b'jni'; dex = b'dex'; classes = b'classes'
    apk = folder / 'probe.apk'; aar = folder / 'library.aar'; sdk = folder / 'core.so'; sdk.write_bytes(core)
    members = {'AndroidManifest.xml':b'manifest','resources.arsc':b'resources','classes.dex':dex,
               'lib/arm64-v8a/libmgmem.so':core,'lib/arm64-v8a/libmgmem_pressure_jni.so':jni,
               'META-INF/MANIFEST.MF':b'x','META-INF/CERT.SF':b'x','META-INF/CERT.RSA':b'x'}
    with zipfile.ZipFile(apk,'w') as z:
        for name, value in members.items(): z.writestr(name,value)
    with zipfile.ZipFile(aar,'w') as z:
        z.writestr('classes.jar',classes); z.writestr('jni/arm64-v8a/libmgmem.so',core)
        z.writestr('jni/arm64-v8a/libmgmem_pressure_jni.so',jni)
    cert='a'*64
    receipt={'state':'PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION','device_executed':False,
             'package_name':runner.PACKAGE,'instrumentation':runner.INSTRUMENT,'abi':'arm64-v8a',
             'apk_sha256':runner.digest(apk),'apk_bytes':apk.stat().st_size,'certificate_sha256':cert,
             'inputs':{'aar_sha256':runner.digest(aar),'shared_core_sha256':runner.sha(core),
                       'shared_jni_sha256':runner.sha(jni),'classes_jar_sha256':runner.sha(classes)},
             'r8_evidence':{'dex_sha256':runner.sha(dex),'instrumentation_retained':True,
                            'managed_native_signatures_retained':True,'shrink_witness_removed':True}}
    build=folder/'receipt.json'; build.write_text(json.dumps(receipt))
    tree=('  E: manifest (line=1)\n    E: uses-sdk (line=1)\n'
          '      A: http://schemas.android.com/apk/res/android:minSdkVersion(0x0101020c)=27\n'
          '      A: http://schemas.android.com/apk/res/android:targetSdkVersion(0x01010270)=33\n'
          '    E: application (line=2)\n'
          '      A: http://schemas.android.com/apk/res/android:label(0x01010001)="probe"\n'
          '      A: http://schemas.android.com/apk/res/android:debuggable(0x0101000f)=true\n'
          '      A: http://schemas.android.com/apk/res/android:allowBackup(0x01010280)=false\n'
          '    E: instrumentation (line=3)\n'
          '      A: http://schemas.android.com/apk/res/android:name(0x01010003)="'+runner.INSTRUMENT+'"\n'
          '      A: http://schemas.android.com/apk/res/android:targetPackage(0x01010021)="'+runner.PACKAGE+'"\n'
          '      A: http://schemas.android.com/apk/res/android:functionalTest(0x01010023)=true\n')
    def fake_run(command, **_):
        executable = Path(command[0]).name
        assert executable in ('fake-java','fake-aapt'), command
        out = ('Signer #1 certificate SHA-256 digest: '+cert+'\n' if executable=='fake-java' else
               "package: name='"+runner.PACKAGE+"'\nminSdkVersion:'27'\ntargetSdkVersion:'33'\nnative-code: 'arm64-v8a'\n" if 'badging' in command else tree)
        return SimpleNamespace(returncode=0,stdout=out.encode(),stderr=b'')
    def verify(): runner.verify_local(apk,aar,sdk,build,'arm64-v8a',cert,'fake-aapt','fake-java','fake-signer')
    with mock.patch.object(runner.subprocess,'run',side_effect=fake_run):
        verify()
        source_paths = runner.source_files()
        receipt['source_sha256']={name:runner.digest(path) for name,path in source_paths.items()}
        build.write_text(json.dumps(receipt))
        for name in ('fake-adb','fake-aapt','fake-java','fake-signer'): (folder/name).write_bytes(name.encode())
        args=SimpleNamespace(serial='fake-device',api=33,page_bytes=4096,timeout_seconds=90,
            output=folder/'plan.json',apk=apk,aar=aar,sdk_core=sdk,build_receipt=build,
            adb=folder/'fake-adb',aapt2=folder/'fake-aapt',java=folder/'fake-java',apksigner=folder/'fake-signer')
        with mock.patch.object(runner, 'BASE', folder): runner.freeze(args)
        plan=runner.unique_json(args.output.read_text())
        legacy_plan = args.output
        args.output = None
        with mock.patch.object(runner, 'BASE', folder/'legacy'):
            private_plan = runner.freeze(args)
        assert private_plan.is_relative_to((runner.ROOT/'build').resolve()) and private_plan.is_file()
        private_bytes = private_plan.read_bytes()
        runner.validate_plan(runner.unique_json(private_bytes.decode()),args)
        args.output = private_plan
        with mock.patch.object(runner, 'BASE', folder/'legacy'):
            try: runner.freeze(args)
            except runner.Blocked as error: assert str(error) == 'fresh_scoped_plan_required'
            else: raise AssertionError('existing private plan overwritten')
        assert private_plan.read_bytes() == private_bytes
        args.output = folder/'fresh-private'/'plan.json'
        with mock.patch.object(runner, 'BASE', folder/'legacy'):
            explicit = runner.freeze(args)
        assert explicit == args.output.resolve() and explicit.is_file()
        args.output = folder/'used-private'/'plan.json'; args.output.parent.mkdir()
        with mock.patch.object(runner, 'BASE', folder/'legacy'):
            try: runner.freeze(args)
            except runner.Blocked as error: assert str(error) == 'fresh_private_plan_directory_required'
            else: raise AssertionError('existing private directory accepted')
        args.output = legacy_plan
        assert plan['device_serial_sha256']==runner.sha(args.serial.encode()) and 'fake-device' not in args.output.read_text()
        runner.validate_plan(plan,args)
        for name in ('build_android_candidate.py', 'package_feature_preview.py', 'source_lock.py', 'android_managed_round.py'):
            target = source_paths[name].resolve().relative_to(runner.ROOT).as_posix()
            assert target in {entry['path'] for entry in plan['files']}
            altered = dict(plan); altered['files'] = [entry for entry in plan['files'] if entry['path'] != target]
            try: runner.validate_plan(altered,args)
            except runner.Blocked as error: assert str(error) == 'plan_missing_required_source'
            else: raise AssertionError('missing frozen source accepted')
            bad_receipt = dict(receipt); bad_receipt['source_sha256'] = dict(receipt['source_sha256'])
            bad_receipt['source_sha256'][name] = '0'*64
            try: runner.verify_build_sources(bad_receipt)
            except runner.Blocked as error: assert str(error) == 'build_source_drift'
            else: raise AssertionError('source identity drift accepted')
        sdk.write_bytes(core+b'tampered')
        try: runner.validate_plan(plan,args)
        except runner.Blocked: pass
        else: raise AssertionError('frozen input drift accepted')
        sdk.write_bytes(core)
        sdk.write_bytes(core+b'tampered')
        try: verify()
        except runner.Blocked: pass
        else: raise AssertionError('different core accepted')
        sdk.write_bytes(core)
        receipt['r8_evidence']['shrink_witness_removed']=False; build.write_text(json.dumps(receipt))
        try: verify()
        except runner.Blocked: pass
        else: raise AssertionError('missing shrink evidence accepted')
        receipt['r8_evidence']['shrink_witness_removed']=True; build.write_text(json.dumps(receipt))
        try: runner.verify_local(apk,aar,sdk,build,'arm64-v8a','b'*64,'fake-aapt','fake-java','fake-signer')
        except runner.Blocked: pass
        else: raise AssertionError('wrong certificate accepted')
        min_line = '      A: http://schemas.android.com/apk/res/android:minSdkVersion(0x0101020c)=27\n'
        target_line = '      A: http://schemas.android.com/apk/res/android:targetSdkVersion(0x01010270)=33\n'
        for bad in (tree.replace(min_line, ''), tree.replace(target_line, ''),
                    tree.replace(min_line, min_line + min_line), tree.replace(target_line, target_line + target_line),
                    tree.replace(min_line, min_line.replace('=27', '=26')),
                    tree.replace(target_line, target_line.replace('=33', '=34'))):
            try: runner.verify_manifest_tree(bad)
            except runner.Blocked: pass
            else: raise AssertionError('incorrect SDK manifest accepted')
        original_fake = fake_run
        def badging_fake(wrong):
            def apply(command, **kwargs):
                result = original_fake(command, **kwargs)
                if 'badging' in command: result.stdout = wrong(result.stdout)
                return result
            return apply
        for modify in (lambda x:x.replace(b"minSdkVersion:'27'", b"minSdkVersion:'26'"),
                       lambda x:x.replace(b"targetSdkVersion:'33'", b"targetSdkVersion:'34'"),
                       lambda x:x.replace(b"minSdkVersion:'27'\n", b''),
                       lambda x:x.replace(b"targetSdkVersion:'33'\n", b''),
                       lambda x:x.replace(b"minSdkVersion:'27'\n", b"minSdkVersion:'27'\nminSdkVersion:'27'\n"),
                       lambda x:x.replace(b"targetSdkVersion:'33'\n", b"targetSdkVersion:'33'\ntargetSdkVersion:'33'\n")):
            with mock.patch.object(runner.subprocess,'run',side_effect=badging_fake(modify)):
                try: verify()
                except runner.Blocked as error: assert str(error) == 'local_apk_badging_sdk_mismatch'
                else: raise AssertionError('incorrect SDK badging accepted')
        try: runner.verify_manifest_tree(tree.replace(runner.PACKAGE,'other.package'))
        except runner.Blocked: pass
        else: raise AssertionError('wrong target accepted')


def main():
    work = runner.ROOT / 'build' / ('managed-runner-offline-' + secrets.token_hex(8))
    work.mkdir(parents=True, exist_ok=False)
    parsers(); user_package_states(); feature_scope(); atomic(work); local_artifacts(work); synthetic_run(work); synthetic_run(work, frozen=True); synthetic_run(work, timeout=True)
    synthetic_run(work, fail=True); synthetic_run(work, late_exit=True)
    synthetic_run(work, frozen=True, other_busy=True); synthetic_run(work, install_timeout=True)
    synthetic_run(work, other_user=True); synthetic_run(work, postflight_bad=True); synthetic_run(work, gate_block=True)
    synthetic_run(work, existing='same', other_user=True); synthetic_run(work, existing='different')
    synthetic_run(work, existing='different', bad_signer=True)
    synthetic_run(work, existing='different', other_user=True, other_state='empty')
    synthetic_run(work, existing='different', other_user=True, other_state='retained')
    synthetic_run(work, existing='different', other_user=True, other_state='unknown')
    print(json.dumps({'state':'PASS_OFFLINE_SYNTHETIC_NO_DEVICE',
        'coverage':['report', 'copy_arena_scope_and_markers', 'copy_arena_plan_and_dispatch', 'known_only_empty_other_user', 'retained_or_unknown_other_user_blocked', 'admission', 'atomic_receipt', 'private_plan_default_and_no_overwrite', 'shared_source_drift', 'artifact_identity', 'frozen_plan_hash', 'success', 'fail', 'freeze', 'timeout',
                    'late_exit', 'cross_user', 'install_unknown', 'postflight_drift', 'busy_before_install', 'same_signer_and_other_user_identity', 'no_remote_force_stop']}))

if __name__ == '__main__': main()
