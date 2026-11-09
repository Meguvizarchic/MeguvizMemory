#ifndef MGMEM_MEMORY_HPP
#define MGMEM_MEMORY_HPP

#include <mgmem/mgmem.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// MSVC can otherwise outline nested borrow paths even under /O2 /Ob2,
// adding a per-access frame to synchronous views. Other compilers keep ordinary
// inline semantics, including when a callback recursively acquires a READ view.
#if defined(_MSC_VER)
# define MGMEM_CPP_BORROW_INLINE __forceinline
#else
# define MGMEM_CPP_BORROW_INLINE inline
#endif

/* Optional, header-only C++17 ownership adapter. No global allocator replacement.
 * Wrappers are consumer-local C++ objects, not a cross-DSO C++ ABI. The selected
 * native runtime module must remain loaded until every wrapper has been released.
 *
 * Each wrapper is move-only. Distinct wrappers may be handed between threads;
 * mutating the same wrapper concurrently still requires caller synchronization.
 * An allocation and every borrow retain the domain independently. Destroying a
 * domain wrapper never invalidates its surviving allocations or borrows.
 *
 * Storage is uninitialized. A raw data pointer may only be used while its borrow
 * exists, and READ pointers may not be written through const_cast. Acquire the
 * borrow before queueing asynchronous work. No raw C owner handle is exported.
 *
 * Successful release consumes the wrapper. Empty release is an idempotent OK.
 * Checked-import owner/lease release returning PROVIDER_ERROR has uncertain
 * consumption: the wrapper is invalidated without retry and its shared domain
 * is quarantined. The domain must remain loaded, and its native state and any
 * uncertain claims are intentionally retained, never force-collected/destroyed.
 * domain::quarantined() reports this sticky state while a domain wrapper exists.
 * Use explicit release to observe the status; a destructor cannot return it.
 * All fallible creation functions require an empty output and preserve it on
 * failure. std::bad_alloc while creating shared ownership maps to OUT_OF_MEMORY.
 *
 * Final release/destruction may invoke synchronous provider cleanup, including
 * begin_close -> collect -> drain -> destroy for the last shared state. Arrange
 * for the last wrapper to be destroyed on a cleanup worker, not a UI/completion
 * thread that must remain responsive. begin_close itself remains nonblocking.
 *
 * Except for checked-import PROVIDER_ERROR quarantine, destructors are noexcept
 * and require all C cleanup calls to succeed. A provider
 * violating its no-throw/lifetime contract or an invalid ownership state causes
 * std::terminate; checked-import quarantine deliberately retains resources as
 * described above and never forcibly frees storage still borrowed. Explicit
 * release/collect return status for earlier diagnosis.
 */

namespace mgmem {
namespace detail {
inline void require_cleanup(mgmem_status status) noexcept {
    if (status != MGMEM_OK) std::terminate();
}

struct domain_state {
    mgmem_domain *native = nullptr;
    std::atomic<bool> checked_quarantine{false};

    domain_state() = default;
    domain_state(const domain_state &) = delete;
    domain_state &operator=(const domain_state &) = delete;

    ~domain_state() noexcept {
        if (!native) return;
        // A checked failure may have consumed a handle or invoked its foreign
        // release already. Neither retry nor destruction is safe without an
        // unambiguous completion signal. Preserve the native domain/claims.
        if (checked_quarantine.load(std::memory_order_acquire)) return;
        // Every wrapped owner/lease holds this state. Its final destructor cannot
        // run while any such resource or admitted wrapper operation still exists.
        require_cleanup(mgmem_domain_begin_close_v1(native));
        require_cleanup(mgmem_domain_collect_v1(native));
        require_cleanup(mgmem_domain_drain_v1(native, 0));
        require_cleanup(mgmem_domain_destroy_v1(native));
    }

    bool quarantined() const noexcept {
        return checked_quarantine.load(std::memory_order_acquire);
    }
    void quarantine() noexcept {
        checked_quarantine.store(true, std::memory_order_release);
    }

