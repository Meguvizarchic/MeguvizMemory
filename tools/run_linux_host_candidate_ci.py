#!/usr/bin/env python3
"""Build one source-locked Linux candidate in an isolated GitHub-hosted job.

Only the checked tarball and a path-free receipt are placed in export/. Raw
logs, the source snapshot, the CI-only scan marker and build files stay local.
This is an engineering candidate, not an Android or performance qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import secrets
import shutil
import stat
import subprocess
import sys
import zipfile

import build_android_candidate as common
import build_host_candidate as host

ROOT = Path(__file__).resolve().parents[1]
MAX_SOURCE_FILE = 32 * 1024 * 1024
MAX_SOURCE_TOTAL = 256 * 1024 * 1024
MAX_CANDIDATE = 256 * 1024 * 1024
MAX_LOG = 64 * 1024 * 1024
MAX_STRIP_STDERR = 64 * 1024
MAX_STRIP_JOURNAL = 1024 * 1024
MAX_STRIP_ARCHIVE = 64 * 1024 * 1024
LLVM_TOOLS = ('llvm-strip', 'llvm-readobj', 'llvm-nm', 'llvm-ar', 'llvm-readelf')
EXPORT_ARCHIVE = 'MeguvizMemory-Linux-Host-Candidate.tar.gz'
EXPORT_RECEIPT = 'receipt.json'

# These are source-controlled diagnostic categories, not values supplied by a
# child process. In particular, never echo an arbitrary alphanumeric "reason".
HOST_FAILURE_CODES = frozenset('''
coff_archive_members coff_relocations_missing coff_section_flags_missing coff_section_metadata_missing
coff_section_name_missing coff_section_raw_pointer_missing coff_section_size_mismatch coff_sections_missing
coff_static_export_invalid duplicate_host_member elf_nobits_has_payload elf_static_allocated_sections_missing
elf_static_section_metadata_missing elf_static_section_size_mismatch forbidden_host_archive_member
generator_root_mismatch host_archive_roundtrip_mismatch host_cmake_package_dir_mismatch
host_compiler_identity_missing host_compiler_missing host_consumer_result_mismatch host_consumer_source_changed
host_core_declarations_missing host_core_declarations_unparseable host_core_jni_compiler_mismatch
host_core_soname_mismatch host_dynamic_binary_changed host_elf_search_path_forbidden host_final_archive_mismatch
host_generated_jni_headers_missing host_generator_changed_during_build host_java_class_missing
host_jni_core_dependency host_linux_core_links_missing host_loaded_identity_mismatch
host_pe_executable_sections_missing host_pe_export_invalid host_pe_export_missing host_pe_export_not_executable
host_pe_forwarded_or_empty_export host_pe_test_export host_pressure_declarations_missing
host_pressure_declarations_unparseable host_pressure_export_missing host_probe_path_invalid
host_published_identity_mismatch host_readback_missing host_sdk_changed_during_validation
host_sdk_header_mismatch host_sdk_headers_missing host_sdk_member_missing host_sdk_unexpected_member
host_source_changed_during_build host_static_changed_during_validation host_static_contract_changed
host_static_debug_remains host_static_input_changed host_symlink_cycle host_symlink_dangling host_symlink_path
host_symlink_target host_tar_member_missing host_tar_member_short host_tar_too_large host_tool_missing
host_toolchain_changed host_unexpected_dynamic_dependency host_windows_symlink_unsupported invalid_host_pe
invalid_host_pe_executable_section invalid_host_pe_export_directory invalid_host_tar_member invalid_jobs
invalid_private_markers jdk_jni_headers_missing pressure_consumer_imported_path_mismatch
pressure_consumer_unexpected_stderr pressure_test_main_return_unsupported pressure_test_main_unexpected
pressure_v1_result_mismatch pressure_v2_result_mismatch private_markers_outside_build required_host_tool_missing
source_or_lock_missing unsupported_host_architecture unsupported_host_os work_overlaps_source
wrong_host_consumer_abi wrong_host_elf_abi wrong_host_pe_abi wrong_host_static_abi
archive_roundtrip_mismatch atomic_noreplace_unsupported duplicate_source_path generator_source_mismatch
generator_changed built_binary_missing class_jar_mismatch class_native_mismatch core_unexpected_dependency
debug_section_remains elf_bad_load elf_bad_name elf_bad_name_offset elf_bad_names elf_bad_tables
elf_changed_during_validation elf_dynamic_or_relocations_changed elf_load_changed elf_load_missing
elf_not_16k_load_aligned elf_wrong_abi invalid_elf java_sources_invalid jni_core_dependency
jni_generated_header_missing jni_target_consumer_dependency_mismatch jni_target_export_format_mismatch
jni_target_imported_path_mismatch jni_unexpected_dependency keep_rule_missing llvm_notice_missing
published_identity_mismatch required_class_missing sdk_header_mismatch sdk_notice_mismatch sdk_static_mismatch
sdk_symlink sdk_unexpected_or_missing_member static_changed_during_validation static_debug_section_remains
stripped_section_allocated stripped_section_loaded toolchain_changed toolchain_file_missing
invalid_needed invalid_source_path invalid_tool_step java_native_declarations jni_export_missing
jni_symbol_table_unparseable pressure_binding_missing required_source_missing snapshot_changed
snapshot_set_changed source_changed_during_build source_changed_during_copy source_file_missing
source_lock_count source_lock_hash source_lock_mismatch source_lock_scope source_lock_unknown_root
source_set_mismatch source_symlink static_allocated_sections_missing static_archive_members
static_contract_changed static_nobits_has_payload static_relocations_missing static_section_size
test_jni_export test_native_export test_static_export tool_journal_already_started
tool_journal_duplicate_or_invalid_step tool_journal_empty tool_journal_exists tool_journal_identity_changed
tool_journal_missing tool_journal_not_private_regular_file tool_journal_open_failed
tool_journal_record_mismatch tool_log_dir_invalid tool_log_set_mismatch tool_work_dir_invalid
unresolved_pressure_symbol unsupported_jni_name_escape work_dir_not_fresh_build_child workspace_directory_invalid
ValueError OSError FileNotFoundError PermissionError TimeoutExpired UnicodeDecodeError BadZipFile RuntimeError TypeError
'''.split())
HOST_STEPS = frozenset({
    'compile-managed-java', 'core-configure', 'core-build', 'core-install', 'jni-configure', 'jni-build', 'jni-install',
    'pressure-strip', 'pressure-clean-debug-sections', 'pressure-clean-public-exports',
    'pressure-consumer-configure', 'pressure-consumer-build', 'pressure-v1-dynamic', 'pressure-v1-run',
    'pressure-v2-dynamic', 'pressure-v2-run', 'consumer-plain-configure', 'consumer-plain-build',
    'consumer-probe-configure', 'consumer-probe-build', 'consumer-reference-configure',
    'consumer-reference-build', 'consumer-verify',
} | {prefix + suffix for prefix in ('core', 'jni') for suffix in
     ('-symbols', '-dynamic', '-relocations', '-search-path')} |
    {prefix + suffix for prefix in ('pressure-original', 'pressure-clean') for suffix in
     ('-members', '-header', '-payload-completeness', '-sections', '-relocations', '-symbols')})
RUNNER_FAILURE_CODES = frozenset('''
build_dist_different_filesystems builder_result_mismatch candidate_already_exists candidate_binary_mismatch
candidate_consumer_identity_mismatch candidate_consumer_mismatch candidate_consumer_receipt_mismatch
candidate_core_link_mismatch candidate_elf_abi_mismatch candidate_header_mismatch candidate_member_set_mismatch
candidate_metadata_missing candidate_notice_mismatch candidate_scope_mismatch candidate_source_lock_mismatch
candidate_source_manifest_mismatch candidate_toolchain_mismatch checkout_commit_mismatch checkout_not_clean
checkout_root_mismatch ci_work_already_exists complete_llvm_tool_directory_missing duplicate_archive_name
duplicate_json_key duplicate_source_name export_readback_mismatch git_command_failed github_hosted_linux_required
github_workspace_mismatch input_not_regular_file input_size_limit invalid_commit_sha invalid_json_constant
invalid_source_name jdk_or_jni_headers_missing journal_duplicate_step journal_empty journal_link_count
journal_log_hash_mismatch journal_log_set_mismatch journal_required_step_missing journal_step_failed
linux_x86_64_required logs_directory_invalid pressure_consumer_output_mismatch runner_script_commit_mismatch
selected_tool_changed selected_tool_file_missing snapshot_changed snapshot_missing snapshot_path_escape
snapshot_set_changed snapshot_symlink source_archive_blob_mismatch source_archive_failed source_archive_set_mismatch
source_archive_size_limit source_archive_size_mismatch source_archive_symlink source_file_size_limit
source_git_batch_failed source_git_blob_missing source_git_blob_truncated source_git_size_invalid
source_git_size_mismatch source_lock_changed source_lock_digest_invalid source_lock_duplicate source_lock_invalid
source_lock_scope source_lock_count source_lock_hash source_lock_unknown_root source_lock_mismatch source_set_mismatch
generator_source_mismatch source_tree_incomplete unexpected_external_source unsupported_git_tree_entry workflow_commit_mismatch
workspace_directory_outside_root workspace_directory_symlink
'''.split()) | frozenset('required_tool_missing_' + name for name in ('cmake', 'ninja', 'gcc', 'git')) | frozenset(
    'required_tool_not_executable_' + name for name in ('cmake', 'ninja', 'gcc', 'git'))


def safe_builder_code(reason):
    if isinstance(reason, str):
        if reason in HOST_FAILURE_CODES:
            return reason
        for prefix in ('tool_failed_', 'tool_timeout_', 'tool_launch_error_', 'duplicate_tool_step_',
                       'tool_log_exists_', 'tool_log_hash_mismatch_'):
            if reason.startswith(prefix) and reason[len(prefix):] in HOST_STEPS:
                return reason
    return 'UNKNOWN_BUILDER_REASON'


def safe_builder_reason(stderr):
    """Return only a fixed known category from a bounded builder JSON error."""
    if not isinstance(stderr, bytes) or len(stderr) > 4096:
        return 'UNKNOWN_BUILDER_REASON'
    try:
        item = strict_json(stderr)
        if isinstance(item, dict) and item.get('state') in ('FAILED', 'PREFLIGHT_FAILED'):
            return safe_builder_code(item.get('reason'))
    except (UnicodeError, ValueError, TypeError, json.JSONDecodeError):
        pass
    return 'UNKNOWN_BUILDER_REASON'


class ChildStageFailure(ValueError):
    def __init__(self, stage, outcome, reason, exit_code=None):
        super().__init__('child_stage_failed')
        require(stage in ('source_lock', 'host_builder') and
                outcome in ('EXITED_NONZERO', 'TIMEOUT', 'LAUNCH_ERROR', 'LOG_WRITE_FAILED'), 'invalid_child_stage')
        if outcome == 'EXITED_NONZERO':
            reason = safe_builder_code(reason) if stage == 'host_builder' else 'SOURCE_LOCK_FAILED'
        else:
            reason = {'TIMEOUT': 'CHILD_TIMEOUT', 'LAUNCH_ERROR': 'CHILD_LAUNCH_FAILED',
                      'LOG_WRITE_FAILED': 'PRIVATE_LOG_WRITE_FAILED'}[outcome]
        self.stage, self.outcome, self.reason = stage, outcome, reason
        self.exit_code = exit_code if type(exit_code) is int and -255 <= exit_code <= 255 else None
        self.pressure_strip_detail = None

    def public_record(self):
        result = {'state': 'FAILED_NO_CI_ARTIFACT_UPLOAD', 'stage': self.stage, 'outcome': self.outcome,
                  'reason': self.reason, 'exit_code': self.exit_code}
        if (self.stage == 'host_builder' and self.outcome == 'EXITED_NONZERO' and
                self.reason == 'tool_failed_pressure-strip' and self.pressure_strip_detail is not None):
            result['pressure_strip'] = safe_pressure_detail(self.pressure_strip_detail)
        return result


def public_failure(error):
    if isinstance(error, ChildStageFailure):
        return error.public_record()
    reason = str(error)
    if reason == 'required_tool_missing_g++':
        reason = 'required_tool_missing_cpp'
    elif reason == 'required_tool_not_executable_g++':
        reason = 'required_tool_not_executable_cpp'
    elif reason not in RUNNER_FAILURE_CODES:
        reason = 'UNKNOWN_RUNNER_REASON'
    return {'state': 'FAILED_NO_CI_ARTIFACT_UPLOAD', 'stage': 'runner', 'reason': reason}


def require(value, reason):
    if not value:
        raise ValueError(reason)


def valid_commit(value):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{40}', value) is not None,
            'invalid_commit_sha')
    return value


def verify_ci_identity(commit, environment, root):
    require(environment.get('GITHUB_ACTIONS') == 'true' and environment.get('RUNNER_OS') == 'Linux' and
            environment.get('RUNNER_ARCH') == 'X64' and environment.get('RUNNER_ENVIRONMENT') == 'github-hosted' and
            environment.get('GITHUB_EVENT_NAME') == 'workflow_dispatch', 'github_hosted_linux_required')
    require(environment.get('GITHUB_SHA') == commit, 'workflow_commit_mismatch')
    value = environment.get('GITHUB_WORKSPACE')
    require(value and Path(value).resolve(strict=True) == root, 'github_workspace_mismatch')


def safe_name(value):
    require(isinstance(value, str) and value and '\\' not in value and
            not any(ord(c) < 32 or ord(c) == 127 for c in value), 'invalid_source_name')
    path = PurePosixPath(value)
    require(not path.is_absolute() and '..' not in path.parts and '.' not in path.parts and
            path.as_posix() == value and not re.match(r'^[A-Za-z]:', value), 'invalid_source_name')
    return path


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def strict_json(data):
    def pairs(values):
        result = {}
        for key, value in values:
            require(key not in result, 'duplicate_json_key')
            result[key] = value
        return result
    def invalid_constant(_value):
        raise ValueError('invalid_json_constant')
    return json.loads(data.decode('utf-8'), object_pairs_hook=pairs, parse_constant=invalid_constant)


def read_regular(path, maximum):
    require(path.is_file() and not path.is_symlink(), 'input_not_regular_file')
    with path.open('rb') as stream:
        data = stream.read(maximum + 1)
        require(len(data) <= maximum and stream.read(1) == b'', 'input_size_limit')
    return data


STRIP_ERROR_PATTERNS = {
    'FORMAT_UNRECOGNIZED': re.compile(rb'\b(?:file format not recognized|unrecognized file format|not recognized as a valid object file|invalid file type)\b', re.I),
    'UNSUPPORTED_FORMAT_OR_ARCH': re.compile(rb'\b(?:unsupported archive format|unsupported object file|unsupported architecture|unsupported relocation)\b', re.I),
    'INVALID_OR_TRUNCATED_ARCHIVE': re.compile(rb'\b(?:malformed archive|truncated archive|invalid archive|invalid bitcode signature)\b', re.I),
    'UNKNOWN_OPTION': re.compile(rb'\b(?:unknown argument|unknown option|unrecognized option)\b', re.I),
    'MISSING_FILE': re.compile(rb'\b(?:no such file or directory|cannot open input file)\b', re.I),
    'PERMISSION_DENIED': re.compile(rb'\bpermission denied\b', re.I),
    'NO_SPACE': re.compile(rb'\bno space left on device\b', re.I),
}
STRIP_CATEGORIES = frozenset(STRIP_ERROR_PATTERNS) | {'UNKNOWN'}
DIAGNOSTIC_FILE_STATES = frozenset({'OK', 'MISSING', 'OVERSIZE', 'UNTRUSTED', 'UNAVAILABLE'})


def classify_strip_stderr(data):
    """Match fixed tool-error patterns only; the text itself never leaves here."""
    if not isinstance(data, bytes) or len(data) > MAX_STRIP_STDERR or b'\0' in data:
        return 'UNKNOWN'
    matches = set()
    for line in data.splitlines():
        if len(line) > 2048:
            return 'UNKNOWN'
        marker = re.search(rb'(?:^|[/\\])(?:llvm-strip|llvm-objcopy)(?:-[0-9]+)?(?:\.exe)?:\s*(?:fatal\s+)?error:\s*',
                           line, re.I)
        if marker:
            text = line[marker.end():]
            matches.update(name for name, pattern in STRIP_ERROR_PATTERNS.items() if pattern.search(text))
    return next(iter(matches)) if len(matches) == 1 else 'UNKNOWN'


def read_private_diagnostic(work, parts, maximum):
    """Read a fixed private path, rejecting links, changes and oversized data."""
    try:
        current = Path(work)
        if current.is_symlink() or not current.is_dir():
            return 'UNTRUSTED', None
        for part in parts[:-1]:
            current = current / part
            if not os.path.lexists(current):
                return 'MISSING', None
            if current.is_symlink() or not current.is_dir():
                return 'UNTRUSTED', None
        path = current / parts[-1]
        if not os.path.lexists(path):
            return 'MISSING', None
        before = path.lstat()
        if path.is_symlink() or not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            return 'UNTRUSTED', None
        if not path.resolve(strict=True).is_relative_to(Path(work).resolve(strict=True)):
            return 'UNTRUSTED', None
        if before.st_size < 0 or before.st_size > maximum:
            return 'OVERSIZE', None
        flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
        fd = os.open(path, flags)
        with os.fdopen(fd, 'rb') as stream:
            opened = os.fstat(stream.fileno())
            if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 or
                    (opened.st_dev, opened.st_ino, opened.st_size) != (before.st_dev, before.st_ino, before.st_size)):
                return 'UNTRUSTED', None
            data = stream.read(maximum + 1)
            after = os.fstat(stream.fileno())
        current_stat = path.lstat()
        if (len(data) != before.st_size or len(data) > maximum or
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) !=
                (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) or
                (current_stat.st_dev, current_stat.st_ino, current_stat.st_size, current_stat.st_mtime_ns) !=
                (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)):
            return 'UNTRUSTED', None
        return 'OK', data
    except (OSError, ValueError):
        return 'UNAVAILABLE', None


def safe_pressure_detail(value):
    """Copy only bounded, fixed-schema fields into a public failure record."""
    value = value if isinstance(value, dict) else {}
    def enum(name, allowed):
        item = value.get(name)
        return item if isinstance(item, str) and item in allowed else 'UNKNOWN'
    def number(name, maximum):
        item = value.get(name)
        return item if type(item) is int and 0 <= item <= maximum else None
    def digest(name):
        item = value.get(name)
        return item if isinstance(item, str) and re.fullmatch(r'[0-9a-f]{64}', item) else None
    code = value.get('tool_exit_code')
    if type(code) is not int or code == 0 or not -255 <= code <= 255:
        code = None
    stderr_verified = (value.get('stderr_status') == 'OK' and number('stderr_bytes', MAX_STRIP_STDERR) is not None and
                       digest('stderr_sha256') is not None)
    verified = (value.get('journal_status') == 'VERIFIED' and stderr_verified and
                value.get('tool_outcome') == 'EXITED_NONZERO' and code is not None)
    archive_verified = (value.get('archive_status') == 'OK' and
                        number('archive_bytes', MAX_STRIP_ARCHIVE) is not None and digest('archive_sha256') is not None)
    return {
        'journal_status': ('VERIFIED' if verified else
                           enum('journal_status', {'MISSING', 'OVERSIZE', 'UNTRUSTED', 'UNAVAILABLE', 'INVALID'})),
        'stderr_status': enum('stderr_status', DIAGNOSTIC_FILE_STATES),
        'stderr_bytes': number('stderr_bytes', MAX_STRIP_STDERR) if stderr_verified else None,
        'stderr_sha256': digest('stderr_sha256') if stderr_verified else None,
        'tool_outcome': enum('tool_outcome', {'EXITED_NONZERO', 'TIMEOUT', 'LAUNCH_ERROR'}) if verified else 'UNKNOWN',
        'tool_exit_code': code if verified else None,
        'category': enum('category', STRIP_CATEGORIES) if verified else 'UNKNOWN',
        'classification_scope': 'FIXED_STDERR_PATTERNS_ONLY_NOT_ROOT_CAUSE',
        'archive_status': enum('archive_status', DIAGNOSTIC_FILE_STATES),
        'archive_format': enum('archive_format', {'AR', 'THIN_AR', 'OTHER'}) if archive_verified else 'UNKNOWN',
        'archive_bytes': number('archive_bytes', MAX_STRIP_ARCHIVE) if archive_verified else None,
        'archive_sha256': digest('archive_sha256') if archive_verified else None,
        'archive_scope': 'POST_FAILURE_REQUESTED_FILE_NOT_EXECUTION_ATTESTATION',
        'llvm_identity': enum('llvm_identity', {'MATCH_PREBUILD', 'CHANGED', 'UNAVAILABLE'}),
        'llvm_sha256': digest('llvm_sha256'),
        'llvm_scope': 'REQUESTED_FILE_HASH_NOT_EXECUTED_IMAGE_ATTESTATION',
    }


def collect_pressure_strip_detail(work, tools, tool_identities):
    """Inspect only fixed private files after this specific builder failure."""
    detail = {'journal_status': 'UNKNOWN', 'stderr_status': 'UNKNOWN', 'category': 'UNKNOWN',
              'archive_status': 'UNKNOWN', 'archive_format': 'UNKNOWN', 'llvm_identity': 'UNKNOWN'}
    status_state, status_data = read_private_diagnostic(work, ('STATUS.json',), 16384)
    journal_state, journal_data = read_private_diagnostic(work, ('operations.jsonl',), MAX_STRIP_JOURNAL)
    stderr_state, stderr_data = read_private_diagnostic(work, ('logs', 'pressure-strip.stderr'), MAX_STRIP_STDERR)
    detail['stderr_status'] = stderr_state
    if stderr_state == 'OK':
        detail['stderr_bytes'] = len(stderr_data)
        detail['stderr_sha256'] = digest_bytes(stderr_data)
    detail['journal_status'] = journal_state
    if status_state == journal_state == stderr_state == 'OK':
        try:
            status = strict_json(status_data)
            rows = [strict_json(line) for line in journal_data.splitlines()]
            labels = [row.get('step') for row in rows if isinstance(row, dict)]
            matches = [row for row in rows if isinstance(row, dict) and row.get('step') == 'pressure-strip']
            if (isinstance(status, dict) and status.get('state') == 'FAILED' and
                    status.get('reason') == 'tool_failed_pressure-strip' and status.get('output_written') is False and
                    len(labels) == len(rows) and all(isinstance(label, str) for label in labels) and
                    len({label.casefold() for label in labels}) == len(labels) and
                    len(matches) == 1 and matches[0].get('state') == 'EXITED' and
                    type(matches[0].get('exit_code')) is int and matches[0]['exit_code'] != 0 and
                    -255 <= matches[0]['exit_code'] <= 255 and
                    matches[0].get('stderr_sha256') == digest_bytes(stderr_data)):
                detail['journal_status'] = 'VERIFIED'
                detail['tool_outcome'] = 'EXITED_NONZERO'
                detail['tool_exit_code'] = matches[0]['exit_code']
                detail['category'] = classify_strip_stderr(stderr_data)
            else:
                detail['journal_status'] = 'INVALID'
        except (ValueError, TypeError, UnicodeError, json.JSONDecodeError):
            detail['journal_status'] = 'INVALID'
    archive_state, archive_data = read_private_diagnostic(
        work, ('raw-sdk', 'lib', 'libmgmem_pressure.a'), MAX_STRIP_ARCHIVE)
    detail['archive_status'] = archive_state
    if archive_state == 'OK':
        detail['archive_bytes'] = len(archive_data)
        detail['archive_sha256'] = digest_bytes(archive_data)
        detail['archive_format'] = ('AR' if archive_data.startswith(b'!<arch>\n') else
                                    'THIN_AR' if archive_data.startswith(b'!<thin>\n') else 'OTHER')
    try:
        before = tool_identities.get('llvm-strip')
        tool = (tools['llvm'] / 'llvm-strip').resolve(strict=True)
        if (isinstance(before, dict) and isinstance(before.get('sha256'), str) and
                re.fullmatch(r'[0-9a-f]{64}', before['sha256'])):
            detail['llvm_identity'] = 'MATCH_PREBUILD' if common.file_identity(tool) == before else 'CHANGED'
            detail['llvm_sha256'] = before['sha256']
        else:
            detail['llvm_identity'] = 'UNAVAILABLE'
    except (OSError, ValueError, KeyError, TypeError):
        detail['llvm_identity'] = 'UNAVAILABLE'
    return safe_pressure_detail(detail)


def attach_pressure_strip_detail(error, work, tools, tool_identities):
    if (isinstance(error, ChildStageFailure) and error.stage == 'host_builder' and
            error.outcome == 'EXITED_NONZERO' and error.reason == 'tool_failed_pressure-strip'):
        # Diagnostics are best effort: malformed private evidence or memory
        # errors must not replace the already confirmed child-stage failure.
        try:
            error.pressure_strip_detail = collect_pressure_strip_detail(work, tools, tool_identities)
        except Exception:
            error.pressure_strip_detail = safe_pressure_detail({})


def write_new(path, data):
    with path.open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def workspace_directory(root, name):
    path = root / name
    require(not path.is_symlink(), 'workspace_directory_symlink')
    path.mkdir(exist_ok=True)
    require(path.is_dir() and path.resolve().parent == root, 'workspace_directory_outside_root')
    return path.resolve()


def git(root, *args, timeout=60):
    command = ['git', '-c', 'safe.directory=' + root.as_posix(), '-c', 'core.fsmonitor=false',
               '-C', str(root), *args]
    result = subprocess.run(command, capture_output=True, timeout=timeout, check=False)
    require(result.returncode == 0, 'git_command_failed')
    return result.stdout


def verify_checkout(root, commit):
    valid_commit(commit)
    head = git(root, 'rev-parse', '--verify', 'HEAD').decode('ascii').strip()
    require(head == commit, 'checkout_commit_mismatch')
    top = Path(git(root, 'rev-parse', '--show-toplevel').decode('utf-8').strip()).resolve()
    require(top == root, 'checkout_root_mismatch')
    require(not git(root, 'status', '--porcelain', '-z', '--untracked-files=normal'), 'checkout_not_clean')
    return head


def git_tree(root, commit):
    raw = git(root, 'ls-tree', '-r', '-l', '-z', '--full-tree', commit)
    files = {}
    folded = set()
    total = 0
    for row in raw.split(b'\0'):
        if not row:
            continue
        metadata, name_bytes = row.split(b'\t', 1)
        mode, kind, oid, size_text = metadata.decode('ascii').split()
        require(kind == 'blob' and mode in ('100644', '100755') and
                re.fullmatch(r'[0-9a-f]{40}', oid) is not None, 'unsupported_git_tree_entry')
        require(size_text.isdecimal(), 'source_git_size_invalid')
        size = int(size_text)
        require(size <= MAX_SOURCE_FILE, 'source_file_size_limit')
        total += size
        require(total <= MAX_SOURCE_TOTAL, 'source_archive_size_limit')
        name = name_bytes.decode('utf-8')
        safe_name(name)
        require(name not in files and name.casefold() not in folded, 'duplicate_source_name')
        folded.add(name.casefold())
        files[name] = {'mode': mode, 'oid': oid, 'bytes': size}
    require(files and 'CMakeLists.txt' in files and 'tools/run_linux_host_candidate_ci.py' in files,
            'source_tree_incomplete')
    return files


def compare_archive_names(infos, expected):
    files = {}
    folded = set()
    total = 0
    for info in infos:
        name = info.filename[:-1] if info.is_dir() and info.filename.endswith('/') else info.filename
        safe_name(name)
        if info.is_dir():
            continue
        mode = (info.external_attr >> 16) & 0xffff
        require(not stat.S_ISLNK(mode), 'source_archive_symlink')
        require(name not in files and name.casefold() not in folded, 'duplicate_archive_name')
        require(0 <= info.file_size <= MAX_SOURCE_FILE, 'source_file_size_limit')
        if name in expected:
            require(info.file_size == expected[name]['bytes'], 'source_archive_size_mismatch')
        total += info.file_size
        require(total <= MAX_SOURCE_TOTAL, 'source_archive_size_limit')
        folded.add(name.casefold())
        files[name] = info
    require(set(files) == set(expected), 'source_archive_set_mismatch')
    return files


def export_snapshot(root, commit, work):
    expected = git_tree(root, commit)
    archive = work / 'source.zip'
    with archive.open('xb') as output:
        command = ['git', '-c', 'safe.directory=' + root.as_posix(), '-c', 'core.fsmonitor=false',
                   '-C', str(root), 'archive', '--format=zip', commit]
        result = subprocess.run(command, stdout=output, stderr=subprocess.PIPE, timeout=120, check=False)
        require(result.returncode == 0, 'source_archive_failed')
        output.flush(); os.fsync(output.fileno())
    snapshot = work / 'source'
    snapshot.mkdir()
    batch = subprocess.Popen(['git', '-c', 'safe.directory=' + root.as_posix(), '-c', 'core.fsmonitor=false',
                              '-C', str(root), 'cat-file', '--batch'], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    records = {}
    try:
        with zipfile.ZipFile(archive) as zipped:
            entries = compare_archive_names(zipped.infolist(), expected)
            for name in sorted(expected):
                info = entries[name]
                batch.stdin.write((commit + ':' + name + '\n').encode('utf-8')); batch.stdin.flush()
                header = batch.stdout.readline(200).decode('ascii').strip().split(' ')
                require(len(header) == 3 and header[0] == expected[name]['oid'] and header[1] == 'blob',
                        'source_git_blob_missing')
                size = int(header[2])
                require(0 <= size <= MAX_SOURCE_FILE and size == info.file_size == expected[name]['bytes'],
                        'source_git_size_mismatch')
                original = batch.stdout.read(size)
                require(len(original) == size and batch.stdout.read(1) == b'\n', 'source_git_blob_truncated')
                data = zipped.read(info)
                require(data == original, 'source_archive_blob_mismatch')
                target = snapshot.joinpath(*safe_name(name).parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                require(target.resolve().is_relative_to(snapshot.resolve()), 'snapshot_path_escape')
                write_new(target, data)
                os.chmod(target, 0o755 if expected[name]['mode'] == '100755' else 0o644)
                records[name] = {'bytes': size, 'sha256': digest_bytes(data), 'mode': expected[name]['mode']}
        batch.stdin.close()
        require(batch.wait(timeout=30) == 0, 'source_git_batch_failed')
    finally:
        if batch.poll() is None:
            batch.kill(); batch.wait()
        # A rejected archive must not leave the batch process or its pipes
        # holding an otherwise isolated workspace open.
        if batch.stdin is not None and not batch.stdin.closed:
            try:
                batch.stdin.close()
            except OSError:
                pass
        if batch.stdout is not None:
            batch.stdout.close()
    verify_snapshot(snapshot, records)
    return archive, snapshot, records


def verify_snapshot(snapshot, records):
    require(snapshot.is_dir() and not snapshot.is_symlink(), 'snapshot_missing')
    actual = set()
    for path in snapshot.rglob('*'):
        require(not path.is_symlink(), 'snapshot_symlink')
        if path.is_file():
            name = path.relative_to(snapshot).as_posix(); safe_name(name); actual.add(name)
    require(actual == set(records), 'snapshot_set_changed')
    for name, expected in records.items():
        value = read_regular(snapshot.joinpath(*safe_name(name).parts), MAX_SOURCE_FILE)
        require(len(value) == expected['bytes'] and digest_bytes(value) == expected['sha256'], 'snapshot_changed')


def discover_tools(environment):
    def on_path(name):
        value = shutil.which(name, path=environment.get('PATH'))
        require(value is not None, 'required_tool_missing_' + name)
        path = Path(value).resolve(strict=True)
        require(path.is_file() and os.access(path, os.X_OK), 'required_tool_not_executable_' + name)
        return path
    tools = {name: on_path(name) for name in ('cmake', 'ninja', 'gcc', 'g++', 'git')}
    java_homes = [environment.get('JAVA_HOME_17_X64'), environment.get('JAVA_HOME')]
    javac = shutil.which('javac', path=environment.get('PATH'))
    if javac:
        java_homes.append(str(Path(javac).resolve().parent.parent))
    jdk = None
    for candidate in java_homes:
        if not candidate:
            continue
        home = Path(candidate).resolve()
        required = [home / 'bin/java', home / 'bin/javac', home / 'include/jni.h', home / 'include/linux/jni_md.h']
        if all(path.is_file() for path in required) and all(os.access(path, os.X_OK) for path in required[:2]):
            jdk = home; break
    require(jdk is not None, 'jdk_or_jni_headers_missing')
    candidates = list(Path('/usr/lib').glob('llvm-*/bin')) + [Path('/usr/bin')]
    candidates.sort(key=lambda p: tuple(int(x) for x in re.findall(r'\d+', p.parent.name)) or (0,), reverse=True)
    llvm = next((p.resolve() for p in candidates if p.is_dir() and
                 all((p / name).is_file() and os.access(p / name, os.X_OK) for name in LLVM_TOOLS)), None)
    require(llvm is not None, 'complete_llvm_tool_directory_missing')
    tools['jdk'], tools['llvm'] = jdk, llvm
    return tools


def selected_tool_files(tools):
    files = {name: tools[name] for name in ('cmake', 'ninja', 'gcc', 'g++', 'git')}
    files['python'] = Path(sys.executable).resolve(strict=True)
    files['java'] = (tools['jdk'] / 'bin/java').resolve(strict=True)
    files['javac'] = (tools['jdk'] / 'bin/javac').resolve(strict=True)
    for name in LLVM_TOOLS:
        files[name] = (tools['llvm'] / name).resolve(strict=True)
    require(all(path.is_file() for path in files.values()), 'selected_tool_file_missing')
    return files


def verify_tool_identities(files, before, report):
    after = {name: common.file_identity(path) for name, path in files.items()}
    require(after == before, 'selected_tool_changed')
    host_names = ('cmake', 'ninja', 'java', 'javac', *LLVM_TOOLS)
    require(all(report.get('toolchain', {}).get(name) == before[name] for name in host_names) and
            report.get('compiler') == before['g++'], 'candidate_toolchain_mismatch')


def child_environment(tools, original):
    # Do not forward GitHub tokens, LD_PRELOAD, compiler/linker flags, Python or
    # Java injection variables. Preserve HOME only as supplied by the runner.
    dirs = [tools['jdk'] / 'bin', tools['llvm'], tools['cmake'].parent, tools['ninja'].parent,
            tools['gcc'].parent, tools['g++'].parent, Path('/usr/bin'), Path('/bin')]
    env = {'PATH': os.pathsep.join(dict.fromkeys(str(p) for p in dirs)), 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'JAVA_HOME': str(tools['jdk']), 'CC': str(tools['gcc']), 'CXX': str(tools['g++'])}
    if original.get('HOME'):
        env['HOME'] = original['HOME']
    if original.get('TMPDIR'):
        env['TMPDIR'] = original['TMPDIR']
    return env


def execute(command, environment, stdout_path, stderr_path, timeout, stage):
    require(stage in ('source_lock', 'host_builder'), 'invalid_child_stage')
    try:
        # Reserve both private logs before starting a child. A collision cannot
        # run a step or overwrite another file, including a dangling symlink.
        with stdout_path.open('xb') as stdout_file, stderr_path.open('xb') as stderr_file:
            try:
                result = subprocess.run([str(x) for x in command], env=environment, capture_output=True,
                                        timeout=timeout, check=False)
                stdout, stderr, outcome, exit_code = result.stdout, result.stderr, 'EXITED', result.returncode
            except subprocess.TimeoutExpired as error:
                stdout, stderr, outcome, exit_code = error.stdout or b'', error.stderr or b'', 'TIMEOUT', None
            except OSError:
                stdout, stderr, outcome, exit_code = b'', b'', 'LAUNCH_ERROR', None
            # These bytes remain private. Public diagnostics use fixed codes.
            stdout_file.write(stdout); stdout_file.flush(); os.fsync(stdout_file.fileno())
            stderr_file.write(stderr); stderr_file.flush(); os.fsync(stderr_file.fileno())
    except OSError as error:
        raise ChildStageFailure(stage, 'LOG_WRITE_FAILED', 'PRIVATE_LOG_WRITE_FAILED') from error
    if outcome == 'TIMEOUT':
        raise ChildStageFailure(stage, outcome, 'CHILD_TIMEOUT')
    if outcome == 'LAUNCH_ERROR':
        raise ChildStageFailure(stage, outcome, 'CHILD_LAUNCH_FAILED')
    if exit_code != 0:
        reason = safe_builder_reason(stderr) if stage == 'host_builder' else 'SOURCE_LOCK_FAILED'
        raise ChildStageFailure(stage, 'EXITED_NONZERO', reason, exit_code)
    return stdout


def validate_source_lock(lock_bytes, source):
    lock = strict_json(lock_bytes)
    entries = lock.get('files')
    require(lock.get('schema_version') == 1 and lock.get('scope') == 'SCOPED_SOURCE_TREE_LOCK' and
            isinstance(entries, list) and lock.get('file_count') == len(entries), 'source_lock_invalid')
    require(digest_bytes(common.canonical(entries)) == lock.get('source_set_sha256'), 'source_lock_digest_invalid')
    require(all(item.get('root') == 'MeguvizMemory' for item in entries), 'unexpected_external_source')
    expected = {}
    for item in entries:
        name = item.get('path'); safe_name(name)
        require(name not in expected, 'source_lock_duplicate')
        expected[name] = {'bytes': item.get('bytes'), 'sha256': item.get('sha256')}
    require(source.get('local_source_count') == len(expected) and source.get('local_source_records') == expected and
            source.get('source_lock_sha256') == digest_bytes(lock_bytes) and
            source.get('local_source_set_sha256') == digest_bytes(common.canonical(expected)),
            'candidate_source_lock_mismatch')
    return expected


def validate_journal(work):
    journal = work / 'operations.jsonl'
    raw = read_regular(journal, MAX_LOG)
    require(journal.stat().st_nlink == 1, 'journal_link_count')
    rows = [strict_json(line) for line in raw.splitlines()]
    require(rows, 'journal_empty')
    labels = [row.get('step') for row in rows]
    require(all(isinstance(x, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,160}', x) for x in labels) and
            len({x.casefold() for x in labels}) == len(labels), 'journal_duplicate_step')
    required = {'compile-managed-java', 'core-configure', 'core-build', 'core-install', 'jni-configure',
                'jni-build', 'jni-install', 'pressure-strip', 'pressure-consumer-configure',
                'pressure-consumer-build', 'pressure-v1-run', 'pressure-v2-run', 'consumer-probe-configure',
                'consumer-probe-build', 'consumer-reference-configure', 'consumer-reference-build', 'consumer-verify'}
    require(required <= set(labels), 'journal_required_step_missing')
    logs = work / 'logs'
    require(logs.is_dir() and not logs.is_symlink(), 'logs_directory_invalid')
    expected_files = {label + '.' + suffix for label in labels for suffix in ('stdout', 'stderr')}
    require({item.name for item in logs.iterdir()} == expected_files, 'journal_log_set_mismatch')
    for row in rows:
        require(row.get('state') == 'EXITED' and type(row.get('exit_code')) is int and row['exit_code'] == 0,
                'journal_step_failed')
        for suffix in ('stdout', 'stderr'):
            path = logs / (row['step'] + '.' + suffix)
            require(digest_bytes(read_regular(path, MAX_LOG)) == row.get(suffix + '_sha256'), 'journal_log_hash_mismatch')
    return {'steps': len(rows), 'streams': len(rows) * 2, 'sha256': digest_bytes(raw)}


def validate_consumer_receipts(work, consumer):
    logs = work / 'logs'
    v1 = strict_json(read_regular(logs / 'pressure-v1-run.stdout', MAX_LOG))
    v2 = read_regular(logs / 'pressure-v2-run.stdout', MAX_LOG).decode('utf-8')
    require(v1.get('status') == 'PASS' and type(v1.get('tests')) is int and v1['tests'] == 3 and
            re.fullmatch(r'managed-pressure-v2 PASS \(\d+ checks\)\s*', v2) is not None,
            'pressure_consumer_output_mismatch')
    path = work / 'consumer-source/build/installed-consumer-verification/receipt.json'
    raw = read_regular(path, MAX_LOG)
    require(digest_bytes(raw) == consumer.get('receipt_sha256'), 'candidate_consumer_receipt_mismatch')
    actual = strict_json(raw)
    require(actual.get('state') == 'PASS_HOST_FUNCTIONAL_DEVELOPMENT' and
            len(actual.get('cases', [])) == 12 and len(actual.get('pressure_cases', [])) == 12 and
            len(actual.get('negative_cases', [])) == 10 and actual.get('pressure_retry_contract') == 'PASS_JVM_ADMISSION_ONLY' and
            actual.get('plain_without_sidecar') == 'PASS', 'candidate_consumer_receipt_mismatch')
    identities = actual.get('identities', {})
    require(identities.get('core_sha256') == consumer.get('core_sha256') and
            identities.get('jni_sha256') == consumer.get('jni_sha256') and
            identities.get('java_jar_sha256') == consumer.get('jar_sha256'), 'candidate_consumer_identity_mismatch')


def validate_candidate(data, markers, lock_bytes, work):
    files, links = host.scan_host_archive(data, markers, False)
    require('SOURCE.json' in files and 'MANIFEST.json' in files, 'candidate_metadata_missing')
    source = strict_json(files['SOURCE.json'])
    report = strict_json(files['MANIFEST.json'])
    require(report.get('source') == source, 'candidate_source_manifest_mismatch')
    records = validate_source_lock(lock_bytes, source)
    require(report.get('schema_version') == 1 and report.get('state') == 'SCOPED_HOST_ENGINEERING_CANDIDATE' and
            report.get('scope') == 'SOURCE_BUILD_SANITIZED_INSTALLED_HOST_CONSUMER_ONLY' and
            report.get('platform') == 'linux-x86_64' and report.get('android_runtime') == 'NOT_RUN' and
            report.get('device_execution') == 'NOT_RUN' and report.get('performance') == 'NOT_MEASURED' and
            report.get('formal_release') == 'NOT_QUALIFIED' and
            report.get('tool_logs') == 'UNIQUE_EXCLUSIVE_AND_HASH_VERIFIED_IN_PRIVATE_WORK_DIRECTORY',
            'candidate_scope_mismatch')
    expected_headers = {name.removeprefix('include/mgmem/'): value for name, value in records.items()
                        if name.startswith('include/mgmem/')}
    actual_headers = {name.removeprefix('sdk/include/mgmem/'): {'bytes': len(value), 'sha256': digest_bytes(value)}
                      for name, value in files.items() if name.startswith('sdk/include/mgmem/')}
    require(expected_headers and actual_headers == expected_headers, 'candidate_header_mismatch')
    core_name, jni_name = 'sdk/lib/libmgmem.so.1.2.1', 'sdk/lib/libmgmem_pressure_jni.so'
    pressure_name, jar_name = 'sdk/lib/libmgmem_pressure.a', 'java/managed-api.jar'
    cmake_names = {'sdk/lib/cmake/MeguvizMemory/' + name for name in (
        'MeguvizMemoryConfig.cmake', 'MeguvizMemoryConfigVersion.cmake',
        'MeguvizMemoryTargets.cmake', 'MeguvizMemoryTargets-release.cmake')}
    expected_files = {'README.md', 'NOTICE-LLVM.txt', 'SOURCE.json', 'MANIFEST.json',
                      core_name, jni_name, pressure_name, jar_name} | cmake_names | {
                          'sdk/include/mgmem/' + name for name in expected_headers}
    require(set(files) == expected_files and set(links) == {'sdk/lib/libmgmem.so', 'sdk/lib/libmgmem.so.1'},
            'candidate_member_set_mismatch')
    require(records.get('third_party_notices/llvm-NOTICE.txt') == common.ident(files['NOTICE-LLVM.txt']),
            'candidate_notice_mismatch')
    # CMake may link the unversioned name directly or through the SONAME.
    # Both are safe only when each chain ends at this archive's one core.
    for start in ('sdk/lib/libmgmem.so', 'sdk/lib/libmgmem.so.1'):
        current = start; visited = set()
        while current in links:
            require(current not in visited, 'candidate_core_link_mismatch')
            visited.add(current)
            current = str(PurePosixPath(current).parent / links[current])
        require(current == core_name, 'candidate_core_link_mismatch')
    for key, name in (('core', core_name), ('jni', jni_name)):
        require(report.get(key, {}).get('identity') == common.ident(files[name]), 'candidate_binary_mismatch')
    require(report.get('managed_jar') == common.ident(files[jar_name]) and
            report.get('static_pressure', {}).get('candidate') == common.ident(files[pressure_name]),
            'candidate_binary_mismatch')
    consumer = report.get('consumer', {}); pressure = consumer.get('packaged_pressure', {})
    require(consumer.get('state') == 'PASS_HOST_INSTALLED_JVM_JNI_IMAGE' and
            consumer.get('default_cases') == 12 and consumer.get('synthetic_pressure_cases') == 12 and
            consumer.get('negative_cases') == 10 and pressure.get('state') == 'PASS_HOST_PACKAGED_STATIC_PRESSURE_V1_V2' and
            pressure.get('core_identity') == 'EACH_PROCESS_OS_MODULE_PATH' and
            pressure.get('required_checked_external_capability') == 'PASS', 'candidate_consumer_mismatch')
    require(consumer.get('core_sha256') == digest_bytes(files[core_name]) and
            consumer.get('jni_sha256') == digest_bytes(files[jni_name]) and
            consumer.get('jar_sha256') == digest_bytes(files[jar_name]) and
            pressure.get('core_sha256') == digest_bytes(files[core_name]) and
            pressure.get('pressure_sha256') == digest_bytes(files[pressure_name]), 'candidate_consumer_identity_mismatch')
    for name in (core_name, jni_name):
        elf = files[name]
        require(len(elf) >= 20 and elf[:6] == b'\x7fELF\x02\x01' and
                int.from_bytes(elf[16:18], 'little') == 3 and int.from_bytes(elf[18:20], 'little') == 62,
                'candidate_elf_abi_mismatch')
    journal = validate_journal(work)
    validate_consumer_receipts(work, consumer)
    return source, report, journal


def write_export(work, payload, receipt_bytes, markers):
    # A failed scan never creates the export directory used by the upload step.
    common.Scanner(markers).bytes(receipt_bytes, 'ci-receipt')
    export = work / 'export'; export.mkdir()
    write_new(export / EXPORT_ARCHIVE, payload)
    write_new(export / EXPORT_RECEIPT, receipt_bytes)
    require({p.name for p in export.iterdir()} == {EXPORT_ARCHIVE, EXPORT_RECEIPT} and
            digest_bytes(read_regular(export / EXPORT_ARCHIVE, MAX_CANDIDATE)) == digest_bytes(payload) and
            read_regular(export / EXPORT_RECEIPT, MAX_LOG) == receipt_bytes, 'export_readback_mismatch')


def run(commit):
    valid_commit(commit)
    require(sys.platform.startswith('linux') and platform.machine().lower() in ('x86_64', 'amd64'),
            'linux_x86_64_required')
    root = ROOT.resolve(strict=True)
    verify_ci_identity(commit, os.environ, root)
    verify_checkout(root, commit)
    require(digest_bytes(read_regular(root / 'tools/run_linux_host_candidate_ci.py', MAX_SOURCE_FILE)) ==
            digest_bytes(git(root, 'show', commit + ':tools/run_linux_host_candidate_ci.py')),
            'runner_script_commit_mismatch')
    tools = discover_tools(os.environ)
    tool_files = selected_tool_files(tools)
    tool_identities = {name: common.file_identity(path) for name, path in tool_files.items()}
    environment = child_environment(tools, os.environ)
    build = workspace_directory(root, 'build'); dist = workspace_directory(root, 'dist')
    require(build.stat().st_dev == dist.stat().st_dev, 'build_dist_different_filesystems')
    work = build / 'linux-host-candidate-ci'
    require(not os.path.lexists(work), 'ci_work_already_exists')
    work.mkdir()
    archive, snapshot, snapshot_records = export_snapshot(root, commit, work)
    lock = work / 'source-lock.json'
    execute([sys.executable, root / 'tools/source_lock.py', '--root', snapshot, '--output', lock],
            environment, work / 'source-lock.stdout', work / 'source-lock.stderr', 120, 'source_lock')
    lock_bytes = read_regular(lock, MAX_LOG)
    locked_bytes, source_records = common.verify_lock(snapshot, lock)
    require(locked_bytes == lock_bytes, 'source_lock_changed')
    generators = {name: root / ('tools/' + name + '.py') for name in (
        'run_linux_host_candidate_ci', 'build_host_candidate', 'build_android_candidate',
        'source_lock', 'package_feature_preview')}
    common.verify_generator_bindings(source_records, generators)
    marker = work / 'ci-only-marker.txt'
    marker_value = 'ci-only-sentinel-' + secrets.token_hex(16)
    write_new(marker, (marker_value + '\n').encode('ascii'))
    candidate = dist / ('MeguvizMemory-Linux-Candidate-' + commit[:12] + '.tar.gz')
    require(not os.path.lexists(candidate), 'candidate_already_exists')
    builder_work = work / 'builder'
    try:
        stdout = execute([sys.executable, root / 'tools/build_host_candidate.py', '--source-root', snapshot,
                          '--source-lock', lock, '--jdk', tools['jdk'], '--cmake', tools['cmake'],
                          '--ninja', tools['ninja'], '--llvm-bin', tools['llvm'], '--private-identifiers-file', marker,
                          '--work-dir', builder_work, '--output', candidate, '--jobs', '2'], environment,
                         work / 'builder.stdout', work / 'builder.stderr', 3000, 'host_builder')
    except ChildStageFailure as error:
        attach_pressure_strip_detail(error, builder_work, tools, tool_identities)
        raise
    outcome = strict_json(stdout)
    status = strict_json(read_regular(builder_work / 'STATUS.json', MAX_LOG))
    payload = read_regular(candidate, MAX_CANDIDATE)
    require(outcome.get('state') == status.get('state') == 'PASS_SCOPED_HOST_ENGINEERING_CANDIDATE' and
            outcome.get('platform') == 'linux-x86_64' and outcome.get('android_runtime') == 'NOT_RUN' and
            outcome.get('bytes') == len(payload) and outcome.get('sha256') == status.get('sha256') == digest_bytes(payload) and
            status.get('output_written') is True, 'builder_result_mismatch')
    source, report, journal = validate_candidate(payload, [marker_value], lock_bytes, builder_work)
    verify_tool_identities(tool_files, tool_identities, report)
    verify_snapshot(snapshot, snapshot_records)
    require(read_regular(lock, MAX_LOG) == lock_bytes, 'source_lock_changed')
    final_lock, final_records = common.verify_lock(snapshot, lock)
    require(final_lock == lock_bytes and final_records == source_records, 'source_lock_changed')
    common.verify_generator_bindings(source_records, generators)
    verify_checkout(root, commit)
    require(source['source_lock_sha256'] == digest_bytes(lock_bytes), 'source_lock_changed')
    consumer = report['consumer']
    packaged_pressure = consumer['packaged_pressure']
    # Copy only the reviewed fields. Never publish arbitrary content from a
    # nested report, even when a private workspace report has extra fields.
    consumer_summary = {
        'state': consumer['state'], 'default_cases': consumer['default_cases'],
        'synthetic_pressure_cases': consumer['synthetic_pressure_cases'], 'negative_cases': consumer['negative_cases'],
        'core_sha256': consumer['core_sha256'], 'jni_sha256': consumer['jni_sha256'],
        'jar_sha256': consumer['jar_sha256'], 'receipt_sha256': consumer['receipt_sha256'],
        'packaged_pressure': {
            'state': packaged_pressure['state'], 'core_identity': packaged_pressure['core_identity'],
            'required_checked_external_capability': packaged_pressure['required_checked_external_capability'],
            'core_sha256': packaged_pressure['core_sha256'], 'pressure_sha256': packaged_pressure['pressure_sha256']}}
    receipt = {'schema_version': 1, 'state': 'PASS_SCOPED_LINUX_HOST_ENGINEERING_CANDIDATE',
               'source_commit': commit, 'source_archive_sha256': digest_bytes(read_regular(archive, MAX_SOURCE_TOTAL)),
               'git_tree_record_count': len(snapshot_records),
               'git_tree_records_sha256': digest_bytes(common.canonical(snapshot_records)),
               'snapshot_verification': 'ALL_TRACKED_FILE_NAMES_AND_BYTES_PRE_AND_POST',
               'source_lock_sha256': digest_bytes(lock_bytes), 'source_record_count': source['local_source_count'],
               'source_set_sha256': source['local_source_set_sha256'],
               'candidate': {'filename': EXPORT_ARCHIVE, 'bytes': len(payload), 'sha256': digest_bytes(payload)},
               'platform': 'linux-x86_64', 'toolchain_sha256_and_sizes': tool_identities, 'consumer': consumer_summary,
               'journal': journal, 'ci_marker_scope': 'GITHUB_HOSTED_CI_ONLY_NOT_LOCAL_PRIVATE_IDENTIFIER_LIST',
               'scan': 'BOUNDED_RECURSIVE_ARCHIVE_PATH_AND_CI_SENTINEL',
               'device_execution': 'NOT_RUN', 'android_runtime': 'NOT_RUN', 'performance': 'NOT_MEASURED',
               'formal_release': 'NOT_QUALIFIED',
               'limits': ['The marker is a CI-only sentinel, not the user local private identifier list.',
                          'No raw logs, source snapshot, user input or environment is published.',
                          'Host module path and disk identity do not prove mapped bytes or signed provenance.',
                          'No Android ART, R8 runtime, device, energy or performance qualification.']}
    receipt_bytes = common.json_bytes(receipt)
    write_export(work, payload, receipt_bytes, [marker_value])
    print(json.dumps({'state': receipt['state'], 'source_commit': commit,
                      'candidate_sha256': receipt['candidate']['sha256'], 'journal_steps': journal['steps']}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--commit', required=True, help='Exact lowercase 40-character commit')
    args = parser.parse_args()
    try:
        run(args.commit)
        return 0
    except Exception as error:
        print(json.dumps(public_failure(error)), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
