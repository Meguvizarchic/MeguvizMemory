#include <mgmem/mgmem.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace probe {
using u64 = uint64_t;
enum Event { Create, Allocate, Acquire, ReleaseLease, ReleaseOwner, Reset,
             BeginClose, Collect, Drain, Destroy, Snapshot, NativeCount };
constexpr const char *event_names[] = {"create", "allocate", "acquire", "release_lease",
    "release_owner", "reset", "begin_close", "collect", "drain", "destroy", "snapshot"};
struct Counts {
    std::array<u64, NativeCount> native{};
    u64 cpp_requests = 0, cpp_requested_bytes = 0;
    u64 provider_allocate = 0, provider_deallocate = 0, provider_retain = 0, provider_release = 0;
};
struct Config {
    uint32_t struct_size = 0, abi_major = 0, abi_minor = 0, flags = 0, mode = 0, reserved = 0;
    u64 budget = 0, chunk = 0;
    bool provider = false;
};
Counts counts;
Config last_config;
bool count_active = false;
inline void event(Event value) noexcept {
#if MGMEM_PROBE_COUNTS
    if (count_active) ++counts.native[value];
#else
    (void)value;
#endif
}
inline void allocation(size_t bytes) noexcept {
#if MGMEM_PROBE_COUNTS
    if (count_active) { ++counts.cpp_requests; counts.cpp_requested_bytes += bytes; }
#else
    (void)bytes;
#endif
}
inline mgmem_status create(const mgmem_domain_options_v1 *o, mgmem_domain **out) {
    event(Create);
    if (o) last_config = {o->struct_size, o->abi_major, o->abi_minor, o->required_flags,
        o->mode, o->reserved, o->budget_bytes, o->arena_chunk_bytes, o->provider != nullptr};
    return mgmem_domain_create_v1(o, out);
}
inline mgmem_status allocate(mgmem_domain *d, u64 b, u64 a, mgmem_handle_v1 *out) {
    event(Allocate); return mgmem_allocate_v1(d, b, a, out);
}
inline mgmem_status acquire(mgmem_domain *d, mgmem_handle_v1 h, u64 o, u64 l, uint32_t a, mgmem_borrow_v1 *out) {
    event(Acquire); return mgmem_borrow_acquire_v1(d, h, o, l, a, out);
}
inline mgmem_status release_lease(mgmem_domain *d, mgmem_handle_v1 h) {
    event(ReleaseLease); return mgmem_borrow_release_v1(d, h);
}
inline mgmem_status release_owner(mgmem_domain *d, mgmem_handle_v1 h) {
    event(ReleaseOwner); return mgmem_release_v1(d, h);
}
inline mgmem_status reset(mgmem_domain *d) { event(Reset); return mgmem_arena_reset_v1(d); }
inline mgmem_status begin_close(mgmem_domain *d) { event(BeginClose); return mgmem_domain_begin_close_v1(d); }
inline mgmem_status collect(mgmem_domain *d) { event(Collect); return mgmem_domain_collect_v1(d); }
inline mgmem_status drain(mgmem_domain *d, u64 t) { event(Drain); return mgmem_domain_drain_v1(d, t); }
inline mgmem_status destroy(mgmem_domain *d) { event(Destroy); return mgmem_domain_destroy_v1(d); }
inline mgmem_status snapshot(mgmem_domain *d, mgmem_snapshot_v1 *o, uint32_t n) {
    event(Snapshot); return mgmem_domain_snapshot_v1(d, o, n);
}
}

