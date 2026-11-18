// mgmem_arena_reset_v1 cost vs retained Arena chunk count.
//
// For each chunk level (1 / 64 / 1024 chunks of 4096 bytes) one ARENA domain is
// prepared once; every round then allocates one chunk-sized block per chunk
// (so every chunk is fully used exactly once), releases all owners, and times
// only the mgmem_arena_reset_v1 call. Preparation (allocate + release) is
// timed separately as context and never enters the reset samples. Snapshots
// prove the provider chunk count stays at the level for the whole run, i.e.
// reset rewinds retained chunks instead of returning them. A small level's
// single reset is below the 100 ns clock tick, so each round also batches
// consecutive resets (same backing-list walk, same zero stores) and reports
// the amortized per-reset cost; slopes use the batched values, cross-checked
// against the single-shot values at 1024 chunks where both resolve.
#include <mgmem/mgmem.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using u64 = uint64_t;
constexpr u64 chunk_bytes = 4096;
constexpr u64 levels[] = {1, 64, 1024};
constexpr size_t level_count = sizeof(levels) / sizeof(levels[0]);

struct Options {
    u64 rounds = 30, warmup_rounds = 5, batch_resets = 1000;
};

void check(mgmem_status status, const char *operation) {
    if (status != MGMEM_OK)
        throw std::runtime_error(std::string(operation) + "_status_" + std::to_string(status));
}
u64 number(const std::string &value) {
    if (value.empty() || value.size() > 8 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid_unsigned_number");
    return std::stoull(value);
}
std::string quote(const std::string &value) {
    std::string result = "\"";
    const char *hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
std::string module_path() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&mgmem_arena_reset_v1), &module))
        throw std::runtime_error("runtime_module_not_observed");
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("runtime_module_path_invalid");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("runtime_module_path_encoding");
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length),
            result.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("runtime_module_path_encoding");
    return result;
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&mgmem_arena_reset_v1), &info) || !info.dli_fname)
        throw std::runtime_error("runtime_module_not_observed");
    return info.dli_fname;
#endif
}
u64 ns(Clock::time_point start, Clock::time_point finish) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>((values.size() - 1) * fraction + 0.5);
    return values[index];
}
double mean(const std::vector<double> &values) {
    double total = 0;
    for (double value : values) total += value;
    return total / static_cast<double>(values.size());
}
mgmem_snapshot_v1 snapshot(mgmem_domain *domain, const char *operation) {
    mgmem_snapshot_v1 value{};
    check(mgmem_domain_snapshot_v1(domain, &value, sizeof(value)), operation);
    return value;
}

struct LevelResult {
    u64 chunks = 0;
    std::vector<double> reset_samples;
    std::vector<double> batch_samples;
    std::vector<double> prepare_samples;
};

// Fills every chunk with one chunk-sized allocation, touches both ends of the
// first and last block, then releases all owners. Returns allocate+release ns.
u64 prepare(mgmem_domain *domain, u64 chunks, std::vector<mgmem_handle_v1> &owners) {
    const auto started = Clock::now();
    for (u64 index = 0; index < chunks; ++index)
        check(mgmem_allocate_v1(domain, chunk_bytes, 16, &owners[static_cast<size_t>(index)]), "prepare_allocate");
    for (u64 probe : {u64{0}, chunks - 1}) {
        mgmem_borrow_v1 view{};
        check(mgmem_borrow_acquire_v1(domain, owners[static_cast<size_t>(probe)], 0, chunk_bytes,
            MGMEM_WRITE, &view), "prepare_borrow");
        auto *bytes = static_cast<volatile unsigned char *>(view.data);
        bytes[0] = 0x5a;
        bytes[chunk_bytes - 1] = 0xa5;
        if (bytes[0] != 0x5a || bytes[chunk_bytes - 1] != 0xa5)
            throw std::runtime_error("prepare_payload_mismatch");
        check(mgmem_borrow_release_v1(domain, view.lease), "prepare_borrow_release");
        if (chunks == 1) break;
    }
    for (u64 index = 0; index < chunks; ++index)
        check(mgmem_release_v1(domain, owners[static_cast<size_t>(index)]), "prepare_release");
    return ns(started, Clock::now());
}

