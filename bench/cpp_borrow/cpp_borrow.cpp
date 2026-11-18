// Empty-borrow cost: C API vs the optional C++ header on identical work.
// Owners are prepared before timing; the loop only acquires and releases a
// READ borrow with an empty body, so the delta isolates wrapper overhead
// (shared_ptr domain-state refcounts in the C++ layer) from core cost.
#include <mgmem/memory.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

static const char *arg_value(int argc, char **argv, const char *name, const char *fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}

int main(int argc, char **argv) {
    const bool cpp = !std::strcmp(arg_value(argc, argv, "--api", "c"), "cpp");
    const bool shared = !std::strcmp(arg_value(argc, argv, "--sharing", "shared"), "shared");
    const int threads = std::atoi(arg_value(argc, argv, "--threads", "1"));
    const long long iterations = std::atoll(arg_value(argc, argv, "--iterations", "200000"));
    CHECK(threads >= 1 && iterations > 0);
    constexpr int kOwnersPerThread = 64;

    auto make_domain = [] {
        mgmem::domain domain;
        CHECK(mgmem::domain::create_arena(64ull << 20, 1ull << 20, domain) == MGMEM_OK);
        return domain;
    };
    struct Prepared {
        mgmem::domain domain;
        std::vector<mgmem::owned_allocation> owners;
        std::vector<mgmem_handle_v1> handles; // C view of the same owners is not
        // exported by the wrapper; C mode therefore uses its own allocations.
        mgmem_domain *native = nullptr;
    };
    auto prepare_cpp = [&](mgmem::domain domain) {
        Prepared p; p.domain = std::move(domain);
        for (int i = 0; i < kOwnersPerThread; ++i) {
            mgmem::owned_allocation owner;
            CHECK(p.domain.allocate(64, owner) == MGMEM_OK);
            p.owners.push_back(std::move(owner));
        }
        return p;
    };
    auto prepare_c = [&](mgmem_domain *native) {
        std::vector<mgmem_handle_v1> handles;
        for (int i = 0; i < kOwnersPerThread; ++i) {
            mgmem_handle_v1 h{}; CHECK(mgmem_allocate_v1(native, 64, 16, &h) == MGMEM_OK);
            handles.push_back(h);
        }
        return handles;
    };
    auto make_native = [] {
        mgmem_domain_options_v1 options{};
        options.struct_size = sizeof(options); options.abi_major = MGMEM_ABI_MAJOR; options.abi_minor = MGMEM_ABI_MINOR;
        options.mode = MGMEM_ARENA; options.budget_bytes = 64ull << 20; options.arena_chunk_bytes = 1ull << 20;
        mgmem_domain *domain = nullptr; CHECK(mgmem_domain_create_v1(&options, &domain) == MGMEM_OK);
        return domain;
    };
    auto close_native = [](mgmem_domain *domain) {
        CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
        CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    };

    const long long per_thread = iterations / threads;
    auto run = [&](bool timed) {
        std::vector<std::thread> workers;
        std::vector<Prepared> cpp_sets;
        std::vector<mgmem_domain *> natives;
        std::vector<std::vector<mgmem_handle_v1>> c_sets;
        if (cpp) {
            if (shared) {
                auto domain = make_domain();
                // One shared domain: move slices of owners to each thread by
                // preparing per-thread owner vectors on the same domain.
                for (int t = 0; t < threads; ++t) {
                    Prepared p; // shares state below via owners' own refs
                    for (int i = 0; i < kOwnersPerThread; ++i) {
                        mgmem::owned_allocation owner;
                        CHECK(domain.allocate(64, owner) == MGMEM_OK);
                        p.owners.push_back(std::move(owner));
                    }
                    cpp_sets.push_back(std::move(p));
                }
                // Keep the domain wrapper alive in the first set.
                cpp_sets.front().domain = std::move(domain);
            } else {
                for (int t = 0; t < threads; ++t) cpp_sets.push_back(prepare_cpp(make_domain()));
            }
        } else {
            if (shared) {
                auto *native = make_native(); natives.push_back(native);
                for (int t = 0; t < threads; ++t) c_sets.push_back(prepare_c(native));
            } else {
                for (int t = 0; t < threads; ++t) {
                    auto *native = make_native(); natives.push_back(native);
                    c_sets.push_back(prepare_c(native));
                }
            }
        }
        const auto started = std::chrono::steady_clock::now();
        if (!timed) { /* warmup uses same path with fewer iterations */ }
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                const long long n = timed ? per_thread : per_thread / 10;
                if (cpp) {
                    auto &owners = cpp_sets[static_cast<size_t>(t)].owners;
                    for (long long i = 0; i < n; ++i)
                        CHECK(owners[static_cast<size_t>(i % kOwnersPerThread)]
                                  .with_read([](const void *, uint64_t) {}) == MGMEM_OK);
                } else {
                    auto *native = shared ? natives.front() : natives[static_cast<size_t>(t)];
                    auto &handles = c_sets[static_cast<size_t>(t)];
                    for (long long i = 0; i < n; ++i) {
                        mgmem_borrow_v1 view{};
                        CHECK(mgmem_borrow_acquire_v1(native, handles[static_cast<size_t>(i % kOwnersPerThread)],
                                                      0, 64, MGMEM_READ, &view) == MGMEM_OK);
                        CHECK(mgmem_borrow_release_v1(native, view.lease) == MGMEM_OK);
                    }
                }
            });
        }
        for (auto &w : workers) w.join();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        for (auto &set : c_sets) for (auto h : set) CHECK(mgmem_release_v1(
            shared ? natives.front() : natives[&set - &c_sets[0]], h) == MGMEM_OK);
        for (auto *native : natives) close_native(native);
        return elapsed;
    };
    (void)run(false);
    const auto elapsed = run(true);
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    std::printf("{\"api\":\"%s\",\"sharing\":\"%s\",\"threads\":%d,\"iterations\":%lld,\"ns_per_op\":%.2f}\n",
                cpp ? "cpp" : "c", shared ? "shared" : "per-thread", threads,
                per_thread * threads, ns / static_cast<double>(per_thread * threads));
}