#if MGMEM_PROBE_COUNTS
void *operator new(size_t bytes) {
    for (;;) {
        if (void *p = std::malloc(bytes ? bytes : 1)) { probe::allocation(bytes); return p; }
        auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void *operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
void *operator new(size_t bytes, std::align_val_t alignment) {
    for (;;) {
        void *p = nullptr;
#if defined(_WIN32)
        p = _aligned_malloc(bytes ? bytes : 1, static_cast<size_t>(alignment));
#else
        if (posix_memalign(&p, static_cast<size_t>(alignment), bytes ? bytes : 1)) p = nullptr;
#endif
        if (p) { probe::allocation(bytes); return p; }
        auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void *operator new[](size_t b, std::align_val_t a) { return ::operator new(b, a); }
void operator delete(void *p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void *p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void *p, size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void *p, size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

// Only header-originating calls are redirected. Forwarders above call the real core.
#define mgmem_domain_create_v1 probe::create
#define mgmem_allocate_v1 probe::allocate
#define mgmem_borrow_acquire_v1 probe::acquire
#define mgmem_borrow_release_v1 probe::release_lease
#define mgmem_release_v1 probe::release_owner
#define mgmem_arena_reset_v1 probe::reset
#define mgmem_domain_begin_close_v1 probe::begin_close
#define mgmem_domain_collect_v1 probe::collect
#define mgmem_domain_drain_v1 probe::drain
#define mgmem_domain_destroy_v1 probe::destroy
#define mgmem_domain_snapshot_v1 probe::snapshot
#endif
#include <mgmem/memory.hpp>
#if MGMEM_PROBE_COUNTS
#undef mgmem_domain_create_v1
#undef mgmem_allocate_v1
#undef mgmem_borrow_acquire_v1
#undef mgmem_borrow_release_v1
#undef mgmem_release_v1
#undef mgmem_arena_reset_v1
#undef mgmem_domain_begin_close_v1
#undef mgmem_domain_collect_v1
#undef mgmem_domain_drain_v1
#undef mgmem_domain_destroy_v1
#undef mgmem_domain_snapshot_v1
#endif

namespace probe {
using Clock = std::chrono::steady_clock;
void check(bool condition, const char *text) { if (!condition) throw std::runtime_error(text); }
void status(mgmem_status value, const char *text) { check(value == MGMEM_OK, text); }
u64 ns(Clock::time_point begin) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}
struct Provider {
    u64 allocations = 0, deallocations = 0, retains = 0, releases = 0, active = 0, bytes = 0;
    static mgmem_status allocate(void *context, u64 bytes, u64 alignment, void **out) {
        auto &self = *static_cast<Provider *>(context);
        *out = nullptr;
#if defined(_WIN32)
        *out = _aligned_malloc(static_cast<size_t>(bytes), static_cast<size_t>(alignment));
#else
        if (posix_memalign(out, static_cast<size_t>(alignment), static_cast<size_t>(bytes))) *out = nullptr;
#endif
        if (!*out) return MGMEM_OUT_OF_MEMORY;
        ++self.allocations; ++self.active; self.bytes += bytes;
#if MGMEM_PROBE_COUNTS
        if (count_active) ++counts.provider_allocate;
#endif
        return MGMEM_OK;
    }
    static void deallocate(void *context, void *p, u64 bytes, u64) {
        auto &self = *static_cast<Provider *>(context);
        ++self.deallocations; --self.active; self.bytes -= bytes;
#if MGMEM_PROBE_COUNTS
        if (count_active) ++counts.provider_deallocate;
#endif
#if defined(_WIN32)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }
    static void retain(void *context) {
        ++static_cast<Provider *>(context)->retains;
#if MGMEM_PROBE_COUNTS
        if (count_active) ++counts.provider_retain;
#endif
    }
    static void release(void *context) {
        ++static_cast<Provider *>(context)->releases;
#if MGMEM_PROBE_COUNTS
        if (count_active) ++counts.provider_release;
#endif
    }
    mgmem_provider_v1 table() {
        return {sizeof(mgmem_provider_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
                this, allocate, deallocate, retain, release};
    }
};
struct Options {
    std::string method = "manual", workload = "scoped", domain = "arena", range = "whole";
    u64 cycles = 256, rounds = 12, warmup = 4;
};
struct Row {
    u64 hot_ns = 0, wall_ns = 0, checksum = 0, operations = 0, payload_bytes = 0;
    u64 provider_allocations = 0, provider_deallocations = 0;
    Counts total{}, hot{};
    Config config{};
};
Counts difference(Counts end, const Counts &begin) {
    for (size_t i = 0; i < end.native.size(); ++i) end.native[i] -= begin.native[i];
    end.cpp_requests -= begin.cpp_requests; end.cpp_requested_bytes -= begin.cpp_requested_bytes;
    end.provider_allocate -= begin.provider_allocate; end.provider_deallocate -= begin.provider_deallocate;
    end.provider_retain -= begin.provider_retain; end.provider_release -= begin.provider_release;
    return end;
}
mgmem_domain_options_v1 manual_options(uint32_t mode, const mgmem_provider_v1 *provider) {
    return {sizeof(mgmem_domain_options_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
        mode, 0, 4096, mode == MGMEM_ARENA ? 4096ull : 0ull, provider};
}
template<bool Helper>
mgmem_status create_domain(uint32_t mode, const mgmem_provider_v1 *provider, mgmem::domain &out) {
    if constexpr (Helper) {
        return mode == MGMEM_ARENA ? mgmem::domain::create_arena(4096, 4096, out, provider)
                                    : mgmem::domain::create_system(4096, out, provider);
    } else return mgmem::domain::create(manual_options(mode, provider), out);
}
template<class Body>
mgmem_status manual_scoped(const mgmem::domain &domain, u64 bytes, Body &&body) {
    mgmem::owned_allocation owner;
    auto result = domain.allocate(bytes, alignof(std::max_align_t), owner);
    if (result != MGMEM_OK) return result;
    mgmem::borrow view;
    result = owner.acquire_borrow(0, bytes, MGMEM_WRITE, view);
    if (result != MGMEM_OK) return result;
    return body(view.write_data(), view.size_bytes());
}
template<bool Helper>
Row execute(const Options &o) {
    const uint32_t mode = o.domain == "arena" ? MGMEM_ARENA : MGMEM_SYSTEM;
    Provider provider;
    auto table = provider.table();
    Row row;
    counts = {}; count_active = true;
    const auto wall_begin = Clock::now();
    {
        mgmem::domain domain;
        status(create_domain<Helper>(mode, &table, domain), "create");
        if (o.workload == "cold" || o.workload == "scoped") {
            const Counts before = counts;
            const auto hot_begin = Clock::now();
            for (u64 i = 0; i < o.cycles; ++i) {
                auto body = [&](void *p, u64 length) -> mgmem_status {
                    check(p && length == 64, "scoped_view");
                    auto *data = static_cast<volatile unsigned char *>(p);
                    for (u64 b = 0; b < length; ++b) data[b] = static_cast<unsigned char>((i * 17 + b * 3 + 11) & 255);
                    for (u64 b = 0; b < length; ++b) row.checksum += data[b];
                    row.payload_bytes += length;
                    return MGMEM_OK;
                };
                if constexpr (Helper) status(domain.with_allocation(64, body), "helper_scoped");
                else status(manual_scoped(domain, 64, body), "manual_scoped");
                if (mode == MGMEM_ARENA && o.workload != "cold") status(domain.reset(), "reset");
            }
            row.hot_ns = ns(hot_begin); row.hot = difference(counts, before);
        } else {
            mgmem::owned_allocation owner;
            if constexpr (Helper) status(domain.allocate(128, owner), "helper_allocate");
            else status(domain.allocate(128, alignof(std::max_align_t), owner), "manual_allocate");
            {
                mgmem::borrow initial;
                status(owner.acquire_borrow(0, 128, MGMEM_WRITE, initial), "initial_write");
                auto *data = static_cast<unsigned char *>(initial.write_data());
                for (u64 b = 0; b < 128; ++b) data[b] = static_cast<unsigned char>((b * 3 + 11) & 255);
            }
            const u64 offset = o.range == "sub" ? 32 : 0;
            const u64 length = o.range == "sub" ? 64 : 128;
            const Counts before = counts;
            const auto hot_begin = Clock::now();
            for (u64 i = 0; i < o.cycles; ++i) {
                auto read = [&](const void *p, u64 bytes) -> mgmem_status {
                    check(p && bytes == length, "read_view");
                    const auto *data = static_cast<const volatile unsigned char *>(p);
                    for (u64 b = 0; b < bytes; ++b) row.checksum += data[b];
                    row.payload_bytes += bytes; return MGMEM_OK;
                };
                auto write = [&](void *p, u64 bytes) -> mgmem_status {
                    check(p && bytes == length, "write_view");
                    auto *data = static_cast<volatile unsigned char *>(p);
                    for (u64 b = 0; b < bytes; ++b) data[b] = static_cast<unsigned char>((i * 17 + (b + offset) * 3 + 11) & 255);
                    for (u64 b = 0; b < bytes; ++b) row.checksum += data[b];
                    row.payload_bytes += bytes; return MGMEM_OK;
                };
                if constexpr (Helper) {
                    if (o.workload == "read") {
                        status(o.range == "sub" ? owner.with_read(offset, length, read) : owner.with_read(read), "helper_read");
                    } else {
                        status(o.range == "sub" ? owner.with_write(offset, length, write) : owner.with_write(write), "helper_write");
                    }
                } else {
                    mgmem::borrow view;
                    status(owner.acquire_borrow(offset, length, o.workload == "read" ? MGMEM_READ : MGMEM_WRITE, view), "manual_borrow");
                    status(o.workload == "read" ? read(view.data(), view.size_bytes()) : write(view.write_data(), view.size_bytes()), "manual_body");
                    status(view.release(), "manual_release");
                }
            }
            row.hot_ns = ns(hot_begin); row.hot = difference(counts, before);
        }
        mgmem_snapshot_v1 final{};
        status(domain.snapshot(final), "final_snapshot");
        check(final.active_allocations == 0 && final.active_leases == 0 && final.live_bytes == 0 &&
              final.retiring_bytes == 0 && final.reserved_bytes == 0, "active_resources");
        row.operations = o.cycles;
    }
    row.wall_ns = ns(wall_begin); row.total = counts; row.config = last_config;
    count_active = false;
    check(provider.active == 0 && provider.bytes == 0 && provider.allocations == provider.deallocations &&
          provider.retains == 1 && provider.releases == 1, "provider_pairing");
    row.provider_allocations = provider.allocations; row.provider_deallocations = provider.deallocations;
    return row;
}
void print_counts(const Counts &c) {
    std::printf("{\"native\":{");
    for (size_t i = 0; i < c.native.size(); ++i)
        std::printf("%s\"%s\":%llu", i ? "," : "", event_names[i], static_cast<unsigned long long>(c.native[i]));
    std::printf("},\"cpp_requests\":%llu,\"cpp_requested_bytes\":%llu,\"provider_allocate\":%llu,\"provider_deallocate\":%llu,\"provider_retain\":%llu,\"provider_release\":%llu}",
        static_cast<unsigned long long>(c.cpp_requests), static_cast<unsigned long long>(c.cpp_requested_bytes),
        static_cast<unsigned long long>(c.provider_allocate), static_cast<unsigned long long>(c.provider_deallocate),
        static_cast<unsigned long long>(c.provider_retain), static_cast<unsigned long long>(c.provider_release));
}
std::string runtime_module() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&mgmem_allocate_v1), &module) != 0, "runtime_module");
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    check(length && length < path.size(), "runtime_path");
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    check(bytes > 0, "runtime_path_encoding");
    std::string result(static_cast<size_t>(bytes), '\0');
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length),
        result.data(), bytes, nullptr, nullptr) == bytes, "runtime_path_encoding");
    return result;
#else
    Dl_info observed{};
    check(dladdr(reinterpret_cast<void *>(&mgmem_allocate_v1), &observed) && observed.dli_fname, "runtime_path");
    return observed.dli_fname;
#endif
}
void quote(const std::string &text) {
    std::putchar('"');
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { std::putchar('\\'); std::putchar(ch); }
        else if (ch < 32) std::printf("\\u%04x", static_cast<unsigned>(ch));
        else std::putchar(ch);
    }
    std::putchar('"');
}
}

