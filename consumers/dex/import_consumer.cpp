#include <mgmem/mgmem.h>
#include <dexmeguviz/dexmeguviz.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint64_t input_limit = 32ull * 1024 * 1024;
constexpr uint64_t parser_limit = 256ull * 1024 * 1024;
#ifdef MGMEM_DEX_ARENA_IMPORT
constexpr uint64_t workspace_bytes = 4096;
constexpr uint64_t workspace_chunk = 8192;
#endif
std::atomic<uint64_t> next_identity{1};
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void memory_check(mgmem_status status, const char *name) {
    if (status != MGMEM_OK) throw std::runtime_error(std::string(name) + "_mgmem_" + std::to_string(status));
}
void dex_check(dmg_status status, const char *name) {
    if (status != DMG_OK) throw std::runtime_error(std::string(name) + "_dex_" + std::to_string(status));
}
std::string escaped(const std::string &value) {
    std::string result;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') result += '\\';
        result += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    return result;
}
struct OriginStats {
    std::atomic<uint32_t> retains{0}, releases{0};
    std::atomic<bool> freed{false}, freed_on_other_thread{false};
    const std::thread::id creator = std::this_thread::get_id();
};
// The intrusive owner, not the stack bridge, owns the immutable vector. The
// final callback really destroys it, while OriginStats survives for assertions.
struct Origin {
    std::vector<uint8_t> bytes;
    OriginStats &stats;
    const uint64_t identity = next_identity.fetch_add(1);
    std::atomic<uint32_t> references{1};
    explicit Origin(OriginStats &value) : stats(value) {}
    static mgmem_status retain(void *context) noexcept {
        auto &self = *static_cast<Origin *>(context);
        self.references.fetch_add(1, std::memory_order_relaxed);
        self.stats.retains.fetch_add(1, std::memory_order_relaxed);
        return MGMEM_OK;
    }
    static void release(void *context) noexcept {
        auto *self = static_cast<Origin *>(context);
        self->stats.releases.fetch_add(1, std::memory_order_relaxed);
        if (self->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        auto &stats = self->stats;
        delete self;
        stats.freed_on_other_thread.store(std::this_thread::get_id() != stats.creator);
        stats.freed.store(true, std::memory_order_release);
    }
};
using Engine = std::unique_ptr<dmg_engine, decltype(&dmg_engine_release)>;
using Snapshot = std::unique_ptr<dmg_snapshot, decltype(&dmg_snapshot_release)>;
using Result = std::unique_ptr<dmg_result, decltype(&dmg_result_release)>;

struct Input {
    bool imported;
    Origin *caller_origin;
    OriginStats &stats;
    mgmem_domain *domain = nullptr;
    mgmem_handle_v1 owner{};
    mgmem_borrow_v1 reader{};
    bool owner_live = false, reader_live = false, caller_live = true;
    std::atomic<uint32_t> references{1}, retains{0}, releases{0};
    std::atomic<mgmem_status> final_status{MGMEM_BUSY};
    std::atomic<bool> final_other_thread{false};
    const std::thread::id creator = std::this_thread::get_id();
    const uint8_t *data;
    const uint64_t size, capacity;
    uint64_t backing_peak = 0, imported_peak = 0;
#ifdef MGMEM_DEX_ARENA_IMPORT
    mgmem_handle_v1 workspace_owner{};
    mgmem_borrow_v1 workspace_lease{};
    bool workspace_owner_live = false, workspace_lease_live = false;
    uint64_t workspace_flushes = 0;
    void release_workspace() {
        if (workspace_lease_live) {
            memory_check(mgmem_borrow_release_v1(domain, workspace_lease.lease), "workspace_lease_release");
            workspace_lease_live = false;
        }
        if (workspace_owner_live) {
            memory_check(mgmem_release_v1(domain, workspace_owner), "workspace_owner_release");
            workspace_owner_live = false;
        }
    }
#endif
    explicit Input(bool mode, Origin *source, OriginStats &value)
        : imported(mode), caller_origin(source), stats(value), data(source->bytes.data()),
          size(source->bytes.size()), capacity(source->bytes.capacity()) {}
    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;
    static void retain(void *context) noexcept {
        auto &self = *static_cast<Input *>(context);
        self.references.fetch_add(1, std::memory_order_relaxed);
        self.retains.fetch_add(1, std::memory_order_relaxed);
    }
    static void release(void *context) noexcept {
        auto &self = *static_cast<Input *>(context);
        self.releases.fetch_add(1, std::memory_order_relaxed);
        if (self.references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        mgmem_status status = MGMEM_OK;
        if (self.reader_live) {
            status = mgmem_borrow_release_v1(self.domain, self.reader.lease);
            self.reader_live = false;
        }
        if (self.caller_origin) { Origin::release(self.caller_origin); self.caller_origin = nullptr; }
        self.final_other_thread.store(std::this_thread::get_id() != self.creator);
        self.final_status.store(status, std::memory_order_release);
    }
    void release_caller() noexcept { if (caller_live) { caller_live = false; release(this); } }
    mgmem_snapshot_v1 snapshot() {
        mgmem_snapshot_v1 state{};
        memory_check(mgmem_domain_snapshot_v1(domain, &state, sizeof(state)), "snapshot");
        return state;
    }
    void prepare(const std::string &test_case) {
        if (!imported) return;
        const mgmem_domain_options_v1 options{sizeof(options), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR,
#ifdef MGMEM_DEX_ARENA_IMPORT
            0, MGMEM_ARENA, 0, capacity + workspace_chunk, workspace_chunk, nullptr};
        require((mgmem_capabilities_v1() & MGMEM_CAP_ARENA_EXTERNAL_IMPORT) != 0, "arena_import_capability_missing");
#else
            0, MGMEM_SYSTEM, 0, capacity, 0, nullptr};
#endif
        memory_check(mgmem_domain_create_v1(&options, &domain), "domain_create");
        auto descriptor = mgmem_external_buffer_v1{sizeof(mgmem_external_buffer_v1), MGMEM_ABI_MAJOR,
            MGMEM_ABI_MINOR, 0, data, capacity, 1, caller_origin->identity, MGMEM_READ, 0,
            caller_origin, &Origin::retain, &Origin::release, mgmem_runtime_origin_v1()};
        mgmem_handle_v1 rejected{};
        if (test_case == "import-budget") {
            memory_check(mgmem_domain_set_budget_v1(domain, capacity - 1), "set_small_budget");
            require(mgmem_import_external_v1(domain, &descriptor, &rejected) == MGMEM_OUT_OF_BUDGET &&
                    rejected.id == 0 && stats.retains == 0, "import_budget_not_failure_atomic");
            memory_check(mgmem_domain_set_budget_v1(domain, capacity), "restore_budget");
        }
        if (test_case == "foreign-cookie") {
            auto foreign = descriptor; foreign.managing_runtime_origin ^= UINT64_C(0x5555);
            require(mgmem_import_external_v1(domain, &foreign, &rejected) == MGMEM_STALE_HANDLE &&
                    rejected.id == 0 && stats.retains == 0, "foreign_cookie_not_rejected_before_retain");
        }
#ifdef MGMEM_DEX_ARENA_IMPORT
        memory_check(mgmem_import_external_arena_v1(domain, &descriptor, &owner), "import"); owner_live = true;
#else
        memory_check(mgmem_import_external_v1(domain, &descriptor, &owner), "import"); owner_live = true;
#endif
        if (test_case == "duplicate") {
            // Permit reservation first so this case actually reaches the
            // identity/region registry instead of being rejected by budget.
            memory_check(mgmem_domain_set_budget_v1(domain, capacity * 2), "duplicate_headroom");
            require(mgmem_import_external_v1(domain, &descriptor, &rejected) == MGMEM_BUSY &&
                    rejected.id == 0 && stats.retains == 1, "duplicate_owner_not_rejected");
            memory_check(mgmem_domain_set_budget_v1(domain, capacity), "duplicate_restore_budget");
        }
        mgmem_borrow_v1 denied{};
        require(mgmem_borrow_acquire_v1(domain, owner, 0, size, MGMEM_WRITE, &denied) == MGMEM_UNSUPPORTED,
                "READ_import_allowed_WRITE");
        memory_check(mgmem_borrow_acquire_v1(domain, owner, 0, size, MGMEM_READ, &reader), "read_acquire");
        reader_live = true;
        require(reader.data == data && reader.length == size, "import_changed_input_address");
        auto state = snapshot(); mgmem_import_snapshot_v1 detail{};
        memory_check(mgmem_domain_import_snapshot_v1(domain, &detail, sizeof(detail)), "import_snapshot");
        require(state.backing_bytes == capacity && state.reserved_bytes == 0 && state.provider_allocation_count == 0 &&
                detail.external_backing_bytes == capacity && detail.external_backing_count == 1 &&
                detail.import_count == 1 && !detail.poisoned, "import_charge_not_unique_capacity");
        backing_peak = state.peak_backing_bytes; imported_peak = detail.external_backing_bytes;
#ifdef MGMEM_DEX_ARENA_IMPORT
        if (test_case == "workspace-budget") {
            memory_check(mgmem_domain_set_budget_v1(domain, capacity), "workspace_budget_limit");
            require(mgmem_allocate_v1(domain, workspace_bytes, alignof(uint64_t), &rejected) == MGMEM_OUT_OF_BUDGET &&
                    rejected.id == 0 && snapshot().backing_bytes == capacity, "workspace_budget_not_enforced");
            memory_check(mgmem_domain_set_budget_v1(domain, capacity + workspace_chunk), "workspace_budget_restore");
        }
        memory_check(mgmem_allocate_v1(domain, workspace_bytes, alignof(uint64_t), &workspace_owner), "workspace_allocate");
        workspace_owner_live = true;
        memory_check(mgmem_borrow_acquire_v1(domain, workspace_owner, 0, workspace_bytes, MGMEM_WRITE, &workspace_lease), "workspace_acquire");
        workspace_lease_live = true;
        state = snapshot();
        require(state.backing_bytes == capacity + workspace_chunk && state.provider_allocation_count == 1 &&
                state.active_leases == 2, "workspace_accounting_mismatch");
        backing_peak = state.peak_backing_bytes;
#endif
        if (test_case == "soft-budget") {
            memory_check(mgmem_domain_set_budget_v1(domain, 0), "soft_budget_zero");
            require(mgmem_allocate_v1(domain, 1, 1, &rejected) == MGMEM_OUT_OF_BUDGET && rejected.id == 0,
                    "zero_budget_admitted_new_backing");
            require(snapshot().backing_bytes == capacity
#ifdef MGMEM_DEX_ARENA_IMPORT
                    + workspace_chunk
#endif
                    && !stats.freed, "soft_budget_reclaimed_existing_lease");
        }
        Origin::release(caller_origin); caller_origin = nullptr;
        require(!stats.freed && stats.retains == 1 && stats.releases == 1, "import_not_retaining_origin");
    }
    void retire(bool sdk_holds_source) {
        release_caller();
        if (!imported) return;
        memory_check(mgmem_release_v1(domain, owner), "owner_release"); owner_live = false;
        memory_check(mgmem_domain_begin_close_v1(domain), "begin_close");
        if (sdk_holds_source) {
            const auto state = snapshot();
            require(state.state == MGMEM_CLOSING && state.retiring_bytes == capacity &&
                    state.active_leases ==
#ifdef MGMEM_DEX_ARENA_IMPORT
                    2
#else
                    1
#endif
                    && !stats.freed, "close_did_not_preserve_result_lease");
            require(mgmem_domain_collect_v1(domain) == MGMEM_BUSY &&
                    mgmem_domain_drain_v1(domain, 0) == MGMEM_TIMEOUT, "close_poll_freed_live_lease");
        }
    }
    void finish() {
        require(!caller_live && references == 0 && releases == retains + 1 &&
                final_status.load(std::memory_order_acquire) == MGMEM_OK && stats.freed,
                "source_or_bridge_not_finally_released");
        require(stats.releases == stats.retains + 1, "origin_callbacks_unbalanced");
        if (!domain) return;
        memory_check(mgmem_domain_collect_v1(domain), "collect");
        memory_check(mgmem_domain_drain_v1(domain, 0), "drain");
        const auto state = snapshot(); mgmem_import_snapshot_v1 detail{};
        memory_check(mgmem_domain_import_snapshot_v1(domain, &detail, sizeof(detail)), "final_import_snapshot");
        require(state.state == MGMEM_CLOSED && state.backing_bytes == 0 && state.reserved_bytes == 0 &&
                state.active_allocations == 0 && state.active_leases == 0 && state.retiring_bytes == 0 &&
                detail.external_backing_bytes == 0 && detail.external_backing_count == 0 &&
                detail.pending_import_bytes == 0 && !detail.poisoned, "final_resources_not_zero");
        memory_check(mgmem_domain_destroy_v1(domain), "destroy"); domain = nullptr;
    }
    ~Input() noexcept {
#ifdef MGMEM_DEX_ARENA_IMPORT
        try { release_workspace(); } catch (...) { std::terminate(); }
#endif
        release_caller();
        // SDK wrappers are declared later and are destroyed before this bridge.
        // Keeping a callback to a dead stack or destroying a busy domain is fatal.
        if (references != 0 || final_status.load(std::memory_order_acquire) != MGMEM_OK) std::terminate();
        if (!domain) return;
        if (owner_live && mgmem_release_v1(domain, owner) != MGMEM_OK) std::terminate();
        if (mgmem_domain_begin_close_v1(domain) != MGMEM_OK || mgmem_domain_collect_v1(domain) != MGMEM_OK ||
                mgmem_domain_drain_v1(domain, 0) != MGMEM_OK || mgmem_domain_destroy_v1(domain) != MGMEM_OK) std::terminate();
    }
};
uint64_t mix(uint64_t hash, uint64_t word) {
    for (unsigned i = 0; i < 8; ++i) { hash = (hash ^ (word & 255u)) * UINT64_C(1099511628211); word >>= 8; }
    return hash;
}
uint64_t consume(dmg_result *result, uint32_t expected
#ifdef MGMEM_DEX_ARENA_IMPORT
    , Input &input
#endif
) {
    dmg_result_info info{}; dex_check(dmg_result_get_info(result, &info), "result_info");
    require(info.execution_status == DMG_OK && info.coverage == DMG_COVERAGE_COMPLETE &&
            info.candidates_count == expected, "incomplete_query");
    uint64_t hash = UINT64_C(14695981039346656037);
#ifdef MGMEM_DEX_ARENA_IMPORT
    auto *words = static_cast<uint64_t *>(input.workspace_lease.data);
    size_t used = 0;
    const auto flush = [&] {
        if (!used) return;
        for (size_t i = 0; i < used; ++i) hash = mix(hash, words[i]);
        used = 0;
        ++input.workspace_flushes;
    };
    const auto add = [&](uint64_t word) {
        words[used++] = word;
        if (used == workspace_bytes / sizeof(uint64_t)) flush();
    };
#else
    const auto add = [&](uint64_t word) { hash = mix(hash, word); };
#endif
    for (uint32_t index = 0; index < expected; ++index) {
        dmg_method_row row{}; dex_check(dmg_result_get_method(result, index, &row), "method_row");
        add(row.method_index); add(row.access_flags);
        add(row.declaration_offset); add(row.code_offset);
        const uint16_t *texts[] = {row.owner, row.name, row.prototype};
        const uint64_t lengths[] = {row.owner_length, row.name_length, row.prototype_length};
        for (unsigned part = 0; part < 3; ++part) {
            require(texts[part] || !lengths[part], "invalid_method_text"); add(lengths[part]);
            for (uint64_t i = 0; i < lengths[part]; ++i) add(texts[part][i]);
        }
    }
#ifdef MGMEM_DEX_ARENA_IMPORT
    flush();
    require(input.workspace_flushes > 0, "workspace_was_not_used");
#endif
    return hash;
}
struct Outcome {
    uint64_t size = 0, capacity = 0, methods = 0, checksum = 0, backing = 0, imported = 0;
    uint64_t parser_peak = 0, origin_retains = 0, origin_releases = 0, sdk_retains = 0, sdk_releases = 0;
    bool other_thread = false;
#ifdef MGMEM_DEX_ARENA_IMPORT
    uint64_t workspace_flushes = 0;
#endif
    std::string sha;
};
Outcome run(const char *path, bool imported, const std::string &test_case) {
    OriginStats stats;
    auto source = std::make_unique<Origin>(stats);
    std::ifstream file(path, std::ios::binary | std::ios::ate); require(file.good(), "input_open_failed");
    const auto length = file.tellg(); require(length > 0 && static_cast<uint64_t>(length) <= DMG_MAX_SOURCE_BYTES,
            "input_size_out_of_bounds");
    source->bytes.resize(static_cast<size_t>(length)); file.seekg(0);
    require(static_cast<bool>(file.read(reinterpret_cast<char *>(source->bytes.data()), length)), "input_read_failed");
    require(source->bytes.capacity() <= input_limit, "source_capacity_exceeds_limit");
    if (test_case == "corrupt") source->bytes[0] ^= 0xff;
    Input input(imported, source.release(), stats); input.prepare(test_case);
    const dmg_engine_options limits{sizeof(limits), DMG_ABI_VERSION, parser_limit,
        test_case == "borrow-budget" ? input.size - 1 : input_limit, input_limit, 1, 0};
    dmg_engine *raw_engine = nullptr; dmg_error error{};
    dex_check(dmg_engine_create(&limits, &raw_engine, &error), "engine_create"); Engine engine(raw_engine, &dmg_engine_release);
    const dmg_open_options options{sizeof(options), DMG_ABI_VERSION, input_limit, "external-import-stage", "fixed-input", nullptr};
    const dmg_buffer_lease lease{sizeof(lease), DMG_ABI_VERSION, &input, &Input::retain, &Input::release, 1, 0};
    dmg_snapshot *raw_snapshot = nullptr;
    const auto status = dmg_snapshot_open_borrowed(engine.get(), input.data, input.size, &lease, &options, &raw_snapshot, &error);
    Snapshot snapshot(raw_snapshot, &dmg_snapshot_release);
    Outcome outcome; outcome.size = input.size; outcome.capacity = input.capacity;
    if (test_case == "corrupt" || test_case == "borrow-budget") {
        require(status == static_cast<dmg_status>(test_case == "corrupt" ? DMG_CORRUPT_INPUT : DMG_BUDGET_EXCEEDED) && !snapshot,
                "expected_parser_rejection_missing");
        require(input.retains == (test_case == "corrupt" ? 1u : 0u), "parser_rejection_retained_unexpectedly");
#ifdef MGMEM_DEX_ARENA_IMPORT
        input.release_workspace();
#endif
        input.retire(false);
    } else {
        dex_check(status, "open_borrowed"); dmg_snapshot_info info{};
        dex_check(dmg_snapshot_get_info(snapshot.get(), &info), "snapshot_info");
        require(info.method_def_count > 0 && info.method_def_count <= 65536 && info.source_sha256 &&
                std::strlen(info.source_sha256) == 64, "invalid_source_identity");
        outcome.methods = info.method_def_count; outcome.sha = info.source_sha256;
        input.retire(true);
        dmg_method_query query{}; query.struct_size = sizeof(query); query.abi_version = DMG_ABI_VERSION;
        query.intent = DMG_ENUMERATE; query.max_results = 65536;
        dmg_result *raw_result = nullptr; dex_check(dmg_query_methods(snapshot.get(), &query, &raw_result, &error), "query");
        Result result(raw_result, &dmg_result_release); snapshot.reset();
        require(input.references == 1 && !stats.freed, "result_not_retaining_source_after_snapshot_release");
        outcome.checksum = consume(result.get(), static_cast<uint32_t>(outcome.methods)
#ifdef MGMEM_DEX_ARENA_IMPORT
            , input
#endif
        );
#ifdef MGMEM_DEX_ARENA_IMPORT
        input.release_workspace();
#endif
        if (test_case == "cross-thread" || test_case == "soft-budget") {
            std::thread final_release([owned = std::move(result)]() mutable { owned.reset(); }); final_release.join();
            require(input.final_other_thread && stats.freed_on_other_thread, "final_release_not_on_worker");
        }
    }
    snapshot.reset();
    dmg_engine_stats final_stats{}; dex_check(dmg_engine_get_stats(engine.get(), &final_stats), "engine_stats");
    require(final_stats.borrowed_bytes == 0 && final_stats.active_jobs == 0, "SDK_resources_not_zero");
    outcome.parser_peak = final_stats.peak_owned_bytes; engine.reset(); input.finish();
    outcome.backing = input.backing_peak; outcome.imported = input.imported_peak;
    outcome.origin_retains = stats.retains; outcome.origin_releases = stats.releases;
    outcome.sdk_retains = input.retains; outcome.sdk_releases = input.releases;
    outcome.other_thread = input.final_other_thread;
#ifdef MGMEM_DEX_ARENA_IMPORT
    outcome.workspace_flushes = input.workspace_flushes;
#endif
    return outcome;
}
} // namespace

