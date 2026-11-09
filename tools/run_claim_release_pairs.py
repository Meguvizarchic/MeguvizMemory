#!/usr/bin/env python3
"""Balanced host System-release pairs; release and full allocator workflow stay separate."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
from run_arena_lookup_pairs import percentile, require, runtimes, save, sha

ROOT = Path(__file__).resolve().parents[1]
COUNT, BYTES, BATCH, SEED = 2048, 64, 64, 0x9e3779b97f4a7c15
METRICS = ('release_batch_ns_per_op_p50', 'release_batch_ns_per_op_p95',
           'release_phase_ns_per_op_p50', 'release_phase_ns_per_op_p95',
           'workflow_round_ns_per_op_p50', 'workflow_round_ns_per_op_p95',
           'release_operations_per_second', 'workflow_operations_per_second')


def expected_checksum():
    result = 0
    for index in range(COUNT):
        first = (index * 31 + 7) % 256
        before_wrap = min(BYTES, 256 - first)
        result += before_wrap * (2 * first + before_wrap - 1) // 2
        tail = BYTES - before_wrap
        result += tail * (tail - 1) // 2
    return result


def order_hash(order):
    values = list(range(COUNT)); mask = (1 << 64) - 1
    if order == 'reverse': values.reverse()
    elif order == 'shuffled':
        state = SEED
        for size in range(COUNT, 1, -1):
            state ^= state >> 12; state ^= (state << 25) & mask; state ^= state >> 27
            index = ((state * 2685821657736338717) & mask) % size
            values[size - 1], values[index] = values[index], values[size - 1]
    result = 14695981039346656037
    for value in values:
        for _ in range(8):
            result = ((result ^ (value & 255)) * 1099511628211) & mask; value >>= 8
    return format(result, '016x')


def validate(n, domains, order, threads, args, staged):
    require(isinstance(n, dict) and n.get('schema_version') == 1 and
            n.get('workload') == 'system_claim_release_v1' and n.get('success') is True, 'native_not_success')
    expected = dict(count=COUNT, bytes_per_allocation=BYTES, alignment=16, batch=BATCH,
                    domains=domains, domain_budget_bytes=COUNT // domains * BYTES, threads=threads,
                    order=order, order_hash_fnv64=order_hash(order), shuffle_seed=format(SEED, '016x'),
                    thread_assignment='strided_permutation_positions', rounds=args.rounds, warmup_rounds=args.warmup)
    require(all(n.get(k) == v for k, v in expected.items()), 'workload_identity_changed')
    loaded = Path(n.get('runtime_module', '')).resolve(strict=True)
    require(loaded in staged and sha(loaded) == staged[loaded], 'loaded_runtime_not_selected_bytes')
    checksum = expected_checksum()
    require(n.get('released_operations') == COUNT * args.rounds and n.get('checksum') == checksum * args.rounds and
            n.get('prepared_resources') == 'N_OWNERS_NO_LEASES' and n.get('after_release_resources') == 'ZERO_BEFORE_CLOSE' and
            n.get('final_resources') == 'CLOSED_ZERO' and n.get('performance_qualified') is False, 'completion_or_resources')
    rows = n.get('round_records'); samples, release, workflow, total_release, total_workflow = [], [], [], 0, 0
    require(isinstance(rows, list) and len(rows) == args.rounds, 'round_count')
    for row in rows:
        for field in ('preparation_ns', 'release_phase_ns', 'workflow_ns'):
            require(type(row.get(field)) is int and row[field] > 0, 'invalid_round_time')
        require(type(row.get('close_ns')) is int and row['close_ns'] >= 0 and
                row['workflow_ns'] >= row['preparation_ns'] + row['release_phase_ns'] + row['close_ns'], 'workflow_omits_phase')
        require(row.get('released_operations') == COUNT and row.get('checksum') == checksum and
                row.get('prepared_allocations') == COUNT and row.get('prepared_leases') == 0 and
                row.get('prepared_backing_bytes') == COUNT * BYTES and row.get('peak_backing_bytes') == COUNT * BYTES and
                row.get('provider_allocation_count') == COUNT and
                row.get('domain_peak_backing_bytes') == [COUNT // domains * BYTES] * domains and
                row.get('final_resources') == 'CLOSED_ZERO' and
                all(row.get(k) == 0 for k in ('backing_after_release', 'owners_after_release', 'leases_after_release', 'reserved_after_release')),
                'round_resources_mismatch')
        values = row.get('release_batch_ns_per_op')
        require(isinstance(values, list) and len(values) == COUNT // BATCH and
                all(type(x) in (int, float) and math.isfinite(x) and x > 0 for x in values), 'invalid_release_samples')
        samples.extend(values); release.append(row['release_phase_ns'] / COUNT); workflow.append(row['workflow_ns'] / COUNT)
        total_release += row['release_phase_ns']; total_workflow += row['workflow_ns']
    recomputed = dict(zip(METRICS, (percentile(samples, .5), percentile(samples, .95),
        percentile(release, .5), percentile(release, .95), percentile(workflow, .5), percentile(workflow, .95),
        COUNT * args.rounds * 1e9 / total_release, COUNT * args.rounds * 1e9 / total_workflow)))
    require(all(type(n.get(k)) in (int, float) and math.isclose(n[k], v, rel_tol=1e-10, abs_tol=1e-6)
                for k, v in recomputed.items()), 'summary_not_raw_records')
    return n


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'baseline-runtime-dir', 'candidate-runtime-dir', 'out-dir'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--domains', type=int, nargs='+', default=[1, 32], choices=(1, 32))
    parser.add_argument('--orders', nargs='+', default=['forward', 'reverse', 'shuffled'], choices=('forward', 'reverse', 'shuffled'))
    parser.add_argument('--threads', type=int, nargs='+', default=[1, 4], choices=(1, 4))
    parser.add_argument('--pairs', type=int, default=4)
    parser.add_argument('--rounds', type=int, default=4)
    parser.add_argument('--warmup', type=int, default=1)
    parser.add_argument('--count', type=int, default=COUNT)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    require(args.count == COUNT and 2 <= args.pairs <= 32 and args.pairs % 2 == 0 and
            2 <= args.rounds <= 64 and 0 <= args.warmup <= 16 and 5 <= args.timeout <= 300, 'invalid_frozen_workload_bounds')
    require(all(values and len(values) == len(set(values)) for values in (args.domains, args.orders, args.threads)), 'duplicate_matrix_values')
    exe = args.exe.resolve(strict=True); output = args.out_dir.resolve()
    require(output.is_relative_to((ROOT / 'build').resolve()) and not output.exists(), 'fresh_private_build_output_required')
    libraries = {'baseline': runtimes(args.baseline_runtime_dir), 'candidate': runtimes(args.candidate_runtime_dir)}
    output.mkdir(parents=True)
    receipt = dict(schema_version=1, state='INCOMPLETE', performance_qualified=False,
        scope='SYSTEM_RELEASE_AND_FULL_WORKFLOW_HOST_ONLY', executable_sha256=sha(exe),
        source_sha256={name: sha(ROOT / name) for name in ('bench/claim_release.cpp', 'tools/run_claim_release_pairs.py', 'tools/run_arena_lookup_pairs.py')},
        configuration=dict(count=COUNT, domains=args.domains, orders=args.orders, threads=args.threads,
                           pairs=args.pairs, rounds=args.rounds, warmup=args.warmup, batch=BATCH),
        runtime_files={label: [{'name': p.name, 'sha256': h} for p, h in files] for label, files in libraries.items()},
        runs=[], summaries=[], failure=None)
    save(output / 'receipt.json', receipt)
    try:
        for domains in args.domains:
            for order in args.orders:
                for threads in args.threads:
                    pairs = []
                    for pair in range(args.pairs):
                        observed = {}; variants = ('baseline', 'candidate') if pair % 2 == 0 else ('candidate', 'baseline')
                        for position, label in enumerate(variants):
                            directory = output / f'd{domains}-{order}-t{threads}-p{pair:02d}-{position}-{label}'; directory.mkdir()
                            target = directory / exe.name; shutil.copy2(exe, target); require(sha(target) == receipt['executable_sha256'], 'exe_changed')
                            staged = {}
                            for library, identity in libraries[label]:
                                destination = directory / library.name; shutil.copy2(library, destination, follow_symlinks=True)
                                require(sha(destination) == identity, 'runtime_changed_while_staging'); staged[destination.resolve()] = identity
                            env = dict(os.environ); env['PATH'] = str(directory) + os.pathsep + env.get('PATH', '')
                            if sys.platform.startswith('linux'):
                                env['LD_LIBRARY_PATH'] = str(directory); env.pop('LD_PRELOAD', None); env.pop('LD_AUDIT', None)
                            command = [str(target), '--count', str(COUNT), '--domains', str(domains), '--order', order,
                                       '--threads', str(threads), '--rounds', str(args.rounds), '--warmup', str(args.warmup)]
                            row = dict(domains=domains, order=order, threads=threads, pair=pair, position=position, variant=label,
                                       state='INCOMPLETE', directory=directory.name); receipt['runs'].append(row)
                            try:
                                child = subprocess.run(command, cwd=directory, env=env, capture_output=True, timeout=args.timeout)
                                (directory / 'stdout.json').write_bytes(child.stdout); (directory / 'stderr.txt').write_bytes(child.stderr)
                                row['exit_code'] = child.returncode; require(child.returncode == 0 and not child.stderr.strip(), 'child_failed')
                                observed[label] = validate(json.loads(child.stdout.decode('utf-8')), domains, order, threads, args, staged)
                                row.update(state='PASS_SCOPED', stdout_sha256=sha(directory / 'stdout.json'))
                            except subprocess.TimeoutExpired as error:
                                (directory / 'stdout.partial').write_bytes(error.stdout or b''); (directory / 'stderr.partial').write_bytes(error.stderr or b'')
                                row['state'] = 'TIMEOUT_LOCAL_CHILD_TERMINATION_ATTEMPTED'; raise
                            finally: save(output / 'receipt.json', receipt)
                        a, b = observed['baseline'], observed['candidate']
                        require(all(a[k] == b[k] for k in ('abi', 'checksum', 'released_operations', 'final_resources', 'order_hash_fnv64')), 'pair_semantics_changed')
                        pairs.append(dict(pair=pair, order=list(variants), ratios={k: b[k] / a[k] for k in METRICS},
                                          baseline_peak=[x['peak_backing_bytes'] for x in a['round_records']],
                                          candidate_peak=[x['peak_backing_bytes'] for x in b['round_records']]))
                    receipt['summaries'].append(dict(domains=domains, order=order, threads=threads, pairs=pairs,
                        median_paired_ratios={k: statistics.median(x['ratios'][k] for x in pairs) for k in METRICS},
                        interpretation='Latency ratios below one and throughput ratios above one are favorable; descriptive only.'))
        receipt['state'] = 'PASS_SCOPED_SYNTHETIC_PAIRED'
    except Exception as error: receipt['failure'] = type(error).__name__ + ':' + str(error)
    finally: save(output / 'receipt.json', receipt)
    print(json.dumps(dict(state=receipt['state'], completed_processes=sum(x['state'] == 'PASS_SCOPED' for x in receipt['runs']), performance_qualified=False)))
    return 0 if receipt['state'] == 'PASS_SCOPED_SYNTHETIC_PAIRED' else 1

if __name__ == '__main__': sys.exit(main())