int main(int argc, char **argv) {
    try {
        probe::Options o;
        for (int i = 1; i < argc; i += 2) {
            probe::check(i + 1 < argc, "missing_option");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--method") o.method = value;
            else if (key == "--case") o.workload = value;
            else if (key == "--domain") o.domain = value;
            else if (key == "--range") o.range = value;
            else if (key == "--cycles") o.cycles = std::stoull(value);
            else if (key == "--rounds") o.rounds = std::stoull(value);
            else if (key == "--warmup") o.warmup = std::stoull(value);
            else throw std::runtime_error("unknown_option");
        }
        probe::check(o.method == "manual" || o.method == "helper", "method");
        probe::check(o.domain == "system" || o.domain == "arena", "domain");
        probe::check(o.workload == "cold" || o.workload == "scoped" || o.workload == "read" || o.workload == "write", "case");
        probe::check(o.range == "whole" || o.range == "sub", "range");
        probe::check(o.cycles > 0 && o.cycles <= 100000 && o.rounds > 0 && o.rounds <= 100 && o.warmup <= 100, "bounds");
        probe::check(o.workload != "cold" || o.cycles == 1, "cold_one_operation");
        std::vector<probe::Row> rows(static_cast<size_t>(o.rounds));
        for (probe::u64 i = 0; i < o.warmup + o.rounds; ++i) {
            auto row = o.method == "helper" ? probe::execute<true>(o) : probe::execute<false>(o);
            if (i >= o.warmup) rows[static_cast<size_t>(i - o.warmup)] = row;
        }
        std::printf("{\"schema_version\":1,\"success\":true,\"performance_qualified\":false,\"count_only\":%s,\"method\":\"%s\",\"case\":\"%s\",\"domain\":\"%s\",\"range\":\"%s\",\"cycles\":%llu,\"rounds\":%llu,\"warmup\":%llu,\"final_resources\":\"CLOSED_ZERO\",\"count_scope\":\"HEADER_NATIVE_CALLS_PROVIDER_EVENTS_AND_THIS_EXECUTABLE_CPP_NEW_NOT_ALL_DLL_HEAP\",\"round_records\":[",
#if MGMEM_PROBE_COUNTS
            "true",
#else
            "false",
#endif
            o.method.c_str(), o.workload.c_str(), o.domain.c_str(), o.range.c_str(),
            static_cast<unsigned long long>(o.cycles), static_cast<unsigned long long>(o.rounds), static_cast<unsigned long long>(o.warmup));
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto &r = rows[i];
            std::printf("%s{\"hot_ns\":%llu,\"wall_ns\":%llu,\"checksum\":%llu,\"operations\":%llu,\"payload_bytes\":%llu,\"provider_allocations\":%llu,\"provider_deallocations\":%llu,\"config\":{\"struct_size\":%u,\"abi_major\":%u,\"abi_minor\":%u,\"flags\":%u,\"mode\":%u,\"reserved\":%u,\"budget\":%llu,\"chunk\":%llu,\"provider\":%s},\"total_counts\":",
                i ? "," : "", static_cast<unsigned long long>(r.hot_ns), static_cast<unsigned long long>(r.wall_ns),
                static_cast<unsigned long long>(r.checksum), static_cast<unsigned long long>(r.operations), static_cast<unsigned long long>(r.payload_bytes),
                static_cast<unsigned long long>(r.provider_allocations), static_cast<unsigned long long>(r.provider_deallocations),
                r.config.struct_size, r.config.abi_major, r.config.abi_minor, r.config.flags, r.config.mode, r.config.reserved,
                static_cast<unsigned long long>(r.config.budget), static_cast<unsigned long long>(r.config.chunk), r.config.provider ? "true" : "false");
            probe::print_counts(r.total); std::printf(",\"hot_counts\":"); probe::print_counts(r.hot); std::printf("}");
        }
        std::printf("],\"runtime_module\":"); probe::quote(probe::runtime_module());
        std::printf("}\n"); return 0;
    } catch (const std::exception &e) {
        probe::count_active = false;
        std::fprintf(stderr, "human_api_probe_failed:%s\n", e.what());
        return 1;
    }
}
