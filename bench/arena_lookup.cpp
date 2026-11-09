#include <mgmem/mgmem.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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
struct Options {
    std::string shape = "fixed";
    u64 threads = 1, allocations = 4096, batch = 64, rounds = 6, warmup = 2;
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
    return options.shape == "fixed" ? 64 : (operation % 4 + 1) * 32;
}
u64 alignment_for(const Options &options, u64 operation) {
    return options.shape == "fixed" ? 16 : u64{8} << (operation % 4);
}
struct Slot {
    mgmem_handle_v1 owner{};
    mgmem_borrow_v1 view{};
    bool owner_live = false, view_live = false;
    u64 operation = 0, bytes = 0;
};
struct Worker {
    std::vector<Slot> slots;
    std::vector<double> allocation_ns_per_op;
    std::string error;
    u64 checksum = 0, completed = 0;
    explicit Worker(u64 batch) : slots(static_cast<size_t>(batch)) {}
};
struct Round {
    u64 workflow_ns = 0, close_ns = 0, checksum = 0, completed = 0, backing_peak = 0, providers = 0;
    std::vector<double> allocation_ns_per_op;
};
Round run_round(const Options &options) {
    const u64 total = options.allocations * options.threads;
    mgmem_domain_options_v1 config{sizeof(config), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR,
        0, MGMEM_ARENA, 0, total * 512 + chunk_bytes, chunk_bytes, nullptr};
    mgmem_domain *domain = nullptr;
    check(mgmem_domain_create_v1(&config, &domain), "create");
    std::vector<Worker> workers;
    workers.reserve(static_cast<size_t>(options.threads));
    for (u64 index = 0; index < options.threads; ++index) {
        workers.emplace_back(options.batch);
        workers.back().allocation_ns_per_op.reserve(
            static_cast<size_t>((options.allocations + options.batch - 1) / options.batch));
    }
    std::mutex gate;
    std::condition_variable changed;
    u64 ready = 0;
    bool go = false;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(options.threads));
    auto execute = [&](u64 worker_index) {
        auto &worker = workers[static_cast<size_t>(worker_index)];
        {
            std::unique_lock<std::mutex> lock(gate);
            ++ready; changed.notify_all();
            changed.wait(lock, [&] { return go; });
        }
        try {
            for (u64 first = 0; first < options.allocations; first += options.batch) {
                const u64 count = std::min(options.batch, options.allocations - first);
                const auto started = Clock::now();
                for (u64 index = 0; index < count; ++index) {
                    auto &slot = worker.slots[static_cast<size_t>(index)];
                    slot.operation = first + index;
                    slot.bytes = size_for(options, slot.operation);
                    check(mgmem_allocate_v1(domain, slot.bytes, alignment_for(options, slot.operation),
                        &slot.owner), "allocate");
                    slot.owner_live = true;
                }
                const auto allocated = Clock::now();
                worker.allocation_ns_per_op.push_back(static_cast<double>(ns(started, allocated)) / count);
                for (u64 index = 0; index < count; ++index) {
                    auto &slot = worker.slots[static_cast<size_t>(index)];
                    check(mgmem_borrow_acquire_v1(domain, slot.owner, 0, slot.bytes,
                        MGMEM_WRITE, &slot.view), "borrow");
                    slot.view_live = true;
                    auto *bytes = static_cast<volatile unsigned char *>(slot.view.data);
                    if (reinterpret_cast<uintptr_t>(bytes) % alignment_for(options, slot.operation))
                        throw std::runtime_error("alignment_mismatch");
                    const u64 seed = worker_index * 17 + slot.operation * 31;
                    for (u64 byte = 0; byte < slot.bytes; ++byte)
                        bytes[byte] = static_cast<unsigned char>(seed + byte);
                    for (u64 byte = 0; byte < slot.bytes; ++byte) {
                        if (bytes[byte] != static_cast<unsigned char>(seed + byte))
                            throw std::runtime_error("payload_mismatch");
                        worker.checksum += bytes[byte];
                    }
                    // Keep the lease alive across owner release, as in asynchronous consumers.
                    slot.owner_live = false;
                    check(mgmem_release_v1(domain, slot.owner), "release_owner");
                    slot.view_live = false;
                    check(mgmem_borrow_release_v1(domain, slot.view.lease), "release_lease");
                    ++worker.completed;
                }
            }
        } catch (const std::exception &error) { worker.error = error.what(); }
        catch (...) { worker.error = "unknown_worker_error"; }
    };
    try {
        for (u64 index = 0; index < options.threads; ++index) threads.emplace_back(execute, index);
    } catch (...) {
        { std::lock_guard<std::mutex> lock(gate); go = true; }
        changed.notify_all();
        for (auto &thread : threads) thread.join();
        // No external provider is used. Failed process results never qualify performance.
        (void)mgmem_domain_begin_close_v1(domain);
        throw;
    }
    Clock::time_point started;
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return ready == options.threads; });
        started = Clock::now();
        go = true;
    }
    changed.notify_all();
    for (auto &thread : threads) thread.join();
    const auto finished = Clock::now();
    Round result;
    result.workflow_ns = ns(started, finished);
    for (const auto &worker : workers) {
        if (!worker.error.empty()) throw std::runtime_error(worker.error);
        result.checksum += worker.checksum;
        result.completed += worker.completed;
        result.allocation_ns_per_op.insert(result.allocation_ns_per_op.end(),
            worker.allocation_ns_per_op.begin(), worker.allocation_ns_per_op.end());
    }
    if (result.completed != total) throw std::runtime_error("incomplete_work");
    mgmem_snapshot_v1 before{};
    check(mgmem_domain_snapshot_v1(domain, &before, sizeof(before)), "before_close_snapshot");
    if (before.active_allocations || before.active_leases || before.live_bytes ||
            before.retiring_bytes || before.reserved_bytes)
        throw std::runtime_error("before_close_resources_nonzero");
    result.backing_peak = before.peak_backing_bytes;
    result.providers = before.provider_allocation_count;
    const auto closing = Clock::now();
    check(mgmem_domain_begin_close_v1(domain), "begin_close");
    check(mgmem_domain_collect_v1(domain), "collect");
    check(mgmem_domain_drain_v1(domain, 0), "drain");
    mgmem_snapshot_v1 after{};
    check(mgmem_domain_snapshot_v1(domain, &after, sizeof(after)), "after_close_snapshot");
    if (after.state != MGMEM_CLOSED || after.backing_bytes || after.reserved_bytes ||
            after.live_bytes || after.retiring_bytes || after.active_allocations || after.active_leases)
        throw std::runtime_error("closed_resources_nonzero");
    check(mgmem_domain_destroy_v1(domain), "destroy");
    result.close_ns = ns(closing, Clock::now());
    return result;
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>((values.size() - 1) * fraction + 0.5);
    return values[index];
}
}
int main(int argc, char **argv) {
    try {
        Options options;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 == argc) throw std::runtime_error("missing_option_value");
            const std::string key = argv[index], value = argv[index + 1];
            if (key == "--shape") options.shape = value;
            else if (key == "--threads") options.threads = number(value);
            else if (key == "--allocations") options.allocations = number(value);
            else if (key == "--batch") options.batch = number(value);
            else if (key == "--rounds") options.rounds = number(value);
            else if (key == "--warmup") options.warmup = number(value);
            else throw std::runtime_error("unknown_option");
        }
        if ((options.shape != "fixed" && options.shape != "mixed") || !options.threads ||
                options.threads > 16 || options.allocations < 128 || options.allocations > 65536 ||
                options.allocations * options.threads > 262144 || !options.batch || options.batch > 256 ||
                options.rounds < 2 || options.rounds > 64 || options.warmup > 16 ||
                ((options.allocations + options.batch - 1) / options.batch) * options.threads * options.rounds > 262144)
            throw std::runtime_error("workload_out_of_bounds");
        const auto module = module_path();
        for (u64 round = 0; round < options.warmup; ++round) (void)run_round(options);
        std::vector<Round> rounds;
        std::vector<double> allocation_samples, workflow_samples;
        u64 checksum = 0, total_ns = 0;
        for (u64 round = 0; round < options.rounds; ++round) {
            rounds.push_back(run_round(options));
            const auto &result = rounds.back();
            checksum += result.checksum; total_ns += result.workflow_ns;
            allocation_samples.insert(allocation_samples.end(), result.allocation_ns_per_op.begin(),
                result.allocation_ns_per_op.end());
            workflow_samples.push_back(static_cast<double>(result.workflow_ns) / result.completed);
        }
        const u64 operations = options.allocations * options.threads * options.rounds;
        std::cout << std::setprecision(15) << "{\"schema_version\":1,\"workload\":\"arena_lookup_composite_v1\","
            << "\"success\":true,\"scope\":\"SYNTHETIC_SHARED_DOMAIN_API_NOT_PRODUCT\","
            << "\"runtime_module\":" << quote(module) << ",\"abi\":" << mgmem_abi_version_v1()
            << ",\"shape\":" << quote(options.shape) << ",\"threads\":" << options.threads
            << ",\"allocations_per_thread\":" << options.allocations << ",\"batch\":" << options.batch
            << ",\"rounds\":" << options.rounds << ",\"warmup_rounds\":" << options.warmup
            << ",\"chunk_bytes\":" << chunk_bytes << ",\"completed_operations\":" << operations
            << ",\"checksum\":" << checksum << ",\"allocation_batch_ns_per_op_p50\":" << percentile(allocation_samples, .5)
            << ",\"allocation_batch_ns_per_op_p95\":" << percentile(allocation_samples, .95)
            << ",\"workflow_round_ns_per_op_p50\":" << percentile(workflow_samples, .5)
            << ",\"workflow_round_ns_per_op_p95\":" << percentile(workflow_samples, .95)
            << ",\"workflow_operations_per_second\":" << static_cast<double>(operations) * 1e9 / total_ns
            << ",\"final_resources\":\"CLOSED_ZERO\",\"round_records\":[";
        for (size_t index = 0; index < rounds.size(); ++index) {
            if (index) std::cout << ',';
            const auto &round = rounds[index];
            std::cout << "{\"workflow_ns\":" << round.workflow_ns << ",\"close_ns\":" << round.close_ns
                << ",\"completed_operations\":" << round.completed << ",\"checksum\":" << round.checksum
                << ",\"peak_backing_bytes\":" << round.backing_peak << ",\"provider_allocation_count\":" << round.providers
                << ",\"allocation_batch_ns_per_op\":[";
            for (size_t sample = 0; sample < round.allocation_ns_per_op.size(); ++sample) {
                if (sample) std::cout << ',';
                std::cout << round.allocation_ns_per_op[sample];
            }
            std::cout << "]}";
        }
        std::cout << "],\"performance_qualified\":false}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "{\"schema_version\":1,\"success\":false,\"error\":" << quote(error.what())
            << ",\"performance_qualified\":false}\n";
        return 1;
    }
}

