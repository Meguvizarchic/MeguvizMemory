#!/usr/bin/env python3
"""One-command host verification for every adapters/* CMake project.

Each bridge is an independent CMake project with its own find_package needs,
test options and install rules; this runner encodes that matrix once so a
single invocation answers "do the bridges still build and pass host tests".

It never builds the external Compute/Media SDKs: pass their installed
prefixes. The Memory prefix may be built on demand with --build-core.
Android-only targets are out of scope (host fakes only where a project
provides them) and are reported as SKIP, never as passes.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def find_cmake_tools():
    cmake = shutil.which("cmake")
    ctest = shutil.which("ctest")
    if cmake and ctest:
        return cmake, ctest, None
    # Windows fallback: the Visual Studio bundled CMake used by host-build.cmd.
    vs_root = os.environ.get("MGMEM_VS_ROOT", r"C:\Program Files\Microsoft Visual Studio\18\Community")
    bin_dir = Path(vs_root) / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
    vcvars = Path(vs_root) / "VC/Auxiliary/Build/vcvars64.bat"
    if (bin_dir / "cmake.exe").exists() and vcvars.exists():
        return str(bin_dir / "cmake.exe"), str(bin_dir / "ctest.exe"), str(vcvars)
    raise SystemExit("cmake/ctest not found on PATH and no Visual Studio bundled CMake located")


CMAKE, CTEST, VCVARS = find_cmake_tools()
BASE_ENV = dict(os.environ)
if VCVARS and sys.platform == "win32":
    # Import the vcvars environment once; running CMake through `cmd /c`
    # string concatenation would split ;-separated prefix lists.
    out = subprocess.run(["cmd", "/c", 'call "%s" >nul 2>&1 && set' % VCVARS],
                         capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            # Windows environment keys are case-insensitive; keep exactly one
            # spelling or the child process may see a PATH without our dirs.
            for existing in list(BASE_ENV):
                if existing.lower() == key.lower():
                    del BASE_ENV[existing]
            BASE_ENV[key] = value


def run(argv, log_path, env=None, timeout=900):
    """Run one command, tee output to a log, return (exit_code, seconds)."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        log.write("+ %s\n" % " ".join(argv))
        proc = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT,
                              env=env, timeout=timeout)
    return proc.returncode, time.monotonic() - started


def tail(path, lines=30):
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(text[-lines:])


