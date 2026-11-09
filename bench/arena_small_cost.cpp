#include <mgmem/mgmem.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using u64 = uint64_t;
constexpr u64 chunk_bytes = 4096;
struct Options {
    std::string shape = "fixed", mode = "stream", executor = "inline";
    u64 batch = 32, cycles = 128, rounds = 8, warmup = 2;
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
            reinterpret_cast<LPCWSTR>(&mgmem_allocate_v1), &module))
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
    if (!dladdr(reinterpret_cast<void *>(&mgmem_allocate_v1), &info) || !info.dli_fname)
        throw std::runtime_error("runtime_module_not_observed");
    return info.dli_fname;
#endif
}
u64 ns(Clock::time_point start, Clock::time_point finish) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}
u64 size_for(const Options &options, u64 operation) {
    return options.shape == "mixed" ? (operation % 4 + 1) * 32 : 64;
}
u64 alignment_for(const Options &options, u64 operation) {
    return options.shape == "mixed" ? u64{8} << (operation % 4) : 16;
}
u64 prepared_chunks(const Options &options) {
    // Cover every cycle's possible starting shape; all prepared blocks guarantee 64 alignment.
    u64 maximum = 1;
    for (u64 phase = 0; phase < 4; ++phase) {
        std::vector<u64> used;
        for (u64 i = 0; i < options.batch; ++i) {
            const auto bytes = size_for(options, phase + i);
            const auto alignment = alignment_for(options, phase + i);
            bool fitted = false;
            for (auto &position : used) {
                const auto offset = (position + alignment - 1) & ~(alignment - 1);
                if (offset <= chunk_bytes && bytes <= chunk_bytes - offset) {
                    position = offset + bytes; fitted = true; break;
                }
            }
            if (!fitted) used.push_back(bytes);
        }
        maximum = std::max(maximum, static_cast<u64>(used.size()));
    }
    return maximum;
}
struct Slot { mgmem_handle_v1 owner{}; mgmem_borrow_v1 view{}; u64 operation = 0, bytes = 0, alignment = 0; };
struct Cycle {
    u64 reset_ns = 0, hot_ns = 0, checksum = 0, payload_bytes = 0;
    std::vector<u64> phases;
};
struct Round {
    u64 prepare_ns = 0, close_ns = 0, loop_ns = 0, outer_wall_ns = 0, create_ready_ns = 0;
    u64 round_wall_ns = 0;
    u64 checksum = 0, payload_bytes = 0, providers = 0, peak = 0, allocation_count = 0;
    std::vector<Cycle> cycles;
};
void allocate_batch(mgmem_domain *domain, std::vector<Slot> &slots) {
    for (auto &slot : slots) check(mgmem_allocate_v1(domain, slot.bytes, slot.alignment, &slot.owner), "allocate");
}
void acquire(mgmem_domain *domain, Slot &slot) {
    check(mgmem_borrow_acquire_v1(domain, slot.owner, 0, slot.bytes, MGMEM_WRITE, &slot.view), "borrow");
}
void payload(Slot &slot, Cycle &cycle) {
    auto *bytes = static_cast<volatile unsigned char *>(slot.view.data);
    if (reinterpret_cast<uintptr_t>(bytes) % slot.alignment) throw std::runtime_error("alignment_mismatch");
    const u64 seed = slot.operation * 31;
    for (u64 byte = 0; byte < slot.bytes; ++byte) bytes[byte] = static_cast<unsigned char>(seed + byte);
    for (u64 byte = 0; byte < slot.bytes; ++byte) {
        if (bytes[byte] != static_cast<unsigned char>(seed + byte)) throw std::runtime_error("payload_mismatch");
        cycle.checksum += bytes[byte];
    }
    cycle.payload_bytes += slot.bytes;
}
Round run_round(const Options &options, u64 chunks) {
    Round result;
    result.cycles.resize(static_cast<size_t>(options.cycles));
    for (auto &cycle : result.cycles) cycle.phases.resize(options.mode == "stream" ? 2 : 5);
    std::vector<Slot> slots(static_cast<size_t>(options.batch));
    const auto round_started = Clock::now();
    const auto preparing = Clock::now();
    mgmem_domain_options_v1 config{sizeof(config), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR,
        0, MGMEM_ARENA, 0, chunks * chunk_bytes, chunk_bytes, nullptr};
    mgmem_domain *domain = nullptr;
    check(mgmem_domain_create_v1(&config, &domain), "create");
    std::vector<mgmem_handle_v1> prepared(static_cast<size_t>(chunks));
    for (auto &owner : prepared) check(mgmem_allocate_v1(domain, chunk_bytes, 64, &owner), "prepare_allocate");
    for (auto owner : prepared) check(mgmem_release_v1(domain, owner), "prepare_release");
    check(mgmem_arena_reset_v1(domain), "prepare_reset");
    mgmem_snapshot_v1 ready{};
    check(mgmem_domain_snapshot_v1(domain, &ready, sizeof(ready)), "prepared_snapshot");
    if (ready.active_allocations || ready.active_leases || ready.live_bytes || ready.retiring_bytes ||
            ready.reserved_bytes || ready.provider_allocation_count != chunks ||
            ready.backing_bytes != chunks * chunk_bytes || ready.peak_backing_bytes != chunks * chunk_bytes)
        throw std::runtime_error("prepared_resources_mismatch");
    result.prepare_ns = ns(preparing, Clock::now());
    std::string worker_error;
    auto execute = [&] {
        const auto loop_started = Clock::now();
        try {
            for (u64 cycle_index = 0; cycle_index < options.cycles; ++cycle_index) {
                auto &cycle = result.cycles[static_cast<size_t>(cycle_index)];
                for (u64 i = 0; i < options.batch; ++i) {
                    auto &slot = slots[static_cast<size_t>(i)];
                    slot.operation = cycle_index * options.batch + i;
                    slot.bytes = size_for(options, slot.operation);
                    slot.alignment = alignment_for(options, slot.operation);
                }
                const auto resetting = Clock::now();
                check(mgmem_arena_reset_v1(domain), "cycle_reset");
                cycle.reset_ns = ns(resetting, Clock::now());
                const auto first = Clock::now();
                allocate_batch(domain, slots);
                const auto allocated = Clock::now();
                cycle.phases[0] = ns(first, allocated);
                Clock::time_point finished;
                if (options.mode == "stream") {
                    // Preserve the original lookup benchmark's live set: at most one lease.
                    for (auto &slot : slots) {
                        acquire(domain, slot); payload(slot, cycle);
                        check(mgmem_release_v1(domain, slot.owner), "release_owner");
                        check(mgmem_borrow_release_v1(domain, slot.view.lease), "release_lease");
                    }
                    finished = Clock::now();
                    cycle.phases[1] = ns(allocated, finished);
                } else {
                    // This diagnostic has a different live set and is never subtracted from stream.
                    for (auto &slot : slots) acquire(domain, slot);
                    const auto borrowed = Clock::now();
                    for (auto &slot : slots) payload(slot, cycle);
                    const auto accessed = Clock::now();
                    for (auto &slot : slots) check(mgmem_release_v1(domain, slot.owner), "release_owner");
                    const auto retired = Clock::now();
                    for (auto &slot : slots) check(mgmem_borrow_release_v1(domain, slot.view.lease), "release_lease");
                    finished = Clock::now();
                    cycle.phases[1] = ns(allocated, borrowed);
                    cycle.phases[2] = ns(borrowed, accessed);
                    cycle.phases[3] = ns(accessed, retired);
                    cycle.phases[4] = ns(retired, finished);
                }
                cycle.hot_ns = ns(first, finished);
                result.checksum += cycle.checksum;
                result.payload_bytes += cycle.payload_bytes;
            }
        } catch (const std::exception &error) { worker_error = error.what(); }
        catch (...) { worker_error = "unknown_worker_error"; }
        result.loop_ns = ns(loop_started, Clock::now());
    };
    if (options.executor == "inline") {
        const auto started = Clock::now(); execute(); result.outer_wall_ns = ns(started, Clock::now());
    } else {
        std::mutex mutex;
        std::condition_variable changed;
        bool worker_ready = false, go = false;
        const auto creating = Clock::now();
        std::thread worker([&] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                worker_ready = true; changed.notify_all();
                changed.wait(lock, [&] { return go; });
            }
            execute();
        });
        Clock::time_point started;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return worker_ready; });
            result.create_ready_ns = ns(creating, Clock::now());
            started = Clock::now(); go = true;
        }
        changed.notify_all(); worker.join();
        result.outer_wall_ns = ns(started, Clock::now());
    }
    if (!worker_error.empty()) throw std::runtime_error(worker_error);
    mgmem_snapshot_v1 before{};
    check(mgmem_domain_snapshot_v1(domain, &before, sizeof(before)), "before_close_snapshot");
    if (before.active_allocations || before.active_leases || before.live_bytes || before.retiring_bytes ||
            before.reserved_bytes || before.provider_allocation_count != chunks ||
            before.backing_bytes != chunks * chunk_bytes || before.peak_backing_bytes != chunks * chunk_bytes ||
            before.allocation_count != chunks + options.batch * options.cycles)
        throw std::runtime_error("timed_resources_or_capacity_changed");
    result.providers = before.provider_allocation_count; result.peak = before.peak_backing_bytes;
    result.allocation_count = before.allocation_count;
    const auto closing = Clock::now();
    check(mgmem_domain_begin_close_v1(domain), "begin_close");
    check(mgmem_domain_collect_v1(domain), "collect");
    check(mgmem_domain_drain_v1(domain, 0), "drain");
    mgmem_snapshot_v1 after{};
    check(mgmem_domain_snapshot_v1(domain, &after, sizeof(after)), "closed_snapshot");
    if (after.state != MGMEM_CLOSED || after.backing_bytes || after.reserved_bytes || after.live_bytes ||
            after.retiring_bytes || after.active_allocations || after.active_leases)
        throw std::runtime_error("closed_resources_nonzero");
    check(mgmem_domain_destroy_v1(domain), "destroy");
    result.close_ns = ns(closing, Clock::now());
    result.round_wall_ns = ns(round_started, Clock::now());
    return result;
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>((values.size() - 1) * fraction + 0.5)];
}
void metric(const char *name, const std::vector<double> &values) {
    std::cout << quote(std::string(name) + "_p50") << ':' << percentile(values, .5) << ','
              << quote(std::string(name) + "_p95") << ':' << percentile(values, .95);
}
}
int main(int argc, char **argv) {
    try {
        Options options;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::runtime_error("missing_option_value");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--shape") options.shape = value;
            else if (key == "--mode") options.mode = value;
            else if (key == "--executor") options.executor = value;
            else if (key == "--batch") options.batch = number(value);
            else if (key == "--cycles") options.cycles = number(value);
            else if (key == "--rounds") options.rounds = number(value);
            else if (key == "--warmup") options.warmup = number(value);
            else throw std::runtime_error("unknown_option");
        }
        if ((options.shape != "fixed" && options.shape != "mixed") ||
                (options.mode != "stream" && options.mode != "stages") ||
                (options.executor != "inline" && options.executor != "worker") ||
                options.batch < 1 || options.batch > 128 || options.cycles < 1 || options.cycles > 4096 ||
                options.rounds < 2 || options.rounds > 64 || options.warmup > 16 ||
                options.batch * options.cycles * (options.rounds + options.warmup) > 4194304)
            throw std::runtime_error("workload_out_of_bounds");
        const auto module = module_path();
        const u64 chunks = prepared_chunks(options), operations = options.batch * options.cycles;
        for (u64 round = 0; round < options.warmup; ++round) (void)run_round(options, chunks);
        std::vector<Round> rounds;
        rounds.reserve(static_cast<size_t>(options.rounds));
        for (u64 round = 0; round < options.rounds; ++round) rounds.push_back(run_round(options, chunks));
        const std::vector<std::string> names = options.mode == "stream"
            ? std::vector<std::string>{"allocate_ns_per_op", "stream_remainder_ns_per_op"}
            : std::vector<std::string>{"allocate_ns_per_op", "borrow_ns_per_op", "payload_ns_per_op", "owner_release_ns_per_op", "lease_release_ns_per_op"};
        std::vector<std::vector<double>> phases(names.size());
        std::vector<double> hot, reset, loops, outer, prepare, close, creation, round_wall;
        u64 checksum = 0, payload_bytes = 0, total_hot = 0;
        for (const auto &round : rounds) {
            checksum += round.checksum; payload_bytes += round.payload_bytes;
            loops.push_back(static_cast<double>(round.loop_ns) / operations);
            outer.push_back(static_cast<double>(round.outer_wall_ns) / operations);
            round_wall.push_back(static_cast<double>(round.round_wall_ns) / operations);
            prepare.push_back(static_cast<double>(round.prepare_ns)); close.push_back(static_cast<double>(round.close_ns));
            creation.push_back(static_cast<double>(round.create_ready_ns));
            for (const auto &cycle : round.cycles) {
                hot.push_back(static_cast<double>(cycle.hot_ns) / options.batch);
                reset.push_back(static_cast<double>(cycle.reset_ns)); total_hot += cycle.hot_ns;
                for (size_t i = 0; i < phases.size(); ++i) phases[i].push_back(static_cast<double>(cycle.phases[i]) / options.batch);
            }
        }
        std::cout << std::setprecision(15) << "{\"schema_version\":1,\"workload\":"
            << quote(options.mode == "stream" ? "arena_small_cost_stream_v1" : "arena_small_cost_staged_v1")
            << ",\"success\":true,\"performance_qualified\":false,\"scope\":\"PREPARED_ARENA_FIXED_COST_DIAGNOSTIC_NOT_PRODUCT\""
            << ",\"runtime_module\":" << quote(module) << ",\"abi\":" << mgmem_abi_version_v1()
            << ",\"shape\":" << quote(options.shape) << ",\"mode\":" << quote(options.mode)
            << ",\"executor\":" << quote(options.executor) << ",\"batch\":" << options.batch
            << ",\"cycles_per_round\":" << options.cycles << ",\"rounds\":" << options.rounds
            << ",\"warmup_rounds\":" << options.warmup << ",\"prepared_chunks\":" << chunks
            << ",\"chunk_bytes\":4096,\"preparation_alignment\":64,\"preparation_reset\":true"
            << ",\"completed_operations\":" << operations * options.rounds << ",\"checksum\":" << checksum
            << ",\"payload_bytes\":" << payload_bytes << ",\"final_resources\":\"CLOSED_ZERO\""
            << ",\"logical_peak_allocation_records\":" << options.batch
            << ",\"logical_peak_lease_records\":" << (options.mode == "stream" ? 1 : options.batch)
            << ",\"hot_clock_reads_per_cycle\":" << (options.mode == "stream" ? 3 : 6)
            << ",\"reset_clock_reads_per_cycle\":2,\"round_wall_clock_reads_per_round\":2"
            << ",\"round_wall_scope\":\"BEFORE_DOMAIN_CREATE_THROUGH_DOMAIN_DESTROY\""
            << ",\"semantic_metadata_operations\":{\"allocation_insertions\":" << operations * options.rounds
            << ",\"lease_insertions\":" << operations * options.rounds << ",\"allocation_removals\":" << operations * options.rounds
            << ",\"lease_removals\":" << operations * options.rounds << "},\"round_records\":[";
        for (size_t r = 0; r < rounds.size(); ++r) {
            if (r) std::cout << ',';
            const auto &round = rounds[r];
            std::cout << "{\"round_wall_ns\":" << round.round_wall_ns
                << ",\"prepare_ns\":" << round.prepare_ns << ",\"close_ns\":" << round.close_ns
                << ",\"worker_loop_ns\":" << round.loop_ns << ",\"outer_wall_ns\":" << round.outer_wall_ns
                << ",\"worker_create_ready_ns\":" << round.create_ready_ns << ",\"checksum\":" << round.checksum
                << ",\"payload_bytes\":" << round.payload_bytes << ",\"provider_allocation_count_before_timed\":" << chunks
                << ",\"provider_allocation_count\":" << round.providers << ",\"peak_backing_bytes\":" << round.peak
                << ",\"allocation_count\":" << round.allocation_count << ",\"completed_operations\":" << operations << ",\"cycles\":[";
            for (size_t c = 0; c < round.cycles.size(); ++c) {
                if (c) std::cout << ',';
                const auto &cycle = round.cycles[c];
                std::cout << "{\"reset_ns\":" << cycle.reset_ns << ",\"hot_ns\":" << cycle.hot_ns
                    << ",\"checksum\":" << cycle.checksum << ",\"payload_bytes\":" << cycle.payload_bytes << ",\"phases_ns\":[";
                for (size_t phase = 0; phase < cycle.phases.size(); ++phase) {
                    if (phase) std::cout << ',';
                    std::cout << cycle.phases[phase];
                }
                std::cout << "]}";
            }
            std::cout << "]}";
        }
        std::cout << "],\"metrics\":{"; metric("hot_ns_per_op", hot); std::cout << ',';
        metric("reset_ns_per_cycle", reset); std::cout << ','; metric("worker_loop_ns_per_op", loops); std::cout << ',';
        metric("outer_wall_ns_per_op", outer); std::cout << ','; metric("round_wall_ns_per_op", round_wall); std::cout << ',';
        metric("prepare_ns", prepare); std::cout << ',';
        metric("close_ns", close); std::cout << ','; metric("worker_create_ready_ns", creation);
        for (size_t i = 0; i < names.size(); ++i) { std::cout << ','; metric(names[i].c_str(), phases[i]); }
        std::cout << ",\"hot_operations_per_second\":" << static_cast<double>(operations * options.rounds) * 1e9 / total_hot << "}}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"success\":false,\"performance_qualified\":false,\"error\":" << quote(error.what()) << "}\n";
        return 1;
    }
}