LevelResult run_level(const Options &options, u64 chunks) {
    mgmem_domain_options_v1 config{sizeof(config), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR,
        0, MGMEM_ARENA, 0, chunks * chunk_bytes + chunk_bytes, chunk_bytes, nullptr};
    mgmem_domain *domain = nullptr;
    check(mgmem_domain_create_v1(&config, &domain), "create");
    std::vector<mgmem_handle_v1> owners(static_cast<size_t>(chunks));

    LevelResult result;
    result.chunks = chunks;
    const u64 total_rounds = options.warmup_rounds + options.rounds;
    for (u64 round = 0; round < total_rounds; ++round) {
        const u64 prepare_ns = prepare(domain, chunks, owners);
        if (round == 0) {
            const auto ready = snapshot(domain, "ready_snapshot");
            if (ready.provider_allocation_count != chunks || ready.backing_bytes != chunks * chunk_bytes ||
                    ready.active_allocations || ready.active_leases || ready.live_bytes || ready.retiring_bytes)
                throw std::runtime_error("preparation_mismatch");
        }
        const auto reset_started = Clock::now();
        const mgmem_status reset = mgmem_arena_reset_v1(domain);
        const auto reset_finished = Clock::now();
        check(reset, "arena_reset");
        // A reset on an already rewound arena walks the same backing list and
        // stores the same zero offsets, so consecutive resets repeat the same
        // per-chunk work. Batching them resolves levels whose single reset is
        // at or below one 100 ns clock tick; the single-shot sample above is
        // kept as the cross-check at levels where it resolves.
        const auto batch_started = Clock::now();
        for (u64 index = 0; index < options.batch_resets; ++index)
            check(mgmem_arena_reset_v1(domain), "arena_reset_batch");
        const auto batch_finished = Clock::now();
        if (round >= options.warmup_rounds) {
            result.reset_samples.push_back(static_cast<double>(ns(reset_started, reset_finished)));
            result.batch_samples.push_back(static_cast<double>(ns(batch_started, batch_finished)) /
                                           static_cast<double>(options.batch_resets));
            result.prepare_samples.push_back(static_cast<double>(prepare_ns));
        }
    }

    const auto retained = snapshot(domain, "retained_snapshot");
    if (retained.provider_allocation_count != chunks || retained.backing_bytes != chunks * chunk_bytes ||
            retained.active_allocations || retained.active_leases || retained.live_bytes || retained.retiring_bytes)
        throw std::runtime_error("retained_chunks_changed");
    check(mgmem_domain_begin_close_v1(domain), "begin_close");
    check(mgmem_domain_collect_v1(domain), "collect");
    check(mgmem_domain_drain_v1(domain, 0), "drain");
    const auto closed = snapshot(domain, "closed_snapshot");
    if (closed.state != MGMEM_CLOSED || closed.backing_bytes || closed.active_allocations || closed.active_leases)
        throw std::runtime_error("closed_resources_nonzero");
    check(mgmem_domain_destroy_v1(domain), "destroy");
    return result;
}
} // namespace