int main(int argc, char **argv) {
    const auto started = std::chrono::steady_clock::now();
    try {
        require(argc >= 4 && argc <= 12, "usage_file_--variant_direct_or_import_optional_--case_--cycles_--min-duration-ms_--wall-ms");
        std::string variant, test_case = "normal";
        uint32_t cycles = 1, min_duration_ms = 0, wall_ms = 1200000;
        bool case_set = false, cycles_set = false, duration_set = false, wall_set = false;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i]; require(++i < argc, "missing_option_value");
            const std::string value = argv[i];
            if (option == "--variant") { require(variant.empty(), "duplicate_variant"); variant = value; }
            else if (option == "--case") { require(!case_set, "duplicate_case"); test_case = value; case_set = true; }
            else if (option == "--cycles") {
                require(!cycles_set && !value.empty() && value.size() <= 5 &&
                        value.find_first_not_of("0123456789") == std::string::npos, "invalid_cycles");
                cycles = static_cast<uint32_t>(std::stoul(value)); require(cycles >= 1 && cycles <= 65536, "cycles_out_of_bounds");
                cycles_set = true;
            } else if (option == "--min-duration-ms" || option == "--wall-ms") {
                require(!value.empty() && value.size() <= 7 && value.find_first_not_of("0123456789") == std::string::npos,
                        "invalid_duration");
                const auto milliseconds = static_cast<uint32_t>(std::stoul(value));
                if (option == "--min-duration-ms") {
                    require(!duration_set && milliseconds <= 900000, "min_duration_out_of_bounds_or_duplicate");
                    min_duration_ms = milliseconds; duration_set = true;
                } else {
                    require(!wall_set && milliseconds >= 1 && milliseconds <= 1200000, "wall_out_of_bounds_or_duplicate");
                    wall_ms = milliseconds; wall_set = true;
                }
            } else throw std::runtime_error("unknown_option");
        }
