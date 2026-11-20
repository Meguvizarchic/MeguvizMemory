#!/usr/bin/env python3
"""Build the framework/JNI diagnostic APK and standalone AAR without Gradle.

Uses an existing SDK/JDK and already built shared Memory/JNI artifacts. The
generated signing key is diagnostic-only and remains inside ignored build/.
No ADB, install, app-data or global Android-setting operation is performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]


def verify_elf_abi(path,abi):
    expected={'arm64-v8a':(2,183),'x86_64':(2,62),'armeabi-v7a':(1,40)}[abi]
    with path.open('rb') as source:
        header=source.read(20)
    if (len(header)<20 or header[:4]!=b'\x7fELF' or header[5]!=1 or
            (header[4],struct.unpack_from('<H',header,18)[0])!=expected):
        raise ValueError('Native input ELF does not match selected Android ABI')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    parser.add_argument('--java-home',type=Path,required=True)
    parser.add_argument('--memory-sdk',type=Path,required=True)
    parser.add_argument('--native-dir',type=Path,required=True)
    parser.add_argument('--out-dir',type=Path,required=True)
    parser.add_argument('--receipt',type=Path,required=True)
    parser.add_argument('--diagnostic-keystore',type=Path,default=ROOT/'build/features-signing/diagnostic.keystore')
    parser.add_argument('--build-tools',default='37.0.0')
    parser.add_argument('--platform',default='android-37.0')
    parser.add_argument('--abi',choices=('arm64-v8a','x86_64','armeabi-v7a'),default='arm64-v8a')
    args=parser.parse_args()
    out=args.out_dir.resolve()
    if not out.is_relative_to(ROOT/'build') or out.exists():parser.error('Use a new output directory below build/')
    out.mkdir(parents=True)
    tools=args.sdk/'build-tools'/args.build_tools; android=args.sdk/'platforms'/args.platform/'android.jar'
    jdk=args.java_home/'bin'; suffix='.exe' if __import__('os').name=='nt' else ''
    java,javac,jar,keytool=(jdk/(name+suffix) for name in ('java','javac','jar','keytool'))
    native=args.native_dir/'libmgmem_pressure_jni.so'; memory=args.memory_sdk/'lib/libmgmem.so'
    required=[java,javac,jar,keytool,android,tools/('aapt2'+suffix),tools/('zipalign'+suffix),
              tools/'lib/d8.jar',tools/'lib/apksigner.jar',native,memory]
    if not all(p.is_file() for p in required):parser.error('Existing compiler/SDK/shared JNI and Memory inputs are required')
    for library in (native,memory):verify_elf_abi(library,args.abi)
    operations=[]
    def run(label,command):
        done=subprocess.run([str(x) for x in command],capture_output=True,text=True,check=False)
        (out/(label+'.stdout')).write_text(done.stdout,encoding='utf-8')
        (out/(label+'.stderr')).write_text(done.stderr,encoding='utf-8')
        operations.append({'operation':label,'exit_code':done.returncode,
            'stdout_sha256':hashlib.sha256(done.stdout.encode()).hexdigest(),
            'stderr_sha256':hashlib.sha256(done.stderr.encode()).hexdigest()})
        if done.returncode:raise RuntimeError('Build stage failed: '+label+'; see ignored output logs')
        return done.stdout
    library_sources=sorted((ROOT/'adapters/android/java').rglob('*.java'))
    classes=out/'library-classes';classes.mkdir()
    run('javac-library',[javac,'--release','8','-classpath',android,'-d',classes]+library_sources)
    library_jar=out/'classes.jar'
    run('jar-library',[jar,'--create','--file',library_jar,'-C',classes,'.'])
    sample_classes=out/'sample-classes';sample_classes.mkdir()
    separator=';' if suffix else ':'
    sample=ROOT/'adapters/android/sample/PressureInstrumentation.java'
    run('javac-sample',[javac,'--release','8','-classpath',str(android)+separator+str(library_jar),'-d',sample_classes,sample])
    sample_jar=out/'sample.jar';run('jar-sample',[jar,'--create','--file',sample_jar,'-C',sample_classes,'.'])
    dex=out/'dex';dex.mkdir()
    run('d8',[java,'-cp',tools/'lib/d8.jar','com.android.tools.r8.D8','--min-api','27','--lib',android,
              '--output',dex,library_jar,sample_jar])
    unsigned=out/'pressure-unsigned.apk'
    run('aapt2-link',[tools/('aapt2'+suffix),'link','-I',android,'--manifest',ROOT/'adapters/android/sample/AndroidManifest.xml',
                      '--min-sdk-version','27','--target-sdk-version','33','-o',unsigned])
    with zipfile.ZipFile(unsigned,'a',compression=zipfile.ZIP_STORED) as package:
        package.write(dex/'classes.dex','classes.dex')
        package.write(native,f'lib/{args.abi}/libmgmem_pressure_jni.so')
        package.write(memory,f'lib/{args.abi}/libmgmem.so')
    aligned=out/'pressure-aligned.apk'
    run('zipalign',[tools/('zipalign'+suffix),'-P','16','-f','4',unsigned,aligned])
    key=args.diagnostic_keystore.resolve()
    if not key.is_relative_to(ROOT/'build'):parser.error('Diagnostic key must remain inside ignored build/')
    key.parent.mkdir(parents=True,exist_ok=True)
    if not key.exists():
        run('diagnostic-key',[keytool,'-genkeypair','-keystore',key,'-storepass','android','-keypass','android',
            '-alias','androiddebugkey','-keyalg','RSA','-keysize','2048','-validity','3650',
            '-dname','CN=MeguvizMemory Diagnostic,O=MeguvizMemory','-noprompt'])
    else:
        run('verify-diagnostic-key',[keytool,'-list','-keystore',key,'-storepass','android','-alias','androiddebugkey'])
    apk=out/'MeguvizMemory-Pressure-Probe.apk'
    run('sign',[java,'-jar',tools/'lib/apksigner.jar','sign','--ks',key,'--ks-pass','pass:android',
                '--key-pass','pass:android','--ks-key-alias','androiddebugkey','--out',apk,aligned])
    signatures=run('verify-signature',[java,'-jar',tools/'lib/apksigner.jar','verify','--print-certs',apk])
    certificate=re.findall(r'certificate SHA-256 digest:\s*([0-9a-fA-F]+)',signatures)
    if len(certificate)!=1:raise RuntimeError('One diagnostic signing certificate expected')
    aar=out/'MeguvizMemory-Android-Pressure-1.2.2.aar'
    manifest=b'<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="io.github.jichuo1.meguviz.memory.android"><uses-sdk android:minSdkVersion="27" /></manifest>\n'
    with zipfile.ZipFile(aar,'x',compression=zipfile.ZIP_DEFLATED) as package:
        package.writestr('AndroidManifest.xml',manifest);package.write(library_jar,'classes.jar')
        package.write(native,f'jni/{args.abi}/libmgmem_pressure_jni.so');package.write(memory,f'jni/{args.abi}/libmgmem.so')
        package.write(ROOT/'adapters/android/consumer-rules.pro','proguard.txt')
        package.write(ROOT/'third_party_notices/llvm-NOTICE.txt','META-INF/NOTICE-LLVM.txt')
        package.writestr('R.txt','')
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    relative=lambda p:p.resolve().relative_to(ROOT).as_posix()
    sources=library_sources+sorted((ROOT/'adapters/android/native').glob('*.cpp'))+sorted((ROOT/'include/mgmem').glob('*.h'))+[
              sample,ROOT/'adapters/android/sample/AndroidManifest.xml',ROOT/'adapters/android/CMakeLists.txt',
              ROOT/'adapters/android/consumer-rules.pro',ROOT/'third_party_notices/llvm-NOTICE.txt',Path(__file__).resolve()]
    receipt={'schema_version':1,'scope':'ANDROID_PRESSURE_BUILD_AND_DIAGNOSTIC_SIGNING_ONLY',
        'status':'PASS','device_executed':False,'performance_qualified':False,'minimum_api':27,
        'compile_platform':args.platform,'build_tools':args.build_tools,'target_api':33,'abi':args.abi,
        'package_name':'io.github.jichuo1.meguviz.memory.pressureprobe',
        'instrumentation':'io.github.jichuo1.meguviz.memory.android.PressureInstrumentation',
        'certificate_sha256':certificate[0].lower(),'signing':'ISOLATED_DIAGNOSTIC_KEY_NOT_PRODUCTION_SIGNING',
        'artifacts':[{'path':relative(p),'bytes':p.stat().st_size,'sha256':sha(p)} for p in (apk,aar,library_jar,native,memory)],
        'sources':[{'path':relative(p),'sha256':sha(p)} for p in sources],'operations':operations,
        'runtime_closure':'Exactly one installed shared libmgmem.so; pressure helper is linked into JNI, not a second Memory core',
        'limitations':['APK framework dispatch test is synthetic; no real low-RAM event or energy gain is claimed.',
                       'JNI and Memory binaries are prebuilt inputs; source hashes alone do not prove how they were compiled.',
                       'AAR includes keep rules, but no product R8/BIL/Lumen qualification was performed.']}
    args.receipt.parent.mkdir(parents=True,exist_ok=True);args.receipt.write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({'status':'PASS','apk':relative(apk),'aar':relative(aar),'device_executed':False}))
if __name__=='__main__':main()