int main(int argc, char **argv) {
    try {
        Options options;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 == argc) throw std::runtime_error("missing_option_value");
            const std::string key = argv[index], value = argv[index + 1];
            if (key == "--rounds") options.rounds = number(value);
            else if (key == "--warmup-rounds") options.warmup_rounds = number(value);
            else if (key == "--batch-resets") options.batch_resets = number(value);
            else throw std::runtime_error("unknown_option");
        }
        if (!options.rounds || options.rounds > 100000 || options.warmup_rounds > 1000 ||
                !options.batch_resets || options.batch_resets > 1000000)
            throw std::runtime_error("workload_out_of_bounds");
        const auto module = module_path();

        std::vector<LevelResult> results;
        for (size_t index = 0; index < level_count; ++index)
            results.push_back(run_level(options, levels[index]));

        std::vector<double> reset_p50, reset_min, batch_p50;
        for (const auto &result : results) {
            reset_p50.push_back(percentile(result.reset_samples, .5));
            reset_min.push_back(percentile(result.reset_samples, 0));
            batch_p50.push_back(percentile(result.batch_samples, .5));
        }
        auto slope = [&](const std::vector<double> &values, size_t from, size_t to) {
            return (values[to] - values[from]) / static_cast<double>(levels[to] - levels[from]);
        };
        std::cout << std::setprecision(15)
            << "{\"schema_version\":1,\"workload\":\"arena_reset_slope_v1\",\"success\":true,"
            << "\"scope\":\"SYNTHETIC_SHARED_DOMAIN_API_NOT_PRODUCT\","
            << "\"runtime_module\":" << quote(module) << ",\"abi\":" << mgmem_abi_version_v1()
            << ",\"chunk_bytes\":" << chunk_bytes << ",\"allocations_per_chunk\":1"
            << ",\"rounds\":" << options.rounds << ",\"warmup_rounds\":" << options.warmup_rounds
            << ",\"batch_resets\":" << options.batch_resets
            << ",\"levels\":[";
        for (size_t index = 0; index < results.size(); ++index) {
            if (index) std::cout << ',';
            const auto &result = results[index];
            std::cout << "{\"chunks\":" << result.chunks
                << ",\"backing_bytes\":" << result.chunks * chunk_bytes
                << ",\"reset_samples\":" << result.reset_samples.size()
                << ",\"reset_ns_min\":" << reset_min[index]
                << ",\"reset_ns_p50\":" << reset_p50[index]
                << ",\"reset_ns_p95\":" << percentile(result.reset_samples, .95)
                << ",\"reset_ns_mean\":" << mean(result.reset_samples)
                << ",\"reset_ns_p50_per_chunk\":" << reset_p50[index] / result.chunks
                << ",\"batched_reset_ns_per_reset_min\":" << percentile(result.batch_samples, 0)
                << ",\"batched_reset_ns_per_reset_p50\":" << batch_p50[index]
                << ",\"batched_reset_ns_per_reset_mean\":" << mean(result.batch_samples)
                << ",\"prepare_alloc_release_ns_p50\":" << percentile(result.prepare_samples, .5)
                << ",\"reset_ns_samples\":[";
            for (size_t sample = 0; sample < result.reset_samples.size(); ++sample) {
                if (sample) std::cout << ',';
                std::cout << result.reset_samples[sample];
            }
            std::cout << "]}";
        }
        std::cout << "],\"slope_ns_per_chunk_single_p50_1_to_64\":" << slope(reset_p50, 0, 1)
            << ",\"slope_ns_per_chunk_single_p50_64_to_1024\":" << slope(reset_p50, 1, 2)
            << ",\"slope_ns_per_chunk_single_p50_1_to_1024\":" << slope(reset_p50, 0, 2)
            << ",\"slope_ns_per_chunk_single_min_1_to_1024\":" << slope(reset_min, 0, 2)
            << ",\"slope_ns_per_chunk_batched_p50_1_to_64\":" << slope(batch_p50, 0, 1)
            << ",\"slope_ns_per_chunk_batched_p50_64_to_1024\":" << slope(batch_p50, 1, 2)
            << ",\"slope_ns_per_chunk_batched_p50_1_to_1024\":" << slope(batch_p50, 0, 2)
            << ",\"final_resources\":\"CLOSED_ZERO\",\"performance_qualified\":false}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "{\"schema_version\":1,\"workload\":\"arena_reset_slope_v1\",\"success\":false,\"error\":"
            << quote(error.what()) << ",\"performance_qualified\":false}\n";
        return 1;
    }
}
