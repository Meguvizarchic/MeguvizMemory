// Marginal external-import cost vs live import count.
//
// For each background level (0 / 1024 / 16384 live external imports held in one
// SYSTEM domain), repeatedly time one additional mgmem_import_external_v1 call
// against a rotating pool of disjoint probe regions, then release the probe
// outside the timed interval. The background stays live for the whole level, so
// every timed import runs the full duplicate-identity / overlap check against
// all live claims. Differences between levels isolate the per-claim slope from
// the constant import cost (locks, handle-table insert, retain callback).
//
// Probe release time is recorded as a diagnostic: release drops one known
// claim node and is not expected to scale with the live claim count.
#include <mgmem/mgmem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <malloc.h>
#include <windows.h>
#else
#include <cstdlib>
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using u64 = uint64_t;
constexpr u64 slot_bytes = 64;
constexpr u64 slot_alignment = 64;
constexpr u64 probe_pool = 32;
constexpr u64 levels[] = {0, 1024, 16384};
constexpr size_t level_count = sizeof(levels) / sizeof(levels[0]);

struct Options {
    u64 probes = 2000, rounds = 5, warmup_probes = 200;
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
            reinterpret_cast<LPCWSTR>(&mgmem_import_external_v1), &module))
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
    if (!dladdr(reinterpret_cast<void *>(&mgmem_import_external_v1), &info) || !info.dli_fname)
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

struct FakeOwner {
    std::atomic<uint32_t> refs{1};
    std::atomic<u64> retains{0};
    std::atomic<u64> releases{0};
    static mgmem_status retain(void *context) {
        auto &owner = *static_cast<FakeOwner *>(context);
        owner.refs.fetch_add(1, std::memory_order_relaxed);
        owner.retains.fetch_add(1, std::memory_order_relaxed);
        return MGMEM_OK;
    }
    static void release(void *context) {
        auto &owner = *static_cast<FakeOwner *>(context);
        owner.refs.fetch_sub(1, std::memory_order_relaxed);
        owner.releases.fetch_add(1, std::memory_order_relaxed);
    }
};

struct LevelResult {
    u64 background = 0;
    u64 preset_build_ns = 0;
    std::vector<double> import_samples;
    std::vector<double> release_samples;
    std::vector<double> round_import_p50;
};

mgmem_external_buffer_v1 descriptor(unsigned char *base, u64 slot, u64 external_id, FakeOwner &owner) {
    mgmem_external_buffer_v1 buffer{};
    buffer.struct_size = sizeof(buffer);
    buffer.abi_major = MGMEM_ABI_MAJOR;
    buffer.abi_minor = MGMEM_ABI_MINOR;
    buffer.data = base + slot * slot_bytes;
    buffer.capacity_bytes = slot_bytes;
    buffer.alignment = slot_alignment;
    buffer.external_id = external_id;
    buffer.access = MGMEM_READ;
    buffer.context = &owner;
    buffer.retain = &FakeOwner::retain;
    buffer.release = &FakeOwner::release;
    buffer.managing_runtime_origin = mgmem_runtime_origin_v1();
    return buffer;
}

// One probe cycle: time only the import (and, separately, only the release).
// The borrow between them proves the imported handle is usable and points at
// the probe region with its preset pattern; it is outside both intervals.
void probe_once(mgmem_domain *domain, unsigned char *base, u64 slot, u64 external_id,
                FakeOwner &owner, u64 *import_ns, u64 *release_ns) {
    const auto buffer = descriptor(base, slot, external_id, owner);
    mgmem_handle_v1 handle{};
    const auto import_started = Clock::now();
    const mgmem_status imported = mgmem_import_external_v1(domain, &buffer, &handle);
    const auto import_finished = Clock::now();
    check(imported, "probe_import");
    mgmem_borrow_v1 view{};
    check(mgmem_borrow_acquire_v1(domain, handle, 0, slot_bytes, MGMEM_READ, &view), "probe_borrow");
    if (view.data != base + slot * slot_bytes || view.length != slot_bytes)
        throw std::runtime_error("probe_view_mismatch");
    const auto *bytes = static_cast<const unsigned char *>(view.data);
    for (u64 index = 0; index < slot_bytes; ++index)
        if (bytes[index] != static_cast<unsigned char>(slot + index))
            throw std::runtime_error("probe_payload_mismatch");
    check(mgmem_borrow_release_v1(domain, view.lease), "probe_borrow_release");
    const auto release_started = Clock::now();
    const mgmem_status released = mgmem_release_v1(domain, handle);
    const auto release_finished = Clock::now();
    check(released, "probe_release");
    if (import_ns) *import_ns = ns(import_started, import_finished);
    if (release_ns) *release_ns = ns(release_started, release_finished);
}

