#!/usr/bin/env python3
"""Build a real R8 consumer from an existing pressure AAR; never invokes ADB.

The input AAR supplies classes, JNI keep rules, and exactly one shared core.
Only the diagnostic consumer is compiled here. Outputs use a fresh build/
directory; the signing key stays in ignored build/ and is never packaged.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile
import xml.etree.ElementTree as ET

from build_packed_art import dex_classes

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "io.github.jichuo1.meguviz.memory.pressureprobe.r8"
INSTRUMENT = "io.github.jichuo1.meguviz.memory.android.PressureR8Instrumentation"
ACTIVITY = "io.github.jichuo1.meguviz.memory.android.PressureR8ForegroundActivity"
NATIVE = "io.github.jichuo1.meguviz.memory.android.NativePressureBackend"
MARKER = "io.github.jichuo1.meguviz.memory.android.R8ConsumerMarker"
UNUSED = "io.github.jichuo1.meguviz.memory.android.R8UnusedMarker"


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--aar", type=Path, default=ROOT / "dist/MeguvizMemory-Android-Pressure-1.2.2.aar")
    parser.add_argument("--abi", choices=("arm64-v8a", "x86_64"), default="arm64-v8a")
    parser.add_argument("--memory-sdk", type=Path, help="paired override: installed shared SDK for selected ABI")
    parser.add_argument("--native-dir", type=Path, help="paired override: freshly built JNI for selected ABI")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--diagnostic-keystore", type=Path, default=ROOT / "build/features-signing/diagnostic.keystore")
    parser.add_argument("--build-tools", default="37.0.0")
    parser.add_argument("--platform", default="android-37.0")
    args = parser.parse_args()
    out = args.out_dir.resolve()
    require(out.is_relative_to(ROOT / "build") and not out.exists(), "fresh_build_directory_required")
    receipt_roots = (ROOT / "artifacts/continuation-20261030", ROOT / "artifacts/android-close-investigation-20261030")
    require(any(args.receipt.resolve().is_relative_to(root) for root in receipt_roots), "scoped_receipt_required")
    out.mkdir(parents=True)
    sdk_tools = args.sdk / "build-tools" / args.build_tools
    android = args.sdk / "platforms" / args.platform / "android.jar"
    suffix = ".exe" if os.name == "nt" else ""
    java, javac, jar, keytool = (args.java_home / "bin" / (name + suffix) for name in ("java", "javac", "jar", "keytool"))
    aapt, align = (sdk_tools / (name + suffix) for name in ("aapt2", "zipalign"))
    r8, signer = sdk_tools / "lib/d8.jar", sdk_tools / "lib/apksigner.jar"
    require(all(path.is_file() for path in (java, javac, jar, keytool, aapt, align, android, r8, signer, args.aar)), "existing_toolchain_and_aar_required")
    require(bool(args.memory_sdk) == bool(args.native_dir), "both_native_override_arguments_required")
    operations = []

    def run(label, command, timeout=120):
        result = subprocess.run([str(item) for item in command], cwd=out, capture_output=True, timeout=timeout, check=False)
        (out / (label + ".stdout")).write_bytes(result.stdout)
        (out / (label + ".stderr")).write_bytes(result.stderr)
        operations.append({"operation": label, "exit_code": result.returncode,
                           "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
                           "stderr_sha256": hashlib.sha256(result.stderr).hexdigest()})
        require(result.returncode == 0, label + "_failed_see_ignored_logs")
        return result.stdout.decode("utf-8", "replace")

    input_aar = args.aar
    overrides = []
    if args.memory_sdk:
        native = args.native_dir.resolve() / "libmgmem_pressure_jni.so"
        core = args.memory_sdk.resolve() / "lib/libmgmem.so"
        require(native.is_file() and core.is_file(), "shared_native_overrides_required")
        # e_machine is checked before creating the derivative AAR. This is a
        # distinct artifact, not a claim that the original AAR contains x86.
        for label, path in (("jni", native), ("core", core)):
            raw = path.read_bytes()
            require(raw[:6] == b"\x7fELF\x02\x01" and int.from_bytes(raw[18:20], "little") == {"arm64-v8a": 183, "x86_64": 62}[args.abi], "override_abi_mismatch")
            overrides.append({"kind": label, "path": path.resolve().relative_to(ROOT).as_posix(), "sha256": digest(path)})
        input_aar = out / ("MeguvizMemory-Android-Pressure-1.2.2-" + args.abi + ".aar")
        with zipfile.ZipFile(args.aar) as base, zipfile.ZipFile(input_aar, "x", compression=zipfile.ZIP_DEFLATED) as derived:
            require(len(base.namelist()) == len(set(base.namelist())), "duplicate_base_aar_members")
            for name in base.namelist():
                if not name.startswith("jni/"):
                    require(not name.startswith("/") and ".." not in Path(name).parts, "invalid_base_aar_member")
                    derived.writestr(name, base.read(name))
            derived.write(native, "jni/" + args.abi + "/libmgmem_pressure_jni.so")
            derived.write(core, "jni/" + args.abi + "/libmgmem.so")
    unpacked = out / "aar-input"; unpacked.mkdir()
    needed = ("classes.jar", "proguard.txt", "jni/" + args.abi + "/libmgmem_pressure_jni.so", "jni/" + args.abi + "/libmgmem.so")
    members = []
    with zipfile.ZipFile(input_aar) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)), "duplicate_aar_members")
        native_members = sorted(name for name in names if name.endswith(".so"))
        require(native_members == sorted(needed[2:]), "one_selected_abi_shared_core_and_jni_required")
        for name in needed:
            data = archive.read(name)
            target = unpacked / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
            members.append({"member": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    library = unpacked / "classes.jar"
    source = ROOT / "adapters/android/sample/PressureR8Instrumentation.java"
    activity_source = ROOT / "adapters/android/sample/PressureR8ForegroundActivity.java"
    manifest = ROOT / "adapters/android/sample/AndroidManifest-r8.xml"
    manifest_root = ET.parse(manifest).getroot()
    application = manifest_root.find("application")
    require(application is not None, "diagnostic_application_required")
    components = [entry for entry in application if entry.tag in ("activity", "activity-alias", "receiver", "service", "provider")]
    namespace = "{http://schemas.android.com/apk/res/android}"
    require(len(components) == 1 and components[0].tag == "activity" and
            components[0].get(namespace + "name") == ACTIVITY and
            components[0].get(namespace + "exported") == "true" and
            components[0].get(namespace + "permission") == "android.permission.DUMP" and
            components[0].find("intent-filter") is None, "diagnostic_bootstrap_permission_or_components_mismatch")
    classes = out / "consumer-classes"; classes.mkdir()
    separator = ";" if os.name == "nt" else ":"
    run("javac-consumer", [javac, "--release", "8", "-classpath", str(android) + separator + str(library), "-d", classes, source, activity_source])
    consumer_jar = out / "consumer.jar"
    run("jar-consumer", [jar, "--create", "--file", consumer_jar, "-C", classes, "."])
    dex = out / "r8-dex"; dex.mkdir()
    config = out / "consumer-r8.pro"
    # Only manifest entries and a renameable witness are added. Native names and
    # onNativeClosed are retained exclusively by the shipped AAR rules.
    config.write_text("-keep class " + INSTRUMENT + " { *; }\n"
                      "-keep class " + ACTIVITY + " { *; }\n"
                      "-keep,allowobfuscation class " + MARKER + " { *; }\n"
                      "-printusage usage.txt\n"
                      "-printseeds seeds.txt\n", encoding="utf-8", newline="\n")
    version = run("r8-version", [java, "-cp", r8, "com.android.tools.r8.R8", "--version"]).strip()
    run("r8-consumer", [java, "-cp", r8, "com.android.tools.r8.R8", "--release", "--min-api", "27", "--lib", android,
                        "--pg-conf", unpacked / "proguard.txt", "--pg-conf", config,
                        "--pg-map-output", out / "mapping.txt", "--output", dex, library, consumer_jar])
    mapping = (out / "mapping.txt").read_text(encoding="utf-8")
    usage = (out / "usage.txt").read_text(encoding="utf-8")
    renamed = re.search(r"^" + re.escape(MARKER) + r" -> ([^:]+):$", mapping, re.MULTILINE)
    require(renamed and renamed.group(1) != MARKER, "real_obfuscation_not_demonstrated")
    require(UNUSED in usage, "real_dead_class_removal_not_demonstrated")
    require(re.search(r"^" + re.escape(NATIVE) + r" -> " + re.escape(NATIVE) + r":$", mapping, re.MULTILINE), "native_class_name_not_retained")
    require(re.search(r"\bonNativeClosed\(int\)(?::[0-9]+)* -> onNativeClosed$", mapping, re.MULTILINE), "jni_completion_method_not_retained")
    dex_files = sorted(dex.glob("*.dex")); require(len(dex_files) == 1, "one_dex_expected")
    descriptors = dex_classes(dex_files[0].read_bytes())
    desc = lambda name: "L" + name.replace(".", "/") + ";"
    require(desc(NATIVE) in descriptors and desc(INSTRUMENT) in descriptors and desc(ACTIVITY) in descriptors, "native_or_manifest_entry_missing")
    require(re.search(r"^" + re.escape(ACTIVITY) + r" -> " + re.escape(ACTIVITY) + r":$", mapping, re.MULTILINE), "diagnostic_activity_name_not_retained")
    require(desc(renamed.group(1)) in descriptors and desc(UNUSED) not in descriptors, "dex_shrink_rename_evidence_missing")
    unsigned = out / "r8-unsigned.apk"
    run("aapt2-link", [aapt, "link", "-I", android, "--manifest", manifest, "--min-sdk-version", "27", "--target-sdk-version", "35", "-o", unsigned])
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_STORED) as package:
        package.write(dex_files[0], "classes.dex")
        for member in needed[2:]: package.write(unpacked / member, "lib/" + args.abi + "/" + Path(member).name)
    aligned = out / "r8-aligned.apk"
    run("zipalign", [align, "-P", "16", "-f", "4", unsigned, aligned])
    key = args.diagnostic_keystore.resolve()
    require(key.is_relative_to(ROOT / "build") and key.is_file(), "existing_ignored_diagnostic_key_required")
    run("verify-diagnostic-key", [keytool, "-list", "-keystore", key, "-storepass", "android", "-alias", "androiddebugkey"])
    apk = out / "MeguvizMemory-R8-Stability-Probe.apk"
    run("sign", [java, "-jar", signer, "sign", "--ks", key, "--ks-pass", "pass:android", "--key-pass", "pass:android", "--ks-key-alias", "androiddebugkey", "--out", apk, aligned])
    signature = run("verify-signature", [java, "-jar", signer, "verify", "--print-certs", apk])
    cert = re.findall(r"certificate SHA-256 digest:\s*([0-9a-fA-F]+)", signature)
    require(len(cert) == 1, "one_diagnostic_certificate_required")
    run("zipalign-verify", [align, "-c", "-P", "16", "-v", "4", apk])
    with zipfile.ZipFile(apk) as package:
        require(package.read("lib/" + args.abi + "/libmgmem.so") == (unpacked / needed[3]).read_bytes(), "core_changed_during_packaging")
        require(package.read("lib/" + args.abi + "/libmgmem_pressure_jni.so") == (unpacked / needed[2]).read_bytes(), "jni_changed_during_packaging")
    relative = lambda path: Path(path).resolve().relative_to(ROOT).as_posix()
    identity = lambda path: {"path": relative(path), "bytes": Path(path).stat().st_size, "sha256": digest(path)}
    evidence = {"schema_version": 1, "scope": "REAL_AAR_R8_CONSUMER_BUILD_NOT_DEVICE_EXECUTION", "status": "PASS",
                "package_name": PACKAGE, "instrumentation": INSTRUMENT, "abi": args.abi, "minimum_api": 27, "target_api": 35,
                "compile_platform": args.platform, "build_tools": args.build_tools, "r8_version": version,
                "input_aar": identity(input_aar), "base_aar": identity(args.aar), "native_overrides": overrides,
                "aar_members": members, "certificate_sha256": cert[0].lower(),
                "signing": "EXISTING_DIAGNOSTIC_KEY_NOT_PRODUCTION", "artifacts": [identity(path) for path in (apk, out / "mapping.txt", out / "usage.txt", out / "seeds.txt", config, dex_files[0])],
                "sources": [identity(path) for path in (source, activity_source, manifest, Path(__file__), ROOT / "tools/build_packed_art.py")],
                "toolchain": [{"name": path.name, "sha256": digest(path)} for path in (r8, android, signer)],
                "r8_evidence": {"shrinking_enabled": True, "optimization_enabled": True, "obfuscation_enabled": True,
                                "marker_before": MARKER, "marker_after": renamed.group(1), "unused_class_removed": UNUSED,
                                "runtime_marker_validation": "Instrumentation reports the name; external runner must compare it to this exact marker_after. No original-name literal comparison inside R8 input.",
                                "native_class_name_retained": NATIVE, "native_completion_method_retained": "onNativeClosed(int)",
                                "foreground_activity_name_retained": ACTIVITY,
                                "foreground_bootstrap": "EXPORTED_EXPLICIT_ONLY_REQUIRES_ANDROID_PERMISSION_DUMP",
                                "native_keep_rule_source": "AAR proguard.txt unchanged", "dex_class_count": len(descriptors)},
                "runtime_closure": "Exactly the AAR libmgmem.so and libmgmem_pressure_jni.so; no second Memory core",
                "device_executed": False, "performance_qualified": False, "BIL_integrated": False, "operations": operations,
                "limitations": ["R8 build evidence does not prove runtime JNI completion; instrumentation must execute on an admitted device.",
                                "API34+ old automatic low-memory signals are not assumed; synthetic dispatch and own-package AMS outcomes are recorded separately.",
                                "Bounded cycles and own-process RSS/PSS observations do not prove indefinite leak freedom, savings or energy benefit."]}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"status": "PASS", "apk": identity(apk), "marker_after": renamed.group(1), "device_executed": False}))


if __name__ == "__main__":
    main()
