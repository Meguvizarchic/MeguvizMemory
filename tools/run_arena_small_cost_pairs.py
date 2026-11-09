#!/usr/bin/env python3
"""Compare selected runtimes with matched prepared-Arena lifetimes and AB/BA order."""
from __future__ import annotations

import argparse
import itertools
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

from run_arena_lookup_pairs import require, runtimes, save, sha
from run_arena_small_cost import validate

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--baseline-runtime-dir', type=Path, required=True)
    parser.add_argument('--candidate-runtime-dir', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--shapes', nargs='+', choices=('fixed', 'mixed'), default=['fixed', 'mixed'])
    parser.add_argument('--batches', nargs='+', type=int, default=[32, 64])
    parser.add_argument('--modes', nargs='+', choices=('stream', 'stages'), default=['stream', 'stages'])
    parser.add_argument('--executors', nargs='+', choices=('inline', 'worker'), default=['inline', 'worker'])
    parser.add_argument('--cycles', type=int, default=128)
    parser.add_argument('--rounds', type=int, default=8)
    parser.add_argument('--warmup', type=int, default=2)
    parser.add_argument('--pairs', type=int, default=8)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    for name in ('shapes', 'batches', 'modes', 'executors'):
        values = getattr(args, name)
        require(values and len(set(values)) == len(values), 'empty_or_duplicate_' + name)
    require(2 <= args.pairs <= 16 and args.pairs % 2 == 0 and
            all(1 <= batch <= 128 for batch in args.batches) and 1 <= args.cycles <= 4096 and
            2 <= args.rounds <= 64 and 0 <= args.warmup <= 16 and 1 <= args.timeout <= 600 and
            max(args.batches) * args.cycles * (args.rounds + args.warmup) <= 4194304,
            'invalid_workload_bounds')
    executable = args.exe.resolve(strict=True)
    require(executable.is_file(), 'executable_missing')
    libraries = {'baseline': runtimes(args.baseline_runtime_dir),
                 'candidate': runtimes(args.candidate_runtime_dir)}
    output = args.out_dir.resolve()
    output.relative_to((ROOT / 'build').resolve())
    output.mkdir(parents=True, exist_ok=False)
    receipt = {'schema_version': 1, 'state': 'INCOMPLETE', 'performance_qualified': False,
               'scope': 'MATCHED_PREPARED_ARENA_COST_PROFILES_NOT_PRODUCT',
               'executable_sha256': sha(executable),
               'source_sha256': {n: sha(ROOT / n) for n in (
                   'bench/arena_small_cost.cpp', 'tools/run_arena_small_cost.py',
                   'tools/run_arena_small_cost_pairs.py', 'tools/run_arena_lookup_pairs.py')},
               'configuration': {n: getattr(args, n) for n in (
                   'shapes', 'batches', 'modes', 'executors', 'cycles', 'rounds', 'warmup', 'pairs')},
               'runtime_files': {label: [{'name': p.name, 'sha256': digest} for p, digest in items]
                                 for label, items in libraries.items()},
               'runs': [], 'summaries': [], 'failure': None}
    save(output / 'receipt.json', receipt)
    try:
        for shape, batch, mode, executor in itertools.product(
                args.shapes, args.batches, args.modes, args.executors):
            pairs = []
            for pair in range(args.pairs):
                order = ('baseline', 'candidate') if pair % 2 == 0 else ('candidate', 'baseline')
                observed = {}
                for position, label in enumerate(order):
                    directory = output / f'{shape}-b{batch}-{mode}-{executor}-p{pair:02d}-{position}-{label}'
                    directory.mkdir()
                    target = directory / executable.name
                    shutil.copy2(executable, target)
                    require(sha(target) == receipt['executable_sha256'], 'staged_executable_changed')
                    staged = {}
                    for source, digest in libraries[label]:
                        destination = directory / source.name
                        shutil.copy2(source, destination, follow_symlinks=True)
                        require(sha(destination) == digest, 'runtime_changed_while_staging')
                        staged[destination.resolve()] = digest
                    env = dict(os.environ)
                    env['PATH'] = str(directory) + os.pathsep + env.get('PATH', '')
                    if sys.platform.startswith('linux'):
                        env['LD_LIBRARY_PATH'] = str(directory)
                        env.pop('LD_PRELOAD', None)
                        env.pop('LD_AUDIT', None)
                    command = [str(target), '--shape', shape, '--batch', str(batch), '--mode', mode,
                               '--executor', executor, '--cycles', str(args.cycles),
                               '--rounds', str(args.rounds), '--warmup', str(args.warmup)]
                    row = {'shape': shape, 'batch': batch, 'mode': mode, 'executor': executor,
                           'pair': pair, 'position': position, 'variant': label,
                           'directory': directory.name, 'state': 'INCOMPLETE'}
                    receipt['runs'].append(row)
                    try:
                        process = subprocess.run(command, cwd=directory, env=env,
                                                 capture_output=True, timeout=args.timeout)
                        (directory / 'stdout.json').write_bytes(process.stdout)
                        (directory / 'stderr.txt').write_bytes(process.stderr)
                        row['exit_code'] = process.returncode
                        require(process.returncode == 0 and not process.stderr.strip(), 'child_failed')
                        native = validate(json.loads(process.stdout.decode('utf-8')), shape, batch,
                                          mode, executor, args, staged)
                        row.update(state='PASS_SCOPED', stdout_sha256=sha(directory / 'stdout.json'))
                        observed[label] = native
                    except subprocess.TimeoutExpired as error:
                        (directory / 'stdout.partial').write_bytes(error.stdout or b'')
                        (directory / 'stderr.partial').write_bytes(error.stderr or b'')
                        row['state'] = 'TIMEOUT_LOCAL_CHILD_TERMINATION_ATTEMPTED'
                        raise
                    finally:
                        save(output / 'receipt.json', receipt)
                baseline, candidate = observed['baseline'], observed['candidate']
                fields = ('abi', 'checksum', 'payload_bytes', 'completed_operations', 'final_resources',
                          'semantic_metadata_operations', 'logical_peak_lease_records', 'prepared_chunks')
                require(all(baseline[n] == candidate[n] for n in fields), 'pair_semantics_changed')
                ratios, deltas = {}, {}
                require(baseline['metrics'].keys() == candidate['metrics'].keys(), 'metric_names_changed')
                for name in baseline['metrics']:
                    a, b = baseline['metrics'][name], candidate['metrics'][name]
                    ratios[name] = b / a if a else None
                    deltas[name] = b - a
                pairs.append({'pair': pair, 'order': list(order), 'ratios': ratios, 'deltas': deltas})
            names = pairs[0]['ratios']
            receipt['summaries'].append({'shape': shape, 'batch': batch, 'mode': mode,
                'executor': executor, 'pairs': pairs,
                'median_paired_ratios': {name: statistics.median(
                    [p['ratios'][name] for p in pairs if p['ratios'][name] is not None])
                    if any(p['ratios'][name] is not None for p in pairs) else None for name in names},
                'interpretation': 'DESCRIPTIVE_SEPARATE_LIFETIMES_NO_PRODUCT_QUALIFICATION'})
        receipt['state'] = 'PASS_SCOPED_SYNTHETIC_PAIRED'
    except Exception as error:
        receipt['failure'] = type(error).__name__ + ':' + str(error)
    finally:
        save(output / 'receipt.json', receipt)
    print(json.dumps({'state': receipt['state'], 'processes': len(receipt['runs']),
                      'performance_qualified': False}))
    return 0 if receipt['state'] == 'PASS_SCOPED_SYNTHETIC_PAIRED' else 1


if __name__ == '__main__':
    sys.exit(main())