LevelResult run_level(const Options &options, u64 background, u64 level_index,
                      unsigned char *base, std::vector<FakeOwner> &owners) {
    const u64 id_base = 1 + level_index * 1000000;
    mgmem_domain_options_v1 config{sizeof(config), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR,
        0, MGMEM_SYSTEM, 0, (background + probe_pool + 16) * slot_bytes, 4096, nullptr};
    mgmem_domain *domain = nullptr;
    check(mgmem_domain_create_v1(&config, &domain), "create");

    LevelResult result;
    result.background = background;
    std::vector<mgmem_handle_v1> held;
    held.reserve(static_cast<size_t>(background));
    const auto preset_started = Clock::now();
    for (u64 index = 0; index < background; ++index) {
        const auto buffer = descriptor(base, index, id_base + index, owners[static_cast<size_t>(index)]);
        mgmem_handle_v1 handle{};
        check(mgmem_import_external_v1(domain, &buffer, &handle), "preset_import");
        held.push_back(handle);
    }
    result.preset_build_ns = ns(preset_started, Clock::now());

    mgmem_import_snapshot_v1 preset{};
    check(mgmem_domain_import_snapshot_v1(domain, &preset, sizeof(preset)), "preset_snapshot");
    if (preset.external_backing_count != background ||
            preset.external_backing_bytes != background * slot_bytes || preset.poisoned)
        throw std::runtime_error("preset_snapshot_mismatch");

    const u64 probe_slot_base = levels[level_count - 1];
    const u64 probe_id_base = id_base + 500000;
    auto probe_slot = [&](u64 ordinal) { return probe_slot_base + ordinal % probe_pool; };
    auto probe_owner = [&](u64 ordinal) -> FakeOwner & {
        return owners[static_cast<size_t>(probe_slot(ordinal))];
    };
    for (u64 index = 0; index < options.warmup_probes; ++index)
        probe_once(domain, base, probe_slot(index), probe_id_base + index % probe_pool,
                   probe_owner(index), nullptr, nullptr);

    for (u64 round = 0; round < options.rounds; ++round) {
        std::vector<double> round_samples;
        round_samples.reserve(static_cast<size_t>(options.probes));
        for (u64 index = 0; index < options.probes; ++index) {
            const u64 ordinal = round * options.probes + index;
            u64 import_sample = 0, release_sample = 0;
            probe_once(domain, base, probe_slot(ordinal), probe_id_base + ordinal % probe_pool,
                       probe_owner(ordinal), &import_sample, &release_sample);
            round_samples.push_back(static_cast<double>(import_sample));
            result.release_samples.push_back(static_cast<double>(release_sample));
        }
        result.round_import_p50.push_back(percentile(round_samples, .5));
        result.import_samples.insert(result.import_samples.end(), round_samples.begin(), round_samples.end());
        mgmem_import_snapshot_v1 after_round{};
        check(mgmem_domain_import_snapshot_v1(domain, &after_round, sizeof(after_round)), "round_snapshot");
        if (after_round.external_backing_count != background || after_round.poisoned)
            throw std::runtime_error("round_snapshot_mismatch");
    }

    for (auto handle : held) check(mgmem_release_v1(domain, handle), "preset_release");
    mgmem_import_snapshot_v1 drained{};
    check(mgmem_domain_import_snapshot_v1(domain, &drained, sizeof(drained)), "drained_snapshot");
    if (drained.external_backing_count != 0 || drained.external_backing_bytes != 0)
        throw std::runtime_error("drained_snapshot_mismatch");
    check(mgmem_domain_begin_close_v1(domain), "begin_close");
    check(mgmem_domain_collect_v1(domain), "collect");
    check(mgmem_domain_drain_v1(domain, 0), "drain");
    mgmem_snapshot_v1 closed{};
    check(mgmem_domain_snapshot_v1(domain, &closed, sizeof(closed)), "closed_snapshot");
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
            if (key == "--probes") options.probes = number(value);
            else if (key == "--rounds") options.rounds = number(value);
            else if (key == "--warmup-probes") options.warmup_probes = number(value);
            else throw std::runtime_error("unknown_option");
        }
        if (!options.probes || options.probes > 100000 || !options.rounds || options.rounds > 64 ||
                options.warmup_probes > 100000)
            throw std::runtime_error("workload_out_of_bounds");
        if ((mgmem_capabilities_v1() & MGMEM_CAP_EXTERNAL_IMPORT) == 0)
            throw std::runtime_error("external_import_capability_missing");
        const auto module = module_path();

        const u64 total_slots = levels[level_count - 1] + probe_pool;
