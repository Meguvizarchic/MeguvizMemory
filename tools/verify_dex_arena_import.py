"""Verify the separate Arena-import DEX consumer on the frozen host input; no ADB."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INPUT_HASH = '83ac99bc338e8bfc1980e382a4c318871cac0364dc449ae7b0316f601c136ab0'
INPUT_BYTES = 10540184
METHODS = 54147
CHECKSUM = 12701547145945416225
CASES = ('normal', 'cross-thread', 'soft-budget', 'workspace-budget', 'corrupt', 'borrow-budget')


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--baseline-exe', type=Path, required=True)
    parser.add_argument('--runtime-dir', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    require(args.input.stat().st_size == INPUT_BYTES and digest(args.input) == INPUT_HASH, 'input_identity_mismatch')
    lock_path = ROOT / 'SOURCE_LOCK.json'
    lock = json.loads(lock_path.read_text(encoding='utf-8'))
    sdk_files = [entry for entry in lock['files'] if entry['root'] == 'DexMeguviz']
    require(len(sdk_files) == 65 and all(digest(args.sdk / row['path']) == row['sha256'] for row in sdk_files),
            'sdk_identity_mismatch')
    runtime = args.runtime_dir / ('mgmem.dll' if os.name == 'nt' else 'libmgmem.so')
    require(runtime.is_file() and args.exe.is_file() and args.baseline_exe.is_file(), 'binary_missing')
    require(not (args.exe.parent / runtime.name).exists() and not (args.baseline_exe.parent / runtime.name).exists(),
            'executable_local_runtime_shadows_selected')
    env = dict(os.environ)
    search_path = 'PATH' if os.name == 'nt' else 'LD_LIBRARY_PATH'
    env[search_path] = str(args.runtime_dir.resolve()) + os.pathsep + env.get(search_path, '')

    def execute(exe, variant, case):
        proc = subprocess.run([str(exe.resolve()), str(args.input.resolve()), '--variant', variant, '--case', case],
                              env=env, capture_output=True, text=True, timeout=120)
        require(proc.returncode == 0 and not proc.stderr.strip(), 'consumer_failed_' + variant + '_' + case)
        result = json.loads(proc.stdout)
        require(result.get('state') == 'PASS' and result.get('success') is True and
                result.get('variant') == variant and result.get('case') == case and
                result.get('performance_qualification') == 'NOT_MEASURED' and
                result.get('cycles_completed') == 1 and len(result.get('iterations', [])) == 1,
                'consumer_receipt_invalid')
        row = result['iterations'][0]
        require(row['input_bytes'] == INPUT_BYTES and row['source_capacity_bytes'] >= INPUT_BYTES and
                row['source_plus_input_unique_capacity_bytes'] == row['source_capacity_bytes'] and
                row['extra_input_backing_bytes'] == row['copy_bytes'] == 0 and
                row['source_freed'] is True and row['final_resources'] == 'ZERO' and
                row['SDK_releases_including_caller'] == row['SDK_retains'] + 1 and
                row['origin_releases_including_caller'] == row['origin_retains'] + 1,
                'resource_contract_failed')
        if case in ('corrupt', 'borrow-budget'):
            require(row['source_sha256'] is None and row['method_count'] == row['result_checksum'] == 0,
                    'rejection_contract_failed')
        else:
            require(row['source_sha256'] == INPUT_HASH and row['method_count'] == METHODS and
                    row['result_checksum'] == CHECKSUM, 'query_oracle_mismatch')
        if variant == 'arena-import':
            require(row['memory_backing_peak_bytes'] == row['source_capacity_bytes'] + 8192 and
                    row['imported_backing_subset_bytes'] == row['source_capacity_bytes'] and
                    row['workspace_requested_bytes'] == 4096 and row['workspace_backing_bytes'] == 8192 and
                    row['provider_published_count'] == 1 and row['origin_retains'] == 1 and
                    row['memory_domain'] == 'CLOSED_ZERO', 'arena_accounting_mismatch')
            require((row['workspace_flushes'] == 0 if case in ('corrupt', 'borrow-budget')
                     else row['workspace_flushes'] > 0), 'workspace_use_mismatch')
            if case in ('cross-thread', 'soft-budget'):
                require(row['last_release_on_worker'] is True, 'final_release_thread_mismatch')
        else:
            require(row['memory_backing_peak_bytes'] == row['imported_backing_subset_bytes'] == 0 and
                    row['origin_retains'] == row['provider_published_count'] == 0 and row['memory_domain'] is None,
                    'baseline_accounting_mismatch')
        return {'variant': variant, 'case': case, 'source_bytes': row['source_capacity_bytes'],
                'methods': row['method_count'], 'checksum': row['result_checksum'],
                'memory_backing_peak_bytes': row['memory_backing_peak_bytes'],
                'imported_subset_bytes': row['imported_backing_subset_bytes'],
                'workspace_flushes': row.get('workspace_flushes'), 'final_resources': row['final_resources']}

    baseline = execute(args.baseline_exe, 'direct', 'normal')
    runs = [execute(args.exe, 'arena-import', case) for case in CASES]
    source_files = ('src/mgmem.cpp', 'include/mgmem/mgmem.h', 'consumers/dex/import_consumer.cpp',
                    'consumers/dex/CMakeLists.txt', 'tools/verify_dex_arena_import.py')
    result = {'state': 'PASS_HOST_FUNCTIONAL_ONLY', 'device_executed': False,
              'performance_qualification': 'NOT_MEASURED', 'bil_integrated': False,
              'input': {'sha256': INPUT_HASH, 'bytes': INPUT_BYTES},
              'sdk_source_lock_sha256': digest(lock_path), 'sdk_files_checked': len(sdk_files),
              'binary_sha256': digest(args.exe), 'baseline_binary_sha256': digest(args.baseline_exe),
              'runtime_sha256': digest(runtime),
              'source_identity': {name: digest(ROOT / name) for name in source_files},
              'baseline': baseline, 'runs': runs,
              'limitations': ['SDK parser allocations remain outside this workspace.',
                              'The workspace buffers consumer result-hash input; it is not a speedup measurement.',
                              'Runtime identity is the selected file hash, not independent loaded-image attestation.',
                              'Library paths and input are not published.']}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'state': result['state'], 'arena_cases': len(runs), 'baseline_cases': 1}))


if __name__ == '__main__':
    main()
