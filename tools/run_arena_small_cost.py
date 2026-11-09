#!/usr/bin/env python3
"""Collect isolated prepared-Arena cost diagnostics, keeping every observation."""
from __future__ import annotations

import argparse
import itertools
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_arena_lookup_pairs import percentile, require, runtimes, save, sha

ROOT = Path(__file__).resolve().parents[1]


def request(shape, operation):
    return ((operation % 4 + 1) * 32, 8 << (operation % 4)) if shape == "mixed" else (64, 16)


def prepared_chunks(shape, batch):
    # Independent integer first-fit model, considering every possible cycle phase.
    counts = []
    for phase in range(4):
        positions = []
        for operation in range(phase, phase + batch):
            count, alignment = request(shape, operation)
            for i, used in enumerate(positions):
                offset = ((used + alignment - 1) // alignment) * alignment
                if offset + count <= 4096:
                    positions[i] = offset + count
                    break
            else:
                positions.append(count)
        counts.append(len(positions))
    return max(counts)


def expected_cycle(shape, batch, cycle):
    checksum = payload_bytes = 0
    for operation in range(cycle * batch, (cycle + 1) * batch):
        count, _ = request(shape, operation)
        first = operation * 31 % 256
        prefix = min(count, 256 - first)
        tail = count - prefix
        checksum += prefix * (2 * first + prefix - 1) // 2 + tail * (tail - 1) // 2
        payload_bytes += count
    return checksum, payload_bytes


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def validate(native, shape, batch, mode, executor, args, staged):
    chunks = prepared_chunks(shape, batch)
    operations = batch * args.cycles
    workload = "arena_small_cost_stream_v1" if mode == "stream" else "arena_small_cost_staged_v1"
    require(isinstance(native, dict) and native.get("schema_version") == 1 and
            native.get("workload") == workload and native.get("success") is True and
            native.get("performance_qualified") is False,
            "native_not_success")
    require(native.get("shape") == shape and native.get("batch") == batch and
            native.get("mode") == mode and native.get("executor") == executor and
            native.get("cycles_per_round") == args.cycles and native.get("rounds") == args.rounds and
            native.get("warmup_rounds") == args.warmup and native.get("prepared_chunks") == chunks and
            native.get("chunk_bytes") == 4096 and native.get("preparation_alignment") == 64 and
            native.get("preparation_reset") is True and integer(native.get("abi"), 1),
            "workload_identity_mismatch")
    loaded = Path(native.get("runtime_module", "")).resolve(strict=True)
    require(loaded in staged and sha(loaded) == staged[loaded], "loaded_runtime_not_selected_bytes")
    require(native.get("logical_peak_allocation_records") == batch and
            native.get("logical_peak_lease_records") == (1 if mode == "stream" else batch) and
            native.get("hot_clock_reads_per_cycle") == (3 if mode == "stream" else 6) and
            native.get("reset_clock_reads_per_cycle") == 2 and
            native.get("round_wall_clock_reads_per_round") == 2 and
            native.get("round_wall_scope") == "BEFORE_DOMAIN_CREATE_THROUGH_DOMAIN_DESTROY",
            "lifetime_or_instrumentation_identity_mismatch")
    metadata = {name: operations * args.rounds for name in
                ("allocation_insertions", "lease_insertions", "allocation_removals", "lease_removals")}
    require(native.get("semantic_metadata_operations") == metadata, "semantic_metadata_count_mismatch")
    expected = [expected_cycle(shape, batch, cycle) for cycle in range(args.cycles)]
    checksum_per_round = sum(x[0] for x in expected)
    bytes_per_round = sum(x[1] for x in expected)
    require(native.get("completed_operations") == operations * args.rounds and
            native.get("checksum") == checksum_per_round * args.rounds and
            native.get("payload_bytes") == bytes_per_round * args.rounds and
            native.get("final_resources") == "CLOSED_ZERO", "oracle_or_close_mismatch")
    phase_names = ["allocate_ns_per_op", "stream_remainder_ns_per_op"] if mode == "stream" else [
        "allocate_ns_per_op", "borrow_ns_per_op", "payload_ns_per_op",
        "owner_release_ns_per_op", "lease_release_ns_per_op"]
    values = {name: [] for name in phase_names}
    values.update({name: [] for name in ("hot_ns_per_op", "reset_ns_per_cycle", "worker_loop_ns_per_op",
                                       "outer_wall_ns_per_op", "round_wall_ns_per_op", "prepare_ns", "close_ns", "worker_create_ready_ns")})
    records = native.get("round_records")
    require(isinstance(records, list) and len(records) == args.rounds, "round_count_mismatch")
    total_hot = 0
    for row in records:
        require(isinstance(row, dict) and all(integer(row.get(name)) for name in
                ("round_wall_ns", "prepare_ns", "close_ns", "worker_loop_ns", "outer_wall_ns", "worker_create_ready_ns")) and
                row["worker_loop_ns"] > 0 and row["outer_wall_ns"] >= row["worker_loop_ns"] and
                row["round_wall_ns"] >= row["prepare_ns"] + row["outer_wall_ns"] +
                row["close_ns"] + row["worker_create_ready_ns"] and
                (executor != "inline" or row["worker_create_ready_ns"] == 0) and
                row.get("provider_allocation_count_before_timed") == chunks and
                row.get("provider_allocation_count") == chunks and row.get("peak_backing_bytes") == chunks * 4096 and
                row.get("allocation_count") == chunks + operations and
                row.get("completed_operations") == operations and row.get("checksum") == checksum_per_round and
                row.get("payload_bytes") == bytes_per_round,
                "round_resources_or_counts_mismatch")
        cycles = row.get("cycles")
        require(isinstance(cycles, list) and len(cycles) == args.cycles, "cycle_count_mismatch")
        for cycle_index, cycle in enumerate(cycles):
            require(isinstance(cycle, dict) and integer(cycle.get("reset_ns")) and
                    integer(cycle.get("hot_ns"), 1) and cycle.get("checksum") == expected[cycle_index][0] and
                    cycle.get("payload_bytes") == expected[cycle_index][1], "cycle_oracle_mismatch")
            phases = cycle.get("phases_ns")
            require(isinstance(phases, list) and len(phases) == len(phase_names) and
                    all(integer(x) for x in phases) and sum(phases) == cycle["hot_ns"],
                    "phase_sum_or_count_mismatch")
            values["hot_ns_per_op"].append(cycle["hot_ns"] / batch)
            values["reset_ns_per_cycle"].append(cycle["reset_ns"])
            total_hot += cycle["hot_ns"]
            for name, duration in zip(phase_names, phases):
                values[name].append(duration / batch)
        values["worker_loop_ns_per_op"].append(row["worker_loop_ns"] / operations)
        values["outer_wall_ns_per_op"].append(row["outer_wall_ns"] / operations)
        values["round_wall_ns_per_op"].append(row["round_wall_ns"] / operations)
        values["prepare_ns"].append(row["prepare_ns"])
        values["close_ns"].append(row["close_ns"])
        values["worker_create_ready_ns"].append(row["worker_create_ready_ns"])
    metrics = {}
    for name, samples in values.items():
        metrics[name + "_p50"] = percentile(samples, .5)
        metrics[name + "_p95"] = percentile(samples, .95)
    metrics["hot_operations_per_second"] = operations * args.rounds * 1e9 / total_hot
    reported = native.get("metrics")
    require(isinstance(reported, dict) and set(reported) == set(metrics) and
            all(type(reported.get(key)) in (float, int) and math.isfinite(reported[key]) and
                math.isclose(reported[key], value, rel_tol=1e-10, abs_tol=1e-6) for key, value in metrics.items()),
            "summary_does_not_match_raw_cycles")
    return native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--shapes", nargs="+", choices=("fixed", "mixed"), default=["fixed", "mixed"])
    parser.add_argument("--batches", nargs="+", type=int, default=[32, 64])
    parser.add_argument("--modes", nargs="+", choices=("stream", "stages"), default=["stream", "stages"])
    parser.add_argument("--executors", nargs="+", choices=("inline", "worker"), default=["inline", "worker"])
    parser.add_argument("--cycles", type=int, default=128)
    parser.add_argument("--rounds", type=int, default=8)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    for name in ("shapes", "batches", "modes", "executors"):
        collection = getattr(args, name)
        require(collection and len(set(collection)) == len(collection), "empty_or_duplicate_" + name)
    require(all(1 <= batch <= 128 for batch in args.batches) and 1 <= args.cycles <= 4096 and
            2 <= args.rounds <= 64 and 0 <= args.warmup <= 16 and 1 <= args.repeats <= 16 and
            1 <= args.timeout <= 600 and max(args.batches) * args.cycles * (args.rounds + args.warmup) <= 4194304,
            "invalid_workload_bounds")
    executable = args.exe.resolve(strict=True)
    require(executable.is_file(), "executable_missing")
    libraries = runtimes(args.runtime_dir)
    output = args.out_dir.resolve()
    output.relative_to((ROOT / "build").resolve())
    output.mkdir(parents=True, exist_ok=False)
    profiles = list(itertools.product(args.shapes, args.batches, args.modes, args.executors))
    receipt = {"schema_version": 1, "state": "INCOMPLETE", "performance_qualified": False,
               "scope": "PREPARED_ARENA_COST_DIAGNOSTIC_SEPARATE_LIFETIMES_NOT_PRODUCT",
               "executable_sha256": sha(executable),
               "source_sha256": {name: sha(ROOT / name) for name in
                                 ("bench/arena_small_cost.cpp", "tools/run_arena_small_cost.py",
                                  "tools/run_arena_lookup_pairs.py")},
               "runtime_files": [{"name": p.name, "sha256": digest} for p, digest in libraries],
               "configuration": {name: getattr(args, name) for name in
                                 ("shapes", "batches", "modes", "executors", "cycles", "rounds", "warmup", "repeats")},
               "profile_order": "FORWARD_ON_EVEN_REPETITIONS_REVERSE_ON_ODD_NO_RETRIES_OR_EXCLUSIONS",
               "runs": [], "failure": None}
    save(output / "receipt.json", receipt)
    try:
        for repetition in range(args.repeats):
            order = profiles if repetition % 2 == 0 else list(reversed(profiles))
            for shape, batch, mode, executor in order:
                name = f"r{repetition:02d}-{shape}-b{batch}-{mode}-{executor}"
                directory = output / name
                directory.mkdir()
                target = directory / executable.name
                shutil.copy2(executable, target)
                require(sha(target) == receipt["executable_sha256"], "staged_executable_changed")
                staged = {}
                for path, digest in libraries:
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
                command = [str(target), "--shape", shape, "--batch", str(batch), "--mode", mode,
                           "--executor", executor, "--cycles", str(args.cycles),
                           "--rounds", str(args.rounds), "--warmup", str(args.warmup)]
                row = {"repetition": repetition, "shape": shape, "batch": batch, "mode": mode,
                       "executor": executor, "state": "INCOMPLETE", "directory": directory.name}
                receipt["runs"].append(row)
                try:
                    process = subprocess.run(command, cwd=directory, env=env,
                                             capture_output=True, timeout=args.timeout)
                    (directory / "stdout.json").write_bytes(process.stdout)
                    (directory / "stderr.txt").write_bytes(process.stderr)
                    row["exit_code"] = process.returncode
                    require(process.returncode == 0 and not process.stderr.strip(), "child_failed")
                    native = validate(json.loads(process.stdout.decode("utf-8")), shape, batch, mode,
                                      executor, args, staged)
                    row.update(state="PASS_DIAGNOSTIC", stdout_sha256=sha(directory / "stdout.json"),
                               metrics=native["metrics"], prepared_chunks=native["prepared_chunks"],
                               logical_peak_lease_records=native["logical_peak_lease_records"])
                except subprocess.TimeoutExpired as error:
                    (directory / "stdout.partial").write_bytes(error.stdout or b"")
                    (directory / "stderr.partial").write_bytes(error.stderr or b"")
                    row["state"] = "TIMEOUT_LOCAL_CHILD_TERMINATION_ATTEMPTED"
                    raise
                finally:
                    save(output / "receipt.json", receipt)
        receipt["state"] = "PASS_SCOPED_COST_DIAGNOSTIC"
    except Exception as error:
        receipt["failure"] = type(error).__name__ + ":" + str(error)
    finally:
        save(output / "receipt.json", receipt)
    print(json.dumps({"state": receipt["state"], "completed_processes":
                      sum(row["state"] == "PASS_DIAGNOSTIC" for row in receipt["runs"]),
                      "performance_qualified": False}))
    return 0 if receipt["state"] == "PASS_SCOPED_COST_DIAGNOSTIC" else 1


if __name__ == "__main__":
    sys.exit(main())