#if defined(_WIN32)
        auto *base = static_cast<unsigned char *>(
            _aligned_malloc(static_cast<size_t>(total_slots * slot_bytes), static_cast<size_t>(slot_alignment)));
#else
        void *allocation = nullptr;
        if (posix_memalign(&allocation, static_cast<size_t>(slot_alignment),
                static_cast<size_t>(total_slots * slot_bytes)) != 0)
            allocation = nullptr;
        auto *base = static_cast<unsigned char *>(allocation);
#endif
        if (!base) throw std::runtime_error("fake_storage_alloc_failed");
        for (u64 slot = 0; slot < total_slots; ++slot)
            for (u64 index = 0; index < slot_bytes; ++index)
                base[slot * slot_bytes + index] = static_cast<unsigned char>(slot + index);
        std::vector<FakeOwner> owners(static_cast<size_t>(total_slots));

        std::vector<LevelResult> results;
        for (size_t index = 0; index < level_count; ++index)
            results.push_back(run_level(options, levels[index], index, base, owners));

        for (const auto &owner : owners) {
            if (owner.refs.load() != 1 || owner.retains.load() != owner.releases.load())
                throw std::runtime_error("fake_owner_refcount_mismatch");
        }
#if defined(_WIN32)
        _aligned_free(base);
#else
        std::free(base);
#endif

        std::vector<double> import_p50;
        for (const auto &result : results) import_p50.push_back(percentile(result.import_samples, .5));
        auto slope = [&](size_t from, size_t to) {
            return (import_p50[to] - import_p50[from]) /
                   static_cast<double>(levels[to] - levels[from]);
        };
        std::cout << std::setprecision(15)
            << "{\"schema_version\":1,\"workload\":\"import_slope_v1\",\"success\":true,"
            << "\"scope\":\"SYNTHETIC_SHARED_DOMAIN_API_NOT_PRODUCT\","
            << "\"runtime_module\":" << quote(module) << ",\"abi\":" << mgmem_abi_version_v1()
            << ",\"slot_bytes\":" << slot_bytes << ",\"slot_alignment\":" << slot_alignment
            << ",\"probe_pool\":" << probe_pool << ",\"probes_per_round\":" << options.probes
            << ",\"rounds\":" << options.rounds << ",\"warmup_probes\":" << options.warmup_probes
            << ",\"levels\":[";
        for (size_t index = 0; index < results.size(); ++index) {
            if (index) std::cout << ',';
            const auto &result = results[index];
            std::cout << "{\"background_live_imports\":" << result.background
                << ",\"preset_build_ns\":" << result.preset_build_ns
                << ",\"preset_build_ns_per_import\":"
                << (result.background ? static_cast<double>(result.preset_build_ns) / result.background : 0)
                << ",\"probe_import_samples\":" << result.import_samples.size()
                << ",\"probe_import_ns_min\":" << percentile(result.import_samples, 0)
                << ",\"probe_import_ns_p50\":" << import_p50[index]
                << ",\"probe_import_ns_p95\":" << percentile(result.import_samples, .95)
                << ",\"probe_import_ns_mean\":" << mean(result.import_samples)
                << ",\"probe_release_ns_p50\":" << percentile(result.release_samples, .5)
                << ",\"round_probe_import_ns_p50\":[";
            for (size_t round = 0; round < result.round_import_p50.size(); ++round) {
                if (round) std::cout << ',';
                std::cout << result.round_import_p50[round];
            }
            std::cout << "]}";
        }
        std::cout << "],\"slope_ns_per_live_claim_0_to_1024\":" << slope(0, 1)
            << ",\"slope_ns_per_live_claim_1024_to_16384\":" << slope(1, 2)
            << ",\"slope_ns_per_live_claim_0_to_16384\":" << slope(0, 2)
            << ",\"final_resources\":\"CLOSED_ZERO\",\"performance_qualified\":false}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "{\"schema_version\":1,\"workload\":\"import_slope_v1\",\"success\":false,\"error\":"
            << quote(error.what()) << ",\"performance_qualified\":false}\n";
        return 1;
    }
}