#ifdef MGMEM_DEX_ARENA_IMPORT
        require(variant == "arena-import", "invalid_variant");
        require(test_case == "normal" || test_case == "cross-thread" || test_case == "corrupt" ||
                test_case == "borrow-budget" || test_case == "soft-budget" || test_case == "workspace-budget", "invalid_case");
#else
        require(variant == "direct" || variant == "import", "invalid_variant");
        require(test_case == "normal" || test_case == "cross-thread" || test_case == "corrupt" || test_case == "borrow-budget" ||
                (variant == "import" && (test_case == "soft-budget" || test_case == "duplicate" ||
                    test_case == "import-budget" || test_case == "foreign-cookie")), "invalid_case");
#endif
        require(dmg_abi_version() == DMG_ABI_VERSION && mgmem_abi_version_v1() >= 0x10001u, "ABI_mismatch");
        require(min_duration_ms <= wall_ms, "minimum_duration_exceeds_wall");
        const auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        };
        std::vector<Outcome> outcomes; outcomes.reserve(cycles);
        while (outcomes.size() < 65536 && elapsed_ms() < wall_ms) {
            outcomes.push_back(run(argv[1],
#ifdef MGMEM_DEX_ARENA_IMPORT
                true,
#else
                variant == "import",
#endif
                test_case));
            if (outcomes.size() >= cycles && elapsed_ms() >= min_duration_ms) break;
        }
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const bool complete = outcomes.size() >= cycles && duration >= min_duration_ms;
        std::cout << "{\"schema_version\":1,\"workload\":\"dex_external_import_complete_query\",\"success\":"
                  << (complete ? "true" : "false") << ",\"state\":\"" << (complete ? "PASS" : "INCOMPLETE")
                  << "\",\"variant\":\"" << variant << "\",\"case\":\"" << test_case << "\",\"cycles_completed\":" << outcomes.size()
                  << ",\"cycles_requested_minimum\":" << cycles << ",\"minimum_duration_ms\":" << min_duration_ms
                  << ",\"new_cycle_admission_wall_ms\":" << wall_ms << ",\"cycle_cap\":65536"
                  << ",\"run_duration_ms\":" << duration << ",\"actual_elapsed_ms\":" << duration
                  << ",\"stop_reason\":\"" << (complete ? "MINIMUM_CYCLES_AND_DURATION_MET" :
                      (outcomes.size() == 65536 ? "CYCLE_CAP_BEFORE_MINIMUM_DURATION" : "WALL_BEFORE_REQUIRED_WORK"))
                  << "\",\"timing_use\":\"FUNCTIONAL_STABILITY_ONLY_NOT_PERFORMANCE\",\"iterations\":[";
        for (size_t i = 0; i < outcomes.size(); ++i) {
            if (i) std::cout << ','; const auto &o = outcomes[i];
            std::cout << "{\"input_bytes\":" << o.size << ",\"source_capacity_bytes\":" << o.capacity
                      << ",\"source_sha256\":" << (o.sha.empty() ? "null" : "\"" + o.sha + "\"")
                      << ",\"method_count\":" << o.methods << ",\"result_checksum\":" << o.checksum
                      << ",\"memory_backing_peak_bytes\":" << o.backing << ",\"imported_backing_subset_bytes\":" << o.imported
                      << ",\"source_plus_input_unique_capacity_bytes\":" << o.capacity
                      << ",\"extra_input_backing_bytes\":0,\"copy_bytes\":0,\"provider_published_count\":"
#ifdef MGMEM_DEX_ARENA_IMPORT
                      << 1 << ",\"workspace_requested_bytes\":" << workspace_bytes
                      << ",\"workspace_backing_bytes\":" << workspace_chunk
                      << ",\"workspace_flushes\":" << o.workspace_flushes
#else
                      << 0
#endif
                      << ",\"parser_peak_owned_bytes\":" << o.parser_peak
                      << ",\"origin_retains\":" << o.origin_retains << ",\"origin_releases_including_caller\":" << o.origin_releases
                      << ",\"SDK_retains\":" << o.sdk_retains << ",\"SDK_releases_including_caller\":" << o.sdk_releases
                      << ",\"last_release_on_worker\":" << (o.other_thread ? "true" : "false")
                      << ",\"source_freed\":true,\"final_resources\":\"ZERO\",\"memory_domain\":"
#ifdef MGMEM_DEX_ARENA_IMPORT
                      << "\"CLOSED_ZERO\""
#else
                      << (variant == "import" ? "\"CLOSED_ZERO\"" : "null")
#endif
                      << ",\"memory_metadata_bytes\":null,\"rss_pss\":null,\"energy\":null}";
        }
        std::cout << "],\"scope\":\"independent_DexMeguviz_not_BIL\",\"performance_qualification\":\"NOT_MEASURED\"}\n";
        return complete ? 0 : 2;
    } catch (const std::exception &error) {
        std::cout << "{\"schema_version\":1,\"success\":false,\"failure_reason\":\"" << escaped(error.what()) << "\"}\n";
    } catch (...) { std::cout << "{\"schema_version\":1,\"success\":false,\"failure_reason\":\"unknown_exception\"}\n"; }
    return 1;
}