    // A retain callback can return PROVIDER_ERROR without poisoning the core;
    // unlike a failed final release this may remain a normal retryable domain.
    // An unavailable snapshot or any pending import is uncertain. This may
    // conservatively quarantine an unrelated in-flight import in another
    // thread, but never mistakes an abandoned reservation for clean failure.
    void observe_checked_import_error() noexcept {
        mgmem_import_snapshot_v1 observed{};
        if (mgmem_domain_import_snapshot_v1(native, &observed, sizeof(observed)) != MGMEM_OK ||
            observed.poisoned != 0 || observed.pending_import_bytes != 0) quarantine();
    }
};
} // namespace detail

class domain;
class owned_allocation;

class borrow {
public:
    borrow() noexcept = default;
    borrow(const borrow &) = delete;
    borrow &operator=(const borrow &) = delete;

    borrow(borrow &&other) noexcept { take(other); }
    borrow &operator=(borrow &&other) noexcept {
        if (this != &other) {
            cleanup();
            take(other);
        }
        return *this;
    }

    ~borrow() noexcept { cleanup(); }

    explicit operator bool() const noexcept { return static_cast<bool>(state_); }
    const void *data() const noexcept { return view_.data; }
    void *write_data() const noexcept { return access_ == MGMEM_WRITE ? view_.data : nullptr; }
    uint64_t size_bytes() const noexcept { return view_.length; }
    uint32_t access() const noexcept { return access_; }

    mgmem_status release() noexcept {
        if (!state_) return MGMEM_OK;
        const mgmem_status status = mgmem_borrow_release_v1(state_->native, view_.lease);
        if (status == MGMEM_OK || (checked_ && status == MGMEM_PROVIDER_ERROR)) {
            if (status != MGMEM_OK) state_->quarantine();
            view_ = {};
            access_ = MGMEM_READ;
            checked_ = false;
            state_.reset();
        }
        return status;
    }

private:
    friend class owned_allocation;
    std::shared_ptr<detail::domain_state> state_;
    mgmem_borrow_v1 view_{};
    uint32_t access_ = MGMEM_READ;
    bool checked_ = false;

    void cleanup() noexcept {
        const bool was_checked = checked_;
        const auto status = release();
        if (!(was_checked && status == MGMEM_PROVIDER_ERROR)) detail::require_cleanup(status);
    }

    void take(borrow &other) noexcept {
        state_ = std::move(other.state_);
        view_ = other.view_;
        access_ = other.access_;
        checked_ = other.checked_;
        other.view_ = {};
        other.access_ = MGMEM_READ;
        other.checked_ = false;
    }
};

class owned_allocation {
public:
    owned_allocation() noexcept = default;
    owned_allocation(const owned_allocation &) = delete;
    owned_allocation &operator=(const owned_allocation &) = delete;

    owned_allocation(owned_allocation &&other) noexcept { take(other); }
    owned_allocation &operator=(owned_allocation &&other) noexcept {
        if (this != &other) {
            cleanup();
            take(other);
        }
        return *this;
    }

    ~owned_allocation() noexcept { cleanup(); }

    explicit operator bool() const noexcept { return static_cast<bool>(state_); }
    uint64_t size_bytes() const noexcept { return bytes_; }

    MGMEM_CPP_BORROW_INLINE mgmem_status acquire_borrow(uint64_t offset, uint64_t length, uint32_t access,
                                mgmem::borrow &out) const noexcept {
        if (!state_ || out) return MGMEM_INVALID_ARGUMENT;
        if (state_->quarantined()) return MGMEM_PROVIDER_ERROR;
        mgmem_borrow_v1 view{};
        const mgmem_status status = mgmem_borrow_acquire_v1(state_->native, handle_,
                                                           offset, length, access, &view);
        if (status != MGMEM_OK) return status;
        out.state_ = state_;
        out.view_ = view;
        out.access_ = access;
        out.checked_ = checked_;
        return MGMEM_OK;
    }

    // Synchronous views; pointers must not escape the body. READ supplies const
    // bytes, WRITE remains exclusive. A final checked release error takes
    // precedence over the body status; body exceptions use ordinary RAII.
    template<class Function>
    MGMEM_CPP_BORROW_INLINE mgmem_status with_read(Function &&body) const {
        return with_read(0, bytes_, std::forward<Function>(body));
    }

    template<class Function>
    MGMEM_CPP_BORROW_INLINE mgmem_status with_read(uint64_t offset, uint64_t length, Function &&body) const {
        return with_borrow<MGMEM_READ>(offset, length, std::forward<Function>(body));
    }

    template<class Function>
    MGMEM_CPP_BORROW_INLINE mgmem_status with_write(Function &&body) const {
        return with_write(0, bytes_, std::forward<Function>(body));
    }

