#!/usr/bin/env python3
"""Run balanced, isolated Arena API pairs; retain local raw observations."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def expected_checksum(shape, threads, allocations):
    # Sum consecutive modulo-256 byte values independently of the C++ read loop.
    result = 0
    for worker in range(threads):
        for operation in range(allocations):
            count = 64 if shape == "fixed" else (operation % 4 + 1) * 32
            first = (worker * 17 + operation * 31) % 256
            before_wrap = min(count, 256 - first)
            result += before_wrap * (2 * first + before_wrap - 1) // 2
            tail = count - before_wrap
            result += tail * (tail - 1) // 2
    return result


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * fraction + 0.5)]


def runtimes(directory):
    directory = directory.resolve(strict=True)
    require(directory.is_dir(), "runtime_directory_missing")
    if sys.platform == "win32":
        selected = [directory / "mgmem.dll"]
    elif sys.platform.startswith("linux"):
        selected = sorted(directory.glob("libmgmem.so*"))
    else:
        raise ValueError("this_controller_supports_windows_and_linux")
    require(selected and all(path.is_file() for path in selected), "runtime_library_missing")
    return [(path, sha(path)) for path in selected]


def validate(native, shape, threads, args, staged):
    require(isinstance(native, dict) and native.get("schema_version") == 1 and
            native.get("workload") == "arena_lookup_composite_v1" and native.get("success") is True,
            "native_not_success")
    require(native.get("shape") == shape and native.get("threads") == threads and
            native.get("allocations_per_thread") == args.allocations and
            native.get("batch") == args.batch and native.get("rounds") == args.rounds and
            native.get("warmup_rounds") == args.warmup and native.get("chunk_bytes") == 4096,
            "workload_identity_changed")
    loaded = Path(native.get("runtime_module", "")).resolve(strict=True)
    require(loaded in staged and sha(loaded) == staged[loaded], "loaded_runtime_not_selected_bytes")
    per_round = expected_checksum(shape, threads, args.allocations)
    operations = args.allocations * threads
    require(native.get("checksum") == per_round * args.rounds and
            native.get("completed_operations") == operations * args.rounds and
            native.get("final_resources") == "CLOSED_ZERO" and native.get("performance_qualified") is False,
            "oracle_or_final_resources_mismatch")
    records = native.get("round_records")
    require(isinstance(records, list) and len(records) == args.rounds, "round_count_mismatch")
    samples, workflow, total = [], [], 0
    for row in records:
        require(isinstance(row, dict) and row.get("checksum") == per_round and
                row.get("completed_operations") == operations and
                type(row.get("workflow_ns")) is int and row["workflow_ns"] > 0 and
                type(row.get("close_ns")) is int and row["close_ns"] >= 0 and
                type(row.get("peak_backing_bytes")) is int and row["peak_backing_bytes"] > 0 and
                type(row.get("provider_allocation_count")) is int and row["provider_allocation_count"] > 1,
                "round_result_mismatch")
        values = row.get("allocation_batch_ns_per_op")
        require(isinstance(values, list) and
                len(values) == threads * math.ceil(args.allocations / args.batch) and
                all(type(value) in (float, int) and math.isfinite(value) and value > 0 for value in values),
                "allocation_samples_invalid")
        samples.extend(values)
        workflow.append(row["workflow_ns"] / operations)
        total += row["workflow_ns"]
    expected = {
        "allocation_batch_ns_per_op_p50": percentile(samples, .5),
        "allocation_batch_ns_per_op_p95": percentile(samples, .95),
        "workflow_round_ns_per_op_p50": percentile(workflow, .5),
        "workflow_round_ns_per_op_p95": percentile(workflow, .95),
        "workflow_operations_per_second": operations * args.rounds * 1e9 / total,
    }
    require(all(type(native.get(key)) in (int, float) and
                math.isclose(native[key], value, rel_tol=1e-10, abs_tol=1e-6)
                for key, value in expected.items()), "summary_does_not_match_raw_records")
    return native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--baseline-runtime-dir", type=Path, required=True)
    parser.add_argument("--candidate-runtime-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=ROOT / "build/arena-lookup-pairs")
    parser.add_argument("--pairs", type=int, default=4)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--allocations", type=int, default=4096)
    parser.add_argument("--batch", type=int, default=64)
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    require(2 <= args.pairs <= 32 and args.pairs % 2 == 0, "pairs_must_be_even_between_2_and_32")
    require(args.threads and len(set(args.threads)) == len(args.threads) and
            all(1 <= count <= 16 for count in args.threads), "invalid_thread_counts")
    require(128 <= args.allocations <= 65536 and
            args.allocations * max(args.threads) <= 262144 and 1 <= args.batch <= 256 and
            2 <= args.rounds <= 64 and 0 <= args.warmup <= 16 and 1 <= args.timeout <= 600 and
            math.ceil(args.allocations / args.batch) * max(args.threads) * args.rounds <= 262144,
            "invalid_workload_bounds")
    executable = args.exe.resolve(strict=True)
    require(executable.is_file(), "executable_missing")
    libraries = {"baseline": runtimes(args.baseline_runtime_dir),
                 "candidate": runtimes(args.candidate_runtime_dir)}
    output = args.out_dir.resolve()
    output.relative_to((ROOT / "build").resolve())
    output.mkdir(parents=True, exist_ok=False)
    receipt = {"schema_version": 1, "state": "INCOMPLETE", "performance_qualified": False,
               "scope": "SYNTHETIC_SHARED_DOMAIN_API_NOT_PRODUCT", "executable_sha256": sha(executable),
               "source_sha256": {name: sha(ROOT / name) for name in
                                 ("bench/arena_lookup.cpp", "tools/run_arena_lookup_pairs.py")},
               "configuration": {"pairs": args.pairs, "threads": args.threads,
                                 "allocations": args.allocations, "batch": args.batch,
                                 "rounds": args.rounds, "warmup": args.warmup},
               "runtime_files": {label: [{"name": path.name, "sha256": digest} for path, digest in items]
                                 for label, items in libraries.items()},
               "runs": [], "summaries": [], "failure": None}
    save(output / "receipt.json", receipt)
    try:
        for shape in ("fixed", "mixed"):
            for threads in args.threads:
                paired = []
                for pair in range(args.pairs):
                    order = ("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")
                    observed = {}
                    for position, label in enumerate(order):
                        directory = output / f"{shape}-t{threads}-p{pair:02d}-{position}-{label}"
                        directory.mkdir()
                        target = directory / executable.name
                        shutil.copy2(executable, target)
                        require(sha(target) == receipt["executable_sha256"], "staged_executable_changed")
                        staged = {}
                        for path, digest in libraries[label]:
                            destination = directory / path.name
                            shutil.copy2(path, destination, follow_symlinks=True)
                            require(sha(destination) == digest, "runtime_changed_while_staging")
                            staged[destination.resolve()] = digest
                        env = dict(os.environ)
                        env["PATH"] = str(directory) + os.pathsep + env.get("PATH", "")
                        if sys.platform.startswith("linux"):
                            env["LD_LIBRARY_PATH"] = str(directory)
                            env.pop("LD_PRELOAD", None)
                            env.pop("LD_AUDIT", None)
                        command = [str(target), "--shape", shape, "--threads", str(threads),
                                   "--allocations", str(args.allocations), "--batch", str(args.batch),
                                   "--rounds", str(args.rounds), "--warmup", str(args.warmup)]
                        row = {"shape": shape, "threads": threads, "pair": pair, "position": position,
                               "variant": label, "state": "INCOMPLETE", "directory": directory.name}
                        receipt["runs"].append(row)
                        try:
                            process = subprocess.run(command, cwd=directory, env=env,
                                                     capture_output=True, timeout=args.timeout)
                            (directory / "stdout.json").write_bytes(process.stdout)
                            (directory / "stderr.txt").write_bytes(process.stderr)
                            row["exit_code"] = process.returncode
                            require(process.returncode == 0 and not process.stderr.strip(), "child_failed")
                            native = validate(json.loads(process.stdout.decode("utf-8")),
                                              shape, threads, args, staged)
                            row["state"] = "PASS_SCOPED"
                            observed[label] = native
                            row["stdout_sha256"] = sha(directory / "stdout.json")
                        except subprocess.TimeoutExpired as error:
                            (directory / "stdout.partial").write_bytes(error.stdout or b"")
                            (directory / "stderr.partial").write_bytes(error.stderr or b"")
                            row["state"] = "TIMEOUT_LOCAL_CHILD_TERMINATION_ATTEMPTED"
                            raise
                        finally:
                            save(output / "receipt.json", receipt)
                    baseline, candidate = observed["baseline"], observed["candidate"]
                    # Equal work and resource results are required; scheduling may change arena packing
                    # in the shared-domain mixed case, so report and compare its capacity explicitly.
                    fields = ("abi", "checksum", "completed_operations", "final_resources")
                    require(all(baseline[field] == candidate[field] for field in fields), "pair_semantics_changed")
                    paired.append({
                        "pair": pair, "order": list(order),
                        "allocation_p50_candidate_over_baseline":
                            candidate["allocation_batch_ns_per_op_p50"] / baseline["allocation_batch_ns_per_op_p50"],
                        "workflow_p50_candidate_over_baseline":
                            candidate["workflow_round_ns_per_op_p50"] / baseline["workflow_round_ns_per_op_p50"],
                        "baseline_peak_backing": [row["peak_backing_bytes"] for row in baseline["round_records"]],
                        "candidate_peak_backing": [row["peak_backing_bytes"] for row in candidate["round_records"]],
                    })
                receipt["summaries"].append({
                    "shape": shape, "threads": threads, "pairs": paired,
                    "median_paired_allocation_ratio": statistics.median(
                        row["allocation_p50_candidate_over_baseline"] for row in paired),
                    "median_paired_workflow_ratio": statistics.median(
                        row["workflow_p50_candidate_over_baseline"] for row in paired),
                    "interpretation": "DESCRIPTIVE_SYNTHETIC_RATIO_NO_CONFIDENCE_OR_PRODUCT_QUALIFICATION",
                })
        receipt["state"] = "PASS_SCOPED_SYNTHETIC_PAIRED"
    except Exception as error:
        receipt["failure"] = type(error).__name__ + ":" + str(error)
    finally:
        save(output / "receipt.json", receipt)
    print(json.dumps({"state": receipt["state"], "completed_processes":
                      sum(row["state"] == "PASS_SCOPED" for row in receipt["runs"]),
                      "performance_qualified": False}))
    return 0 if receipt["state"] == "PASS_SCOPED_SYNTHETIC_PAIRED" else 1


if __name__ == "__main__":
    sys.exit(main())