def count_tests(build_dir, env):
    proc = subprocess.run([CTEST, "--test-dir", str(build_dir), "-C", "Release", "-N"],
                          capture_output=True, text=True, env=env)
    for line in proc.stdout.splitlines():
        if "Total Tests:" in line:
            return int(line.rsplit(":", 1)[1].strip())
    return -1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--memory-prefix", help="installed MeguvizMemory prefix")
    ap.add_argument("--build-core", action="store_true", help="build+install core first")
    ap.add_argument("--compute-prefix", required=True, help="installed MeguvizCompute prefix")
    ap.add_argument("--media-prefix", help="installed MeguvizMedia prefix (media SKIPs without it)")
    ap.add_argument("--build-root", default="build/verify-adapters")
    ap.add_argument("--msvc-runtime", help="forwarded unchanged to every configure, e.g. MultiThreaded")
    ap.add_argument("--only", help="comma-separated step names to run")
    ap.add_argument("--keep-going", action="store_true")
    ap.add_argument("--strict", action="store_true", help="capability downgrade counts as failure")
    ap.add_argument("--receipt", help="write a JSON receipt to this path")
    args = ap.parse_args()

    build_root = (ROOT / args.build_root).resolve()
    install_root = build_root / "install"
    logs = build_root / "logs"
    only = set(args.only.split(",")) if args.only else None

    def precheck(prefix, package, origin):
        cfg = Path(prefix) / "lib/cmake" / package / (package + "Config.cmake")
        if not cfg.exists():
            print("MISSING %s config at %s (expected from: %s)" % (package, cfg, origin))
            return False
        return True

    results = []

    def record(name, status, detail="", tests=None, seconds=0.0):
        results.append({"step": name, "status": status, "detail": detail,
                        "tests": tests, "seconds": round(seconds, 1)})
        print("[%-4s] %-16s %s" % (status, name, detail))

    memory_prefix = args.memory_prefix
    if args.build_core:
        memory_prefix = str(install_root / "memory")
    if not memory_prefix:
        raise SystemExit("pass --memory-prefix or --build-core")
    if not precheck(args.compute_prefix, "MeguvizCompute", "external Compute SDK install"):
        raise SystemExit(2)
    if args.media_prefix and not precheck(args.media_prefix, "MeguvizMedia", "external Media SDK install"):
        raise SystemExit(2)
    if not args.build_core and not precheck(memory_prefix, "MeguvizMemory", "--memory-prefix"):
        raise SystemExit(2)

    env = dict(BASE_ENV)
    # Absolute only: test processes run with the build dir as cwd, so a
    # relative PATH entry would silently miss the shared libraries.
    path_prefix = os.pathsep.join(str(Path(p).resolve() / "bin") for p in
                                   [memory_prefix, args.compute_prefix] )
    old_path = next((v for k, v in env.items() if k.lower() == "path"), "")
    for existing in list(env):
        if existing.lower() == "path":
            del env[existing]
    env["PATH"] = path_prefix + os.pathsep + old_path

    common = ["-DCMAKE_BUILD_TYPE=Release"]
    if args.msvc_runtime:
        common.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=" + args.msvc_runtime)
    print("CRT: %s" % (args.msvc_runtime or "unset (toolchain default)"))

    def step(name, source, prefixes, extra, install_to, expect_tests, expect_alt=None):
        if only and name not in only:
            return True
        build_dir = build_root / name
        argv = [CMAKE, "-S", str(ROOT / source), "-B", str(build_dir)] + common
        if prefixes:
            argv.append("-DCMAKE_PREFIX_PATH=" + ";".join(prefixes))
        if install_to:
            argv.append("-DCMAKE_INSTALL_PREFIX=" + str(install_root / install_to))
        argv += extra
        total = 0.0
        code, secs = run(argv, logs / (name + "-configure.log"), env)
        total += secs
        if code != 0:
            record(name, "FAIL", "configure failed\n" + tail(logs / (name + "-configure.log")), seconds=total)
            return False
        code, secs = run([CMAKE, "--build", str(build_dir), "--config", "Release", "--parallel", "2"],
                         logs / (name + "-build.log"), env)
        total += secs
        if code != 0:
            record(name, "FAIL", "build failed\n" + tail(logs / (name + "-build.log")), seconds=total)
            return False
        found = count_tests(build_dir, env)
        downgraded = expect_alt is not None and found == expect_alt and found != expect_tests
        if expect_tests is not None and found != expect_tests and not downgraded:
            record(name, "FAIL", "test count %d, expected %d" % (found, expect_tests), tests=found, seconds=total)
            return False
        code, secs = run([CTEST, "--test-dir", str(build_dir), "-C", "Release", "--output-on-failure"],
                         logs / (name + "-test.log"), env)
        total += secs
        if code != 0:
            record(name, "FAIL", "ctest failed\n" + tail(logs / (name + "-test.log")), tests=found, seconds=total)
            return False
        if install_to:
            code, secs = run([CMAKE, "--install", str(build_dir), "--config", "Release"],
                             logs / (name + "-install.log"), env)
            total += secs
            if code != 0:
                record(name, "FAIL", "install failed\n" + tail(logs / (name + "-install.log")), tests=found, seconds=total)
                return False
        if downgraded:
            status = "FAIL" if args.strict else "WARN"
            record(name, status, "ARENA_DOWNGRADED: %d tests (probe missed; arena path untested)" % found,
                   tests=found, seconds=total)
            return not args.strict
        record(name, "PASS", "%d tests" % found, tests=found, seconds=total)
        return True

    ok = True
    if args.build_core:
        ok = step("core", ".", [], [], "memory", None)
        if not ok and not args.keep_going:
            return finish(args, results, ok)
    mem, comp = str(Path(memory_prefix).resolve()), str(Path(args.compute_prefix).resolve())
    steps = [
        ("compute", "adapters/compute", [mem, comp],
         ["-DMGMEM_COMPUTE_BRIDGE_BUILD_TESTS=ON", "-DMGMEM_COMPUTE_BRIDGE_BUILD_ARENA=ON"],
         "compute-bridge", 7, 4),
        ("compute_host", "adapters/compute_host", [mem, comp],
         ["-DMGMEM_COMPUTE_HOST_BRIDGE_BUILD_TESTS=ON"], "compute-host-bridge", 6, None),
        ("compute_ahb", "adapters/compute_ahb", [mem, comp], [], None, 14, None),
        ("compute_gpu_ahb", "adapters/compute_gpu_ahb", [mem, comp], [], None, 3, None),
        ("gpu_ahb_gles", "adapters/gpu_ahb_gles", [], [], None, 24, None),
    ]
    for entry in steps:
        if not step(*entry):
            ok = False
            if not args.keep_going:
                return finish(args, results, ok)
    if args.media_prefix:
        media = str(Path(args.media_prefix).resolve())
        if not step("media", "adapters/media",
                    [mem, comp, media, str(install_root / "compute-bridge")],
                    ["-DMGMEM_MEDIA_BRIDGE_BUILD_TESTS=ON"], "media-bridge", 3, None):
            ok = False
    else:
        record("media", "SKIP", "no --media-prefix given")
    record("android", "SKIP", "host run: adapters/android requires the NDK toolchain")
    return finish(args, results, ok)


def finish(args, results, ok):
    failed = [r for r in results if r["status"] == "FAIL"]
    summary = {"state": "PASS" if ok and not failed else "FAIL", "steps": results}
    print(json.dumps({"state": summary["state"],
                      "passed": sum(r["status"] == "PASS" for r in results),
                      "warned": sum(r["status"] == "WARN" for r in results),
                      "skipped": sum(r["status"] == "SKIP" for r in results),
                      "failed": len(failed)}))
    if args.receipt:
        Path(args.receipt).write_text(json.dumps(summary, indent=1), encoding="utf-8")
    return 0 if summary["state"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