    template<class Function>
    MGMEM_CPP_BORROW_INLINE mgmem_status with_write(uint64_t offset, uint64_t length, Function &&body) const {
        return with_borrow<MGMEM_WRITE>(offset, length, std::forward<Function>(body));
    }

    mgmem_status release() noexcept {
        if (!state_) return MGMEM_OK;
        const mgmem_status status = mgmem_release_v1(state_->native, handle_);
        if (status == MGMEM_OK || (checked_ && status == MGMEM_PROVIDER_ERROR)) {
            if (status != MGMEM_OK) state_->quarantine();
            handle_ = {};
            bytes_ = 0;
            checked_ = false;
            state_.reset();
        }
        return status;
    }

private:
    friend class domain;
    std::shared_ptr<detail::domain_state> state_;
    mgmem_handle_v1 handle_{};
    uint64_t bytes_ = 0;
    bool checked_ = false;

    template<uint32_t Access, class Function>
    MGMEM_CPP_BORROW_INLINE mgmem_status with_borrow(uint64_t offset, uint64_t length, Function &&body) const {
        using pointer = std::conditional_t<Access == MGMEM_READ, const void *, void *>;
        using result = std::invoke_result_t<Function, pointer, uint64_t>;
        static_assert(std::is_same<result, void>::value ||
                      std::is_same<result, mgmem_status>::value,
                      "Borrow body must return void or mgmem_status");
        mgmem::borrow view;
        const auto acquired = acquire_borrow(offset, length, Access, view);
        if (acquired != MGMEM_OK) return acquired;
        pointer data;
        if constexpr (Access == MGMEM_READ) data = view.data();
        else data = view.write_data();
        mgmem_status body_status = MGMEM_OK;
        if constexpr (std::is_same<result, void>::value)
            std::forward<Function>(body)(data, view.size_bytes());
        else body_status = std::forward<Function>(body)(data, view.size_bytes());
        // The body may move/release the owner and domain. Only the local lease
        // is used from here; it already pins the native domain and payload.
        const auto released = view.release();
        if (released != MGMEM_OK && view) detail::require_cleanup(released);
        return released == MGMEM_OK ? body_status : released;
    }

    void cleanup() noexcept {
        const bool was_checked = checked_;
        const auto status = release();
        if (!(was_checked && status == MGMEM_PROVIDER_ERROR)) detail::require_cleanup(status);
    }

    void take(owned_allocation &other) noexcept {
        state_ = std::move(other.state_);
        handle_ = other.handle_;
        bytes_ = other.bytes_;
        checked_ = other.checked_;
        other.handle_ = {};
        other.bytes_ = 0;
        other.checked_ = false;
    }
};

class domain {
public:
    domain() noexcept = default;
    domain(const domain &) = delete;
    domain &operator=(const domain &) = delete;
    domain(domain &&) noexcept = default;
    domain &operator=(domain &&) noexcept = default;
    ~domain() noexcept = default;

    explicit operator bool() const noexcept { return static_cast<bool>(state_); }
    bool quarantined() const noexcept { return state_ && state_->quarantined(); }

    static mgmem_status create(const mgmem_domain_options_v1 &options, domain &out) noexcept {
        if (out) return MGMEM_INVALID_ARGUMENT;
        try {
            // Allocate C++ ownership before creating the C owner; OOM cannot orphan
            // a successfully created native domain.
            auto state = std::make_shared<detail::domain_state>();
            const mgmem_status status = mgmem_domain_create_v1(&options, &state->native);
            if (status != MGMEM_OK) return status;
            out.state_ = std::move(state);
            return MGMEM_OK;
        } catch (const std::bad_alloc &) {
            return MGMEM_OUT_OF_MEMORY;
        } catch (...) {
            return MGMEM_PROVIDER_ERROR;
        }
    }

    static mgmem_status create_system(uint64_t budget, domain &out,
                                     const mgmem_provider_v1 *provider = nullptr) noexcept {
        mgmem_domain_options_v1 options{};
        options.struct_size = sizeof(options);
        options.abi_major = MGMEM_ABI_MAJOR;
        options.abi_minor = MGMEM_ABI_MINOR;
        options.mode = MGMEM_SYSTEM;
        options.budget_bytes = budget;
        options.provider = provider;
        return create(options, out);
    }

