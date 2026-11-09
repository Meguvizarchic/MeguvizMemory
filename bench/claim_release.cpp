#include <mgmem/mgmem.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
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
constexpr u64 count = 2048, payload_bytes = 64, alignment = 16, batch = 64;
constexpr u64 shuffle_seed = UINT64_C(0x9e3779b97f4a7c15);
struct Options { std::string order = "forward"; u64 domains = 1, threads = 1, rounds = 4, warmup = 1; };
void check(mgmem_status status, const char *operation) {
    if (status != MGMEM_OK) throw std::runtime_error(std::string(operation) + "_status_" + std::to_string(status));
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
    const int utf8_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (utf8_count <= 0) throw std::runtime_error("runtime_module_path_encoding");
    std::string result(static_cast<size_t>(utf8_count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length),
            result.data(), utf8_count, nullptr, nullptr) != utf8_count)
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

std::string hex(u64 value) {
    std::ostringstream stream; stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}
std::vector<u64> release_order(const Options &options) {
    std::vector<u64> order(static_cast<size_t>(count)); std::iota(order.begin(), order.end(), u64{0});
    if (options.order == "reverse") std::reverse(order.begin(), order.end());
    else if (options.order == "shuffled") {
        u64 state = shuffle_seed;
        for (u64 size = count; size > 1; --size) {
            state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
            const u64 index = (state * UINT64_C(2685821657736338717)) % size;
            std::swap(order[static_cast<size_t>(size - 1)], order[static_cast<size_t>(index)]);
        }
    }
    return order;
}
u64 order_hash(const std::vector<u64> &order) {
    u64 result = UINT64_C(14695981039346656037);
    for (u64 value : order) for (unsigned byte = 0; byte < 8; ++byte) {
        result ^= value & 255; result *= UINT64_C(1099511628211); value >>= 8;
    }
    return result;
}
struct Slot { mgmem_domain *domain = nullptr; mgmem_handle_v1 owner{}; };
struct alignas(64) Worker { std::vector<double> samples; std::string error; u64 released = 0; };
struct Round {
    u64 preparation_ns = 0, release_ns = 0, close_ns = 0, workflow_ns = 0;
    u64 checksum = 0, released = 0, backing_peak = 0, providers = 0;
    std::vector<double> samples;
    std::vector<u64> domain_peaks;
};
void require_empty(const mgmem_snapshot_v1 &state, bool closed) {
    if ((closed && state.state != MGMEM_CLOSED) || state.backing_bytes || state.reserved_bytes ||
            state.live_bytes || state.retiring_bytes || state.retained_bytes ||
            state.active_allocations || state.active_leases)
        throw std::runtime_error("resources_not_zero");
}
Round run_round(const Options &options, const std::vector<u64> &order) {
    const auto started = Clock::now();
    const u64 per_domain = count / options.domains;
    std::vector<mgmem_domain *> domains(static_cast<size_t>(options.domains), nullptr);
    std::vector<Slot> slots(static_cast<size_t>(count));
    std::vector<Worker> workers(static_cast<size_t>(options.threads));
    for (auto &worker : workers) worker.samples.reserve(static_cast<size_t>(count / options.threads / batch));
    mgmem_domain_options_v1 config{sizeof(config), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR,
        0, MGMEM_SYSTEM, 0, per_domain * payload_bytes, 0, nullptr};
    for (auto &domain : domains) check(mgmem_domain_create_v1(&config, &domain), "create");
    Round result;
    for (u64 index = 0; index < count; ++index) {
        auto &slot = slots[static_cast<size_t>(index)]; slot.domain = domains[static_cast<size_t>(index % options.domains)];
        check(mgmem_allocate_v1(slot.domain, payload_bytes, alignment, &slot.owner), "prepare_allocate");
        mgmem_borrow_v1 view{};
        check(mgmem_borrow_acquire_v1(slot.domain, slot.owner, 0, payload_bytes, MGMEM_WRITE, &view), "prepare_borrow");
        auto *data = static_cast<volatile unsigned char *>(view.data);
        if (!data || view.length != payload_bytes) throw std::runtime_error("prepared_view_mismatch");
        if (reinterpret_cast<uintptr_t>(data) % alignment) throw std::runtime_error("alignment_mismatch");
        for (u64 byte = 0; byte < payload_bytes; ++byte) data[byte] = static_cast<unsigned char>(index * 31 + 7 + byte);
        for (u64 byte = 0; byte < payload_bytes; ++byte) {
            if (data[byte] != static_cast<unsigned char>(index * 31 + 7 + byte)) throw std::runtime_error("payload_mismatch");
            result.checksum += data[byte];
        }
        check(mgmem_borrow_release_v1(slot.domain, view.lease), "prepare_release_lease");
    }
    for (auto *domain : domains) {
        mgmem_snapshot_v1 ready{}; check(mgmem_domain_snapshot_v1(domain, &ready, sizeof(ready)), "prepared_snapshot");
        if (ready.state != MGMEM_OPEN || ready.budget_bytes != config.budget_bytes ||
                ready.backing_bytes != config.budget_bytes || ready.live_bytes != config.budget_bytes ||
                ready.active_allocations != per_domain || ready.active_leases || ready.retiring_bytes ||
                ready.retained_bytes || ready.reserved_bytes || ready.provider_allocation_count != per_domain)
            throw std::runtime_error("prepared_resources_mismatch");
        result.backing_peak += ready.peak_backing_bytes; result.providers += ready.provider_allocation_count;
        result.domain_peaks.push_back(ready.peak_backing_bytes);
    }
    result.preparation_ns = ns(started, Clock::now());
    std::mutex gate; std::condition_variable changed; u64 ready = 0; bool go = false;
    std::vector<std::thread> threads; threads.reserve(static_cast<size_t>(options.threads));
    auto execute = [&](u64 worker_index) {
        auto &worker = workers[static_cast<size_t>(worker_index)];
        {
            std::unique_lock<std::mutex> lock(gate); ++ready; changed.notify_all();
            changed.wait(lock, [&] { return go; });
        }
        try {
            const u64 operations = count / options.threads;
            for (u64 first = 0; first < operations; first += batch) {
                const u64 size = std::min(batch, operations - first);
                const auto begin = Clock::now();
                for (u64 offset = 0; offset < size; ++offset) {
                    const u64 position = worker_index + (first + offset) * options.threads;
                    const auto &slot = slots[static_cast<size_t>(order[static_cast<size_t>(position)])];
                    check(mgmem_release_v1(slot.domain, slot.owner), "release_owner"); ++worker.released;
                }
                worker.samples.push_back(static_cast<double>(ns(begin, Clock::now())) / size);
            }
        } catch (const std::exception &error) { worker.error = error.what(); }
        catch (...) { worker.error = "unknown_worker_error"; }
    };
    try { for (u64 index = 0; index < options.threads; ++index) threads.emplace_back(execute, index); }
    catch (...) {
        { std::lock_guard<std::mutex> lock(gate); go = true; } changed.notify_all();
        for (auto &thread : threads) thread.join();
        throw;
    }
    Clock::time_point release_started;
    {
        std::unique_lock<std::mutex> lock(gate); changed.wait(lock, [&] { return ready == options.threads; });
        release_started = Clock::now(); go = true;
    }
    changed.notify_all(); for (auto &thread : threads) thread.join();
    result.release_ns = ns(release_started, Clock::now());
    for (const auto &worker : workers) {
        if (!worker.error.empty()) throw std::runtime_error(worker.error);
        result.released += worker.released; result.samples.insert(result.samples.end(), worker.samples.begin(), worker.samples.end());
    }
    if (result.released != count) throw std::runtime_error("incomplete_release");
    for (auto *domain : domains) {
        mgmem_snapshot_v1 empty{}; check(mgmem_domain_snapshot_v1(domain, &empty, sizeof(empty)), "released_snapshot");
        require_empty(empty, false);
    }
    const auto closing = Clock::now();
    for (auto *domain : domains) {
        check(mgmem_domain_begin_close_v1(domain), "begin_close"); check(mgmem_domain_collect_v1(domain), "collect");
        check(mgmem_domain_drain_v1(domain, 0), "drain");
        mgmem_snapshot_v1 empty{}; check(mgmem_domain_snapshot_v1(domain, &empty, sizeof(empty)), "closed_snapshot");
        require_empty(empty, true); check(mgmem_domain_destroy_v1(domain), "destroy");
    }
    const auto finished = Clock::now(); result.close_ns = ns(closing, finished); result.workflow_ns = ns(started, finished);
    return result;
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end()); return values[static_cast<size_t>((values.size() - 1) * fraction + 0.5)];
}
}
int main(int argc, char **argv) {
    try {
        Options options;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 == argc) throw std::runtime_error("missing_option_value");
            const std::string key = argv[index], value = argv[index + 1];
            if (key == "--order") options.order = value;
            else if (key == "--domains") options.domains = number(value);
            else if (key == "--threads") options.threads = number(value);
            else if (key == "--rounds") options.rounds = number(value);
            else if (key == "--warmup") options.warmup = number(value);
            else if (key == "--count") { if (number(value) != count) throw std::runtime_error("fixed_count_required"); }
            else throw std::runtime_error("unknown_option");
        }
        if ((options.order != "forward" && options.order != "reverse" && options.order != "shuffled") ||
                (options.domains != 1 && options.domains != 32) || (options.threads != 1 && options.threads != 4) ||
                options.rounds < 2 || options.rounds > 64 || options.warmup > 16)
            throw std::runtime_error("workload_out_of_bounds");
        const auto planned = release_order(options); const auto module = module_path();
        for (u64 round = 0; round < options.warmup; ++round) (void)run_round(options, planned);
        std::vector<Round> rounds; std::vector<double> samples, release_samples, workflow_samples;
        u64 checksum = 0, total_release = 0, total_workflow = 0;
        for (u64 round = 0; round < options.rounds; ++round) {
            rounds.push_back(run_round(options, planned)); const auto &value = rounds.back();
            checksum += value.checksum; total_release += value.release_ns; total_workflow += value.workflow_ns;
            samples.insert(samples.end(), value.samples.begin(), value.samples.end());
            release_samples.push_back(static_cast<double>(value.release_ns) / count);
            workflow_samples.push_back(static_cast<double>(value.workflow_ns) / count);
        }
        std::cout << std::setprecision(15) << "{\"schema_version\":1,\"workload\":\"system_claim_release_v1\",\"success\":true,"
            << "\"scope\":\"SYNTHETIC_SYSTEM_RELEASE_AND_FULL_ROUND_NOT_PRODUCT\",\"runtime_module\":" << quote(module)
            << ",\"abi\":" << mgmem_abi_version_v1() << ",\"count\":" << count << ",\"bytes_per_allocation\":" << payload_bytes
            << ",\"alignment\":" << alignment << ",\"batch\":" << batch << ",\"domains\":" << options.domains
            << ",\"domain_budget_bytes\":" << (count / options.domains * payload_bytes) << ",\"threads\":" << options.threads
            << ",\"order\":" << quote(options.order) << ",\"order_hash_fnv64\":" << quote(hex(order_hash(planned)))
            << ",\"shuffle_seed\":" << quote(hex(shuffle_seed)) << ",\"thread_assignment\":\"strided_permutation_positions\""
            << ",\"rounds\":" << options.rounds << ",\"warmup_rounds\":" << options.warmup
            << ",\"released_operations\":" << count * options.rounds << ",\"checksum\":" << checksum
            << ",\"release_batch_ns_per_op_p50\":" << percentile(samples, .5) << ",\"release_batch_ns_per_op_p95\":" << percentile(samples, .95)
            << ",\"release_phase_ns_per_op_p50\":" << percentile(release_samples, .5) << ",\"release_phase_ns_per_op_p95\":" << percentile(release_samples, .95)
            << ",\"workflow_round_ns_per_op_p50\":" << percentile(workflow_samples, .5) << ",\"workflow_round_ns_per_op_p95\":" << percentile(workflow_samples, .95)
            << ",\"release_operations_per_second\":" << static_cast<double>(count * options.rounds) * 1e9 / total_release
            << ",\"workflow_operations_per_second\":" << static_cast<double>(count * options.rounds) * 1e9 / total_workflow
            << ",\"metadata_bytes\":null,\"rss_pss\":null"
            << ",\"prepared_resources\":\"N_OWNERS_NO_LEASES\",\"after_release_resources\":\"ZERO_BEFORE_CLOSE\""
            << ",\"final_resources\":\"CLOSED_ZERO\",\"round_records\":[";
        for (size_t index = 0; index < rounds.size(); ++index) {
            if (index) std::cout << ','; const auto &round = rounds[index];
            std::cout << "{\"preparation_ns\":" << round.preparation_ns << ",\"release_phase_ns\":" << round.release_ns
                << ",\"close_ns\":" << round.close_ns << ",\"workflow_ns\":" << round.workflow_ns
                << ",\"released_operations\":" << round.released << ",\"checksum\":" << round.checksum
                << ",\"prepared_allocations\":" << count << ",\"prepared_leases\":0,\"prepared_backing_bytes\":" << round.backing_peak
                << ",\"peak_backing_bytes\":" << round.backing_peak << ",\"provider_allocation_count\":" << round.providers
                << ",\"backing_after_release\":0,\"owners_after_release\":0,\"leases_after_release\":0,\"reserved_after_release\":0"
                << ",\"final_resources\":\"CLOSED_ZERO\",\"domain_peak_backing_bytes\":[";
            for (size_t domain = 0; domain < round.domain_peaks.size(); ++domain) { if (domain) std::cout << ','; std::cout << round.domain_peaks[domain]; }
            std::cout << "],\"release_batch_ns_per_op\":[";
            for (size_t sample = 0; sample < round.samples.size(); ++sample) { if (sample) std::cout << ','; std::cout << round.samples[sample]; }
            std::cout << "]}";
        }
        std::cout << "],\"performance_qualified\":false}\n"; return 0;
    } catch (const std::exception &error) {
        std::cout << "{\"schema_version\":1,\"success\":false,\"error\":" << quote(error.what()) << ",\"performance_qualified\":false}\n"; return 1;
    }
}