    static mgmem_status create_arena(uint64_t budget, uint64_t chunk, domain &out,
                                    const mgmem_provider_v1 *provider = nullptr) noexcept {
        mgmem_domain_options_v1 options{};
        options.struct_size = sizeof(options);
        options.abi_major = MGMEM_ABI_MAJOR;
        options.abi_minor = MGMEM_ABI_MINOR;
        options.mode = MGMEM_ARENA;
        options.budget_bytes = budget;
        options.arena_chunk_bytes = chunk;
        options.provider = provider;
        return create(options, out);
    }

    mgmem_status allocate(uint64_t bytes, uint64_t alignment, owned_allocation &out) const noexcept {
        if (!state_ || out) return MGMEM_INVALID_ARGUMENT;
        if (state_->quarantined()) return MGMEM_PROVIDER_ERROR;
        mgmem_handle_v1 handle{};
        const mgmem_status status = mgmem_allocate_v1(state_->native, bytes, alignment, &handle);
        if (status != MGMEM_OK) return status;
        out.state_ = state_;
        out.handle_ = handle;
        out.bytes_ = bytes;
        return MGMEM_OK;
    }

    mgmem_status allocate(uint64_t bytes, owned_allocation &out) const noexcept {
        return allocate(bytes, alignof(std::max_align_t), out);
    }

    // Synchronous uninitialized workspace, with one WRITE borrow for the body.
    // The body receives (void *, uint64_t), and returns void or mgmem_status.
    // It runs only after allocation and borrow succeed. Its status is returned;
    // its exceptions propagate after the ordinary RAII cleanup below. Cleanup
    // retains the same fail-stop contract as owned_allocation and borrow.
    // Pointers must not escape the body or be queued for later asynchronous use.
    // Arena storage remains retained until explicit reset/trim, as with allocate.
    template<class Function>
    mgmem_status with_allocation(uint64_t bytes, uint64_t alignment, Function &&body) const {
        using result = std::invoke_result_t<Function, void *, uint64_t>;
        static_assert(std::is_same<result, void>::value ||
                      std::is_same<result, mgmem_status>::value,
                      "Workspace body must return void or mgmem_status");
        owned_allocation allocation;
        const auto allocated = allocate(bytes, alignment, allocation);
        if (allocated != MGMEM_OK) return allocated;
        borrow view;
        const auto acquired = allocation.acquire_borrow(0, bytes, MGMEM_WRITE, view);
        if (acquired != MGMEM_OK) return acquired;
        if constexpr (std::is_same<result, void>::value) {
            std::forward<Function>(body)(view.write_data(), view.size_bytes());
            return MGMEM_OK;
        } else {
            return std::forward<Function>(body)(view.write_data(), view.size_bytes());
        }
    }

    template<class Function>
    mgmem_status with_allocation(uint64_t bytes, Function &&body) const {
        return with_allocation(bytes, alignof(std::max_align_t), std::forward<Function>(body));
    }

    mgmem_status import_external(const mgmem_external_buffer_v1 &buffer, owned_allocation &out) const noexcept {
        if (!state_ || out) return MGMEM_INVALID_ARGUMENT;
        if (state_->quarantined()) return MGMEM_PROVIDER_ERROR;
        mgmem_handle_v1 handle{};
        const mgmem_status status = mgmem_import_external_v1(state_->native, &buffer, &handle);
        if (status != MGMEM_OK) return status;
        out.state_ = state_;
        out.handle_ = handle;
        out.bytes_ = buffer.capacity_bytes;
        return MGMEM_OK;
    }

    // This method links the optional checked-import symbol. Deploy a runtime
    // that actually exports it; ABI minor 1 alone does not guarantee support.
    mgmem_status import_external_checked(const mgmem_external_buffer_checked_v1 &buffer,
                                         owned_allocation &out) const noexcept {
        if (!state_ || out) return MGMEM_INVALID_ARGUMENT;
        if (state_->quarantined()) return MGMEM_PROVIDER_ERROR;
        if ((mgmem_capabilities_v1() & MGMEM_CAP_CHECKED_EXTERNAL_RELEASE) == 0) return MGMEM_UNSUPPORTED;
        mgmem_handle_v1 handle{};
        const mgmem_status status = mgmem_import_external_checked_v1(state_->native, &buffer, &handle);
        if (status != MGMEM_OK) {
            if (status == MGMEM_PROVIDER_ERROR) state_->observe_checked_import_error();
            return status;
        }
        out.state_ = state_;
        out.handle_ = handle;
        out.bytes_ = buffer.capacity_bytes;
        out.checked_ = true;
        return MGMEM_OK;
    }

    // These methods link optional arena-import symbols. The selected runtime
    // must export them as well as advertise MGMEM_CAP_ARENA_EXTERNAL_IMPORT.
    mgmem_status import_external_arena(const mgmem_external_buffer_v1 &buffer,
                                       owned_allocation &out) const noexcept {
        if (!state_ || out) return MGMEM_INVALID_ARGUMENT;
        if (state_->quarantined()) return MGMEM_PROVIDER_ERROR;
        if ((mgmem_capabilities_v1() & MGMEM_CAP_ARENA_EXTERNAL_IMPORT) == 0) return MGMEM_UNSUPPORTED;
        mgmem_handle_v1 handle{};
        const mgmem_status status = mgmem_import_external_arena_v1(state_->native, &buffer, &handle);
        if (status != MGMEM_OK) return status;
        out.state_ = state_;
        out.handle_ = handle;
        out.bytes_ = buffer.capacity_bytes;
        return MGMEM_OK;
    }

    mgmem_status import_external_arena_checked(const mgmem_external_buffer_checked_v1 &buffer,
                                               owned_allocation &out) const noexcept {
        if (!state_ || out) return MGMEM_INVALID_ARGUMENT;
        if (state_->quarantined()) return MGMEM_PROVIDER_ERROR;
        const uint64_t required = MGMEM_CAP_ARENA_EXTERNAL_IMPORT | MGMEM_CAP_CHECKED_EXTERNAL_RELEASE;
        if ((mgmem_capabilities_v1() & required) != required) return MGMEM_UNSUPPORTED;
        mgmem_handle_v1 handle{};
        const mgmem_status status = mgmem_import_external_arena_checked_v1(state_->native, &buffer, &handle);
        if (status != MGMEM_OK) {
            if (status == MGMEM_PROVIDER_ERROR) state_->observe_checked_import_error();
            return status;
        }
        out.state_ = state_;
        out.handle_ = handle;
        out.bytes_ = buffer.capacity_bytes;
        out.checked_ = true;
        return MGMEM_OK;
    }

    mgmem_status set_budget(uint64_t bytes) const noexcept {
        return !state_ ? MGMEM_INVALID_ARGUMENT : state_->quarantined() ? MGMEM_PROVIDER_ERROR :
            mgmem_domain_set_budget_v1(state_->native, bytes);
    }

    mgmem_status import_snapshot(mgmem_import_snapshot_v1 &out) const noexcept {
        return state_ ? mgmem_domain_import_snapshot_v1(state_->native, &out, sizeof(out)) : MGMEM_INVALID_ARGUMENT;
    }

    mgmem_status begin_close() const noexcept {
        return state_ ? mgmem_domain_begin_close_v1(state_->native) : MGMEM_INVALID_ARGUMENT;
    }
    mgmem_status collect() const noexcept {
        return !state_ ? MGMEM_INVALID_ARGUMENT : state_->quarantined() ? MGMEM_PROVIDER_ERROR :
            mgmem_domain_collect_v1(state_->native);
    }
    mgmem_status drain(uint64_t timeout_ms) const noexcept {
        return !state_ ? MGMEM_INVALID_ARGUMENT : state_->quarantined() ? MGMEM_PROVIDER_ERROR :
            mgmem_domain_drain_v1(state_->native, timeout_ms);
    }
    mgmem_status reset() const noexcept {
        return !state_ ? MGMEM_INVALID_ARGUMENT : state_->quarantined() ? MGMEM_PROVIDER_ERROR :
            mgmem_arena_reset_v1(state_->native);
    }
    mgmem_status trim() const noexcept {
        return !state_ ? MGMEM_INVALID_ARGUMENT : state_->quarantined() ? MGMEM_PROVIDER_ERROR :
            mgmem_domain_trim_v1(state_->native);
    }
    mgmem_status snapshot(mgmem_snapshot_v1 &out) const noexcept {
        return state_ ? mgmem_domain_snapshot_v1(state_->native, &out, sizeof(out)) : MGMEM_INVALID_ARGUMENT;
    }

private:
    std::shared_ptr<detail::domain_state> state_;
};

} // namespace mgmem
#undef MGMEM_CPP_BORROW_INLINE
#endif
