#include <mgmem/mgmem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <malloc.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using u64 = uint64_t;
constexpr u64 max_u64 = std::numeric_limits<u64>::max();
std::atomic<u64> next_domain_id{1};
unsigned char runtime_anchor;
u64 runtime_origin() noexcept { return static_cast<u64>(reinterpret_cast<uintptr_t>(&runtime_anchor)); }

bool add(u64 a, u64 b, u64 &out) noexcept {
    if (a > max_u64 - b) return false;
    out = a + b;
    return true;
}

bool power_of_two(u64 n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

bool round_up(u64 n, u64 alignment, u64 &out) noexcept {
    u64 sum = 0;
    if (!power_of_two(alignment) || !add(n, alignment - 1, sum)) return false;
    out = sum & ~(alignment - 1);
    return true;
}

bool take_domain_id(u64 &id) noexcept {
    u64 candidate = next_domain_id.load(std::memory_order_relaxed);
    while (candidate != max_u64) {
        if (next_domain_id.compare_exchange_weak(candidate, candidate + 1,
                std::memory_order_relaxed)) {
            id = candidate;
            return true;
        }
    }
    return false;
}

u64 page_size() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<u64>(size) : 0;
#endif
}

template<class Function>
mgmem_status guarded(Function &&function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc &) {
        return MGMEM_OUT_OF_MEMORY;
    } catch (...) {
        return MGMEM_PROVIDER_ERROR;
    }
}

struct RangeClaim { uintptr_t begin; uintptr_t end; u64 external_id; u64 token; bool releasing; };

// Registry references belong to one explicit cleanup path. Destruction never
// drops an uncertain claim retained by a poisoned domain.
struct Backing {
    void *address = nullptr;
    u64 bytes = 0;
    u64 alignment = 0;
    u64 used = 0;
    bool pending = false;
    bool claim_active = false;
    std::list<RangeClaim>::iterator claim_node{};
    bool external = false;
    uint32_t access = MGMEM_WRITE;
    void *external_context = nullptr;
    void (*external_release)(void *) = nullptr;
    mgmem_status (*external_release_checked)(void *) = nullptr;
    // Established immediately on insertion. list splice preserves this identity;
    // rollback copies are detached values and never use it to reclaim a node.
    std::list<Backing>::iterator node{};
};

struct Allocation {
    Backing *backing = nullptr;
    u64 offset = 0;
    u64 bytes = 0;
    u64 readers = 0;
    bool writer = false;
    bool owner = false;
    bool pending = false;
};

struct Lease {
    u64 allocation = 0;
    bool writer = false;
};

// Handles remain monotonic IDs. Only these private records move during probing.
// One active record needs no table allocation; larger live sets use one array.
template<class Record>
class HandleTable {
    static_assert(std::is_trivially_copyable<Record>::value,
                  "Private handle records must move without throwing");
    static_assert(std::is_nothrow_copy_constructible<Record>::value,
                  "Private handle records must construct without throwing");
    struct Slot {
        u64 id;
        // Empty slots have no payload. Explicit construction starts its lifetime
        // in C++17 as well; never inspect value while id is 0.
        union { Record value; };
        Slot() noexcept : id(0) {}
        Slot(u64 key, Record record) noexcept : id(key), value(record) {}
        void store(u64 key, const Record &record) noexcept {
            ::new (static_cast<void *>(&value)) Record(record);
            id = key;
        }
    };
    Slot single_{};
    std::unique_ptr<Slot[]> slots_;
    // With no array, this is only a bounded prediction for the next batch.
    // The first record still uses single_; quiet reclamation forgets the hint.
    size_t capacity_ = 0;
    size_t size_ = 0;

    static size_t index(u64 id, size_t mask) noexcept {
        // SplitMix64's public-domain output mixing, without generator state.
        id = (id ^ (id >> 30)) * 0xbf58476d1ce4e5b9ull;
        id = (id ^ (id >> 27)) * 0x94d049bb133111ebull;
        return static_cast<size_t>(id ^ (id >> 31)) & mask;
    }

    static Slot *place(Slot *slots, size_t capacity, const Slot &value) noexcept {
        const size_t mask = capacity - 1;
        size_t position = index(value.id, mask);
        while (slots[position].id) position = (position + 1) & mask;
        slots[position].store(value.id, value.value);
        return &slots[position];
    }

    void grow() {
        const size_t maximum = std::numeric_limits<size_t>::max() / sizeof(Slot);
        if (slots_ && capacity_ > maximum / 2) throw std::bad_alloc();
        const size_t next = slots_ ? capacity_ * 2 : std::max(capacity_, size_t{4});
        if (next > maximum) throw std::bad_alloc();
        auto fresh = std::make_unique<Slot[]>(next);
        if (!slots_) place(fresh.get(), next, single_);
        else for (size_t i = 0; i < capacity_; ++i)
            if (slots_[i].id) place(fresh.get(), next, slots_[i]);
        slots_.swap(fresh);
        capacity_ = next;
        single_.id = 0;
    }

public:
    HandleTable() = default;
    HandleTable(const HandleTable &) = delete;
    HandleTable &operator=(const HandleTable &) = delete;
    bool empty() const noexcept { return size_ == 0; }
    size_t size() const noexcept { return size_; }

    // The caller takes a fresh nonzero ID under the domain lock before insertion.
    Record &insert_fresh(u64 id, Record value) {
        if (!slots_ && !size_) {
            single_.store(id, value);
            size_ = 1;
            return single_.value;
        }
        if (!slots_ || size_ >= capacity_ - capacity_ / 4) grow();
        auto *slot = place(slots_.get(), capacity_, Slot{id, value});
        ++size_;
        return slot->value;
    }

    Slot *find(u64 id) noexcept {
        if (!slots_) return single_.id == id && id ? &single_ : nullptr;
        const size_t mask = capacity_ - 1;
        size_t position = index(id, mask);
        while (slots_[position].id) {
            if (slots_[position].id == id) return &slots_[position];
            position = (position + 1) & mask;
        }
        return nullptr;
    }

    // A location returned under the same lock remains valid until this erase.
    void erase_found(Slot *slot) noexcept {
        if (!slots_) {
            single_.id = 0;
            size_ = 0;
            return;
        }
        const size_t mask = capacity_ - 1;
        size_t hole = static_cast<size_t>(slot - slots_.get());
        if (--size_ == 0) {
            slots_.reset();
            // Learn short phases without retaining heap storage or reserving
            // an unbounded historical peak for a later two-record workload.
            capacity_ = std::min(capacity_, size_t{128});
            single_.id = 0;
            return;
        }
        for (size_t current = (hole + 1) & mask; slots_[current].id;
                current = (current + 1) & mask) {
            const size_t home = index(slots_[current].id, mask);
            // Move only when the hole lies on this entry's probe path. Keep
            // scanning past at-home entries: later entries may still cross it.
            if (((hole - home) & mask) < ((current - home) & mask)) {
                slots_[hole].store(slots_[current].id, slots_[current].value);
                hole = current;
            }
        }
        slots_[hole].id = 0;
    }

    void erase(u64 id) noexcept {
        if (auto *slot = find(id)) erase_found(slot);
    }

    // Used only when there are no active leases. Empty batches retain no array.
    void release_storage() noexcept {
        slots_.reset();
        capacity_ = 0;
        size_ = 0;
        single_.id = 0;
    }
};

using AllocationTable = HandleTable<Allocation>;
using LeaseTable = HandleTable<Lease>;

// This registry is scoped to one loaded runtime. It stores no domain pointers.
// Lock order is domain -> registry, never registry -> domain. Callbacks run with
// neither lock held. Owned allocations may be legitimately recycled by malloc
// before an old deallocation callback has returned, so their claims may overlap
// transiently only while the old callback has no owner/lease. Imports reject
// overlap with every claim, even those callbacks' still-charged ranges. A
// checked external release may report failure or leave ownership uncertain;
// its claim never becomes reusable until the callback has succeeded. The legacy
// void callback retains its historical provider-reuse behavior.
std::mutex range_mutex;
std::list<RangeClaim> range_claims;
u64 next_claim = 1;

mgmem_status claim_range(const void *data, u64 bytes, u64 external_id, Backing &backing) noexcept {
    backing.claim_active = false;
    backing.claim_node = {};
    const auto begin = reinterpret_cast<uintptr_t>(data);
    if (!data || bytes == 0 || bytes > std::numeric_limits<uintptr_t>::max() - begin)
        return MGMEM_OVERFLOW;
    const auto end = begin + static_cast<uintptr_t>(bytes);
    try {
        std::lock_guard<std::mutex> lock(range_mutex);
        if (external_id) for (const auto &claim : range_claims) {
            if (claim.external_id == external_id || (begin < claim.end && claim.begin < end))
                return MGMEM_BUSY;
        }
        if (next_claim == max_u64) return MGMEM_OVERFLOW;
        const u64 token = next_claim++;
        range_claims.push_back({begin, end, external_id, token, false});
        backing.claim_node = std::prev(range_claims.end());
        backing.claim_active = true;
        return MGMEM_OK;
    } catch (const std::bad_alloc &) { return MGMEM_OUT_OF_MEMORY; }
    catch (...) { return MGMEM_PROVIDER_ERROR; }
}

mgmem_status reserve_owned_claim(Backing &backing) noexcept {
    backing.claim_active = false;
    backing.claim_node = {};
    try {
        std::lock_guard<std::mutex> lock(range_mutex);
        if (next_claim == max_u64) return MGMEM_OVERFLOW;
        const u64 token = next_claim++;
        range_claims.push_back({0, 0, 0, token, false});
        backing.claim_node = std::prev(range_claims.end());
        backing.claim_active = true;
        return MGMEM_OK;
    } catch (const std::bad_alloc &) { return MGMEM_OUT_OF_MEMORY; }
    catch (...) { return MGMEM_PROVIDER_ERROR; }
}

mgmem_status publish_owned_claim(const Backing &backing, void *data, u64 bytes, bool &unsafe_address) noexcept {
    unsafe_address = false;
    if (!data) return MGMEM_OK;
    const auto begin = reinterpret_cast<uintptr_t>(data);
    const bool range_valid = bytes <= std::numeric_limits<uintptr_t>::max() - begin;
    const auto end = range_valid ? begin + static_cast<uintptr_t>(bytes) : std::numeric_limits<uintptr_t>::max();
    std::lock_guard<std::mutex> lock(range_mutex);
    if (!backing.claim_active) {
        unsafe_address = true;
        return MGMEM_PROVIDER_ERROR;
    }
    const auto own = backing.claim_node;
    for (auto it = range_claims.begin(); it != range_claims.end(); ++it) {
        if (it != own && !it->releasing && begin < it->end && it->begin < end) unsafe_address = true;
    }
    own->begin = begin;
    own->end = end;
    if (!range_valid) unsafe_address = true;
    return unsafe_address ? MGMEM_PROVIDER_ERROR : MGMEM_OK;
}

void releasing_claim(const Backing &backing, bool releasing) noexcept {
    if (!backing.claim_active) return;
    std::lock_guard<std::mutex> lock(range_mutex);
    backing.claim_node->releasing = releasing;
}

void drop_claim(Backing &backing) noexcept {
    if (!backing.claim_active) return;
    std::lock_guard<std::mutex> lock(range_mutex);
    range_claims.erase(backing.claim_node);
    backing.claim_active = false;
    backing.claim_node = {};
}
} // namespace

struct mgmem_domain {
    std::mutex mutex;
    std::condition_variable changed;
    uint32_t state = MGMEM_OPEN;
    uint32_t mode = MGMEM_SYSTEM;
    u64 id = 0;
    u64 next_id = 1;
    u64 generation = 1;
    u64 budget = 0;
    u64 chunk_bytes = 0;
    u64 page = 0;
    u64 backing_bytes = 0;
    u64 reserved_bytes = 0;
    u64 peak_bytes = 0;
    u64 live_bytes = 0;
    u64 retiring_bytes = 0;
    u64 allocation_count = 0;
    u64 provider_allocation_count = 0;
    u64 cumulative_requested = 0;
    u64 pending = 0;
    bool maintenance = false;
    bool provider_poisoned = false;
    bool custom_provider = false;
    u64 external_bytes = 0;
    u64 external_count = 0;
    u64 pending_import_bytes = 0;
    u64 import_count = 0;
    mgmem_provider_v1 provider{};
    std::list<Backing> backing;
    // A first-fit lower bound for requests with no smaller size or alignment.
    // Earlier blocks cannot regain space until reset or a pending publication.
    bool arena_search_valid = false;
    u64 arena_search_bytes = 0;
    u64 arena_search_alignment = 0;
    std::list<Backing>::iterator arena_search;
    AllocationTable allocations;
    LeaseTable leases;
};

namespace {
// The only condition-variable wait is drain(), which rejects OPEN under the
// same mutex. Closing/poison publication still notifies while holding that lock;
// ordinary OPEN operations cannot have a drain waiter to wake.
void notify_drain(mgmem_domain &domain) noexcept {
    if (domain.state != MGMEM_OPEN) domain.changed.notify_all();
}

void invalidate_arena_search(mgmem_domain &domain) noexcept {
    domain.arena_search_valid = false;
}

mgmem_status allocate_backing(mgmem_domain &domain, u64 bytes, u64 alignment,
                              void **out) noexcept {
    *out = nullptr;
    if (domain.custom_provider) {
        try {
            return domain.provider.allocate(domain.provider.context, bytes, alignment, out);
        } catch (const std::bad_alloc &) {
            return MGMEM_OUT_OF_MEMORY;
        } catch (...) {
            return MGMEM_PROVIDER_ERROR;
        }
    }
#if defined(_WIN32)
    *out = _aligned_malloc(static_cast<size_t>(bytes), static_cast<size_t>(alignment));
#else
    if (posix_memalign(out, static_cast<size_t>(alignment), static_cast<size_t>(bytes)) != 0)
        *out = nullptr;
#endif
    return *out ? MGMEM_OK : MGMEM_OUT_OF_MEMORY;
}

bool deallocate_backing(mgmem_domain &domain, Backing &backing) noexcept {
    if (!backing.address) { drop_claim(backing); return true; }
    if (!backing.external || !backing.external_release_checked)
        releasing_claim(backing, true);
    if (backing.external) {
        try {
            if (backing.external_release_checked) {
                if (backing.external_release_checked(backing.external_context) != MGMEM_OK) return false;
            } else {
                backing.external_release(backing.external_context);
            }
        }
        catch (...) { releasing_claim(backing, false); return false; }
    } else if (domain.custom_provider) {
        try {
            domain.provider.deallocate(domain.provider.context, backing.address,
                                       backing.bytes, backing.alignment);
        } catch (...) {
            // A callback violating noexcept might already have freed the address.
            // Do not retry it and risk a double free; this domain becomes poisoned.
            releasing_claim(backing, false);
            return false;
        }
    } else {
#if defined(_WIN32)
        _aligned_free(backing.address);
#else
        std::free(backing.address);
#endif
    }
    drop_claim(backing);
    return true;
}

bool take_id(mgmem_domain &domain, u64 &id) noexcept {
    if (domain.next_id == max_u64) return false;
    id = domain.next_id++;
    return true;
}

bool valid_handle(const mgmem_domain &domain, mgmem_handle_v1 handle) noexcept {
    return handle.runtime_origin == runtime_origin() && handle.domain_id == domain.id && handle.id != 0;
}

bool quiet(const mgmem_domain &domain) noexcept {
    return domain.allocations.empty() && domain.leases.empty() && domain.pending == 0 &&
           !domain.maintenance;
}

bool counters_fit(const mgmem_domain &domain, u64 bytes, bool new_backing) noexcept {
    u64 ignored = 0;
    return domain.allocation_count != max_u64 &&
           (!new_backing || domain.provider_allocation_count != max_u64) &&
           add(domain.cumulative_requested, bytes, ignored) &&
           add(domain.live_bytes, bytes, ignored);
}

void commit_allocation(mgmem_domain &domain, Allocation &allocation, u64 bytes) noexcept {
    allocation.owner = true;
    allocation.pending = false;
    domain.live_bytes += bytes;
    ++domain.allocation_count;
    domain.cumulative_requested += bytes;
}

// The allocation metadata has already been consumed. A SYSTEM allocation or an
// external backing in either mode is independently returned to its origin. Keep
// it charged until the callback finishes so another request cannot evade budget.
mgmem_status reclaim_standalone(mgmem_domain &domain, Backing *backing,
                                std::unique_lock<std::mutex> &lock) noexcept {
    std::list<Backing> detached;
    invalidate_arena_search(domain);
    detached.splice(detached.end(), domain.backing, backing->node);
    ++domain.pending;
    lock.unlock();
    const bool success = deallocate_backing(domain, detached.front());
    lock.lock();
    if (success) {
        domain.backing_bytes -= detached.front().bytes;
        if (detached.front().external) {
            domain.external_bytes -= detached.front().bytes;
            --domain.external_count;
        }
    }
    else domain.provider_poisoned = true;
    --domain.pending;
    notify_drain(domain);
    return success ? MGMEM_OK : MGMEM_PROVIDER_ERROR;
}

// No allocations or leases exist when entering. Splicing is allocation-free.
// While callbacks run, maintenance blocks new allocation/reset/trim/drain.
mgmem_status reclaim_all(mgmem_domain &domain, std::unique_lock<std::mutex> &lock,
                         bool closing) {
    if (domain.provider_poisoned) return MGMEM_PROVIDER_ERROR;
    domain.allocations.release_storage();
    domain.leases.release_storage();
    std::list<Backing> detached;
    invalidate_arena_search(domain);
    detached.splice(detached.end(), domain.backing);
    domain.maintenance = true;
    ++domain.pending;
    lock.unlock();
    u64 released = 0;
    u64 external_released = 0, external_count_released = 0;
    bool success = true;
    for (auto &backing : detached) {
        if (deallocate_backing(domain, backing)) {
            released += backing.bytes;
            if (backing.external) { external_released += backing.bytes; ++external_count_released; }
        }
        else success = false;
    }
    lock.lock();
    domain.backing_bytes -= released;
    domain.external_bytes -= external_released;
    domain.external_count -= external_count_released;
    --domain.pending;
    domain.maintenance = false;
    domain.provider_poisoned = !success;
    if (success && closing) domain.state = MGMEM_CLOSED;
    notify_drain(domain);
    return success ? MGMEM_OK : MGMEM_PROVIDER_ERROR;
}
} // namespace

extern "C" {
uint32_t mgmem_abi_version_v1(void) { return (MGMEM_ABI_MAJOR << 16) | MGMEM_ABI_MINOR; }
uint64_t mgmem_capabilities_v1(void) {
    return MGMEM_CAP_SYSTEM | MGMEM_CAP_ARENA | MGMEM_CAP_CUSTOM_PROVIDER |
           MGMEM_CAP_RW_LEASE | MGMEM_CAP_EXPLICIT_COLLECTION |
           MGMEM_CAP_EXTERNAL_IMPORT | MGMEM_CAP_SOFT_BUDGET |
           MGMEM_CAP_CHECKED_EXTERNAL_RELEASE | MGMEM_CAP_ARENA_EXTERNAL_IMPORT;
}
uint64_t mgmem_runtime_origin_v1(void) { return runtime_origin(); }

mgmem_status mgmem_checked_mul_v1(u64 a, u64 b, u64 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = 0;
    if (b != 0 && a > max_u64 / b) return MGMEM_OVERFLOW;
    *out = a * b;
    return MGMEM_OK;
}

mgmem_status mgmem_checked_round_up_v1(u64 bytes, u64 alignment, u64 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = 0;
    if (!power_of_two(alignment)) return MGMEM_INVALID_ARGUMENT;
    return round_up(bytes, alignment, *out) ? MGMEM_OK : MGMEM_OVERFLOW;
}

mgmem_status mgmem_domain_create_v1(const mgmem_domain_options_v1 *options,
                                    mgmem_domain **out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = nullptr;
    return guarded([&]() -> mgmem_status {
        if (!options || options->struct_size < sizeof(*options)) return MGMEM_INVALID_ARGUMENT;
        if (options->abi_major != MGMEM_ABI_MAJOR || options->abi_minor > MGMEM_ABI_MINOR ||
            options->required_flags != 0) return MGMEM_UNSUPPORTED;
        if (options->reserved != 0 || options->mode > MGMEM_ARENA || options->budget_bytes == 0)
            return MGMEM_INVALID_ARGUMENT;
        if (options->mode == MGMEM_ARENA && options->arena_chunk_bytes == 0)
            return MGMEM_INVALID_ARGUMENT;
        if (options->arena_chunk_bytes > std::numeric_limits<size_t>::max()) return MGMEM_OVERFLOW;
        if (options->provider) {
            const auto &provider = *options->provider;
            if (provider.struct_size < sizeof(provider)) return MGMEM_INVALID_ARGUMENT;
            if (provider.abi_major != MGMEM_ABI_MAJOR || provider.abi_minor > MGMEM_ABI_MINOR ||
                provider.required_flags != 0) return MGMEM_UNSUPPORTED;
            if (!provider.allocate || !provider.deallocate || !provider.retain || !provider.release)
                return MGMEM_INVALID_ARGUMENT;
        }
        auto domain = std::make_unique<mgmem_domain>();
        if (!take_domain_id(domain->id)) return MGMEM_OVERFLOW;
        domain->mode = options->mode;
        domain->budget = options->budget_bytes;
        domain->chunk_bytes = options->arena_chunk_bytes;
        domain->page = page_size();
        if (options->provider) {
            domain->provider = *options->provider;
            domain->custom_provider = true;
            domain->provider.retain(domain->provider.context);
        }
        *out = domain.release();
        return MGMEM_OK;
    });
}

mgmem_status mgmem_allocate_v1(mgmem_domain *domain, u64 bytes, u64 alignment,
                               mgmem_handle_v1 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = {};
    if (!domain || bytes == 0 || !power_of_two(alignment)) return MGMEM_INVALID_ARGUMENT;
    if (bytes > std::numeric_limits<size_t>::max() ||
        alignment > std::numeric_limits<size_t>::max()) return MGMEM_OVERFLOW;
    alignment = std::max(alignment, static_cast<u64>(sizeof(void *)));
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (domain->state != MGMEM_OPEN) return MGMEM_IS_CLOSED;
        if (domain->provider_poisoned) return MGMEM_PROVIDER_ERROR;
        if (domain->maintenance) return MGMEM_BUSY;
        u64 admitted_charge = 0;
        if (!add(domain->backing_bytes, domain->reserved_bytes, admitted_charge)) return MGMEM_OVERFLOW;
        if (admitted_charge > domain->budget) return MGMEM_OUT_OF_BUDGET;
        if (!counters_fit(*domain, bytes, false)) return MGMEM_OVERFLOW;
        u64 id = 0;
        if (!take_id(*domain, id)) return MGMEM_OVERFLOW;
        if (domain->mode == MGMEM_ARENA) {
            // Power-of-two alignments are nested: a skipped block that cannot
            // fit the cached request cannot fit this larger, stricter request.
            const bool can_resume_search = domain->arena_search_valid &&
                bytes >= domain->arena_search_bytes && alignment >= domain->arena_search_alignment;
            auto candidate = can_resume_search ? domain->arena_search : domain->backing.begin();
            for (; candidate != domain->backing.end(); ++candidate) {
                auto &backing = *candidate;
                if (backing.external || backing.pending || backing.alignment < alignment) continue;
                u64 offset = 0;
                if (!round_up(backing.used, alignment, offset) || offset > backing.bytes ||
                    bytes > backing.bytes - offset) continue;
                auto &insertion = domain->allocations.insert_fresh(id,
                    Allocation{&backing, offset, bytes, 0, false, false, false});
                commit_allocation(*domain, insertion, bytes);
                backing.used = offset + bytes;
                domain->arena_search_valid = true;
                domain->arena_search_bytes = bytes;
                domain->arena_search_alignment = alignment;
                domain->arena_search = candidate;
                *out = {domain->id, id, runtime_origin()};
                return MGMEM_OK;
            }
            domain->arena_search_valid = true;
            domain->arena_search_bytes = bytes;
            domain->arena_search_alignment = alignment;
            domain->arena_search = domain->backing.end();
        }
        if (!counters_fit(*domain, bytes, true)) return MGMEM_OVERFLOW;
        const u64 capacity = domain->mode == MGMEM_ARENA ? std::max(bytes, domain->chunk_bytes) : bytes;
        if (capacity > std::numeric_limits<size_t>::max()) return MGMEM_OVERFLOW;
        u64 committed_and_pending = 0;
        u64 total = 0;
        if (!add(domain->backing_bytes, domain->reserved_bytes, committed_and_pending) ||
            !add(committed_and_pending, capacity, total)) return MGMEM_OVERFLOW;
        if (total > domain->budget) return MGMEM_OUT_OF_BUDGET;
        if (domain->pending == max_u64) return MGMEM_OVERFLOW;

        // Allocate metadata before accepting the provider operation. A throwing
        // table insertion removes the placeholder and never reserves any budget.
        domain->backing.emplace_back(Backing{nullptr, capacity, alignment, 0, true});
        auto backing_it = std::prev(domain->backing.end());
        backing_it->node = backing_it;
        try {
            domain->allocations.insert_fresh(id,
                Allocation{&*backing_it, 0, bytes, 0, false, false, true});
        } catch (...) {
            invalidate_arena_search(*domain);
            domain->backing.erase(backing_it);
            throw;
        }
        const auto claim_status = reserve_owned_claim(*backing_it);
        if (claim_status != MGMEM_OK) {
            domain->allocations.erase(id);
            invalidate_arena_search(*domain);
            domain->backing.erase(backing_it);
            return claim_status;
        }
        domain->reserved_bytes += capacity;
        domain->peak_bytes = std::max(domain->peak_bytes, total);
        ++domain->pending;
        lock.unlock();
        void *address = nullptr;
        mgmem_status status = allocate_backing(*domain, capacity, alignment, &address);
        const bool aligned = address && (reinterpret_cast<uintptr_t>(address) & (alignment - 1)) == 0;
        if (status == MGMEM_OK && !aligned) status = MGMEM_PROVIDER_ERROR;
        lock.lock();
        // Counter headroom can have changed while a different callback was in
        // flight. Recheck before publication; rollback leaves old allocations live.
        if (status == MGMEM_OK && !counters_fit(*domain, bytes, true)) status = MGMEM_OVERFLOW;
        if (status == MGMEM_OK && domain->state != MGMEM_OPEN) status = MGMEM_IS_CLOSED;
        if (status == MGMEM_OK && domain->provider_poisoned) status = MGMEM_PROVIDER_ERROR;
        bool unsafe_address = false;
        const auto claim_status_after_callback = publish_owned_claim(*backing_it, address, capacity, unsafe_address);
        if (claim_status_after_callback != MGMEM_OK) status = claim_status_after_callback;
        backing_it->address = address;
        if (status == MGMEM_OK) {
            // A previously skipped pending block can precede the cached start.
            invalidate_arena_search(*domain);
            backing_it->pending = false;
            backing_it->used = bytes;
            domain->reserved_bytes -= capacity;
            domain->backing_bytes += capacity;
            ++domain->provider_allocation_count;
            --domain->pending;
            auto &allocation = domain->allocations.find(id)->value;
            commit_allocation(*domain, allocation, bytes);
            *out = {domain->id, id, runtime_origin()};
            notify_drain(*domain);
            return MGMEM_OK;
        }
        // Failure or close raced with admission. Reservation stays charged and
        // pending keeps the domain/provider alive through rollback deallocation.
        Backing failed = *backing_it;
        domain->allocations.erase(id);
        invalidate_arena_search(*domain);
        domain->backing.erase(backing_it);
        lock.unlock();
        // Never deallocate a provider result that aliases a live region: that
        // could free the pre-existing owner's storage. Keep its claim charged.
        const bool reclaimed = !unsafe_address && deallocate_backing(*domain, failed);
        lock.lock();
        domain->reserved_bytes -= capacity;
        --domain->pending;
        if (!reclaimed) {
            domain->provider_poisoned = true;
            domain->backing_bytes += capacity;
            status = MGMEM_PROVIDER_ERROR;
        }
        notify_drain(*domain);
        return status;
    });
}

static mgmem_status import_external_impl(mgmem_domain *domain,
    const mgmem_external_buffer_v1 *buffer,
    mgmem_status (*release_checked)(void *), bool arena_only, mgmem_handle_v1 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = {};
    if (!domain || !buffer || buffer->struct_size < sizeof(*buffer)) return MGMEM_INVALID_ARGUMENT;
    const auto input = *buffer;
    if (input.abi_major != MGMEM_ABI_MAJOR || input.abi_minor > MGMEM_ABI_MINOR ||
        input.required_flags != 0) return MGMEM_UNSUPPORTED;
    if (input.managing_runtime_origin != runtime_origin()) return MGMEM_STALE_HANDLE;
    if (!input.data || !input.capacity_bytes || !input.external_id || !input.retain ||
        (!input.release && !release_checked) || (input.release && release_checked) ||
        input.reserved || input.access > MGMEM_WRITE || !power_of_two(input.alignment))
        return MGMEM_INVALID_ARGUMENT;
    const auto base = reinterpret_cast<uintptr_t>(input.data);
    if (input.capacity_bytes > std::numeric_limits<size_t>::max() ||
        input.alignment > std::numeric_limits<size_t>::max() ||
        input.capacity_bytes > std::numeric_limits<uintptr_t>::max() - base) return MGMEM_OVERFLOW;
    if ((base & (input.alignment - 1)) != 0) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (domain->state != MGMEM_OPEN) return MGMEM_IS_CLOSED;
        if (domain->mode != (arena_only ? MGMEM_ARENA : MGMEM_SYSTEM)) return MGMEM_UNSUPPORTED;
        if (domain->provider_poisoned) return MGMEM_PROVIDER_ERROR;
        if (domain->maintenance) return MGMEM_BUSY;
        if (!counters_fit(*domain, input.capacity_bytes, false) || domain->pending == max_u64 ||
            domain->external_count == max_u64 || domain->import_count == max_u64 ||
            domain->pending >= max_u64 - domain->external_count) return MGMEM_OVERFLOW;
        u64 total = 0, charged = 0;
        if (!add(domain->backing_bytes, domain->reserved_bytes, charged) ||
            !add(charged, input.capacity_bytes, total)) return MGMEM_OVERFLOW;
        if (total > domain->budget) return MGMEM_OUT_OF_BUDGET;
        u64 id = 0;
        if (!take_id(*domain, id)) return MGMEM_OVERFLOW;
        domain->backing.emplace_back(Backing{const_cast<void *>(input.data), input.capacity_bytes,
            input.alignment, input.capacity_bytes, true, false, {}, true, input.access, input.context, input.release,
            release_checked});
        auto backing_it = std::prev(domain->backing.end());
        backing_it->node = backing_it;
        try {
            domain->allocations.insert_fresh(id,
                Allocation{&*backing_it, 0, input.capacity_bytes, 0, false, false, true});
        } catch (...) {
            invalidate_arena_search(*domain);
            domain->backing.erase(backing_it);
            throw;
        }
        const auto claimed = claim_range(input.data, input.capacity_bytes, input.external_id, *backing_it);
        if (claimed != MGMEM_OK) {
            domain->allocations.erase(id);
            invalidate_arena_search(*domain);
            domain->backing.erase(backing_it);
            return claimed;
        }
        domain->reserved_bytes += input.capacity_bytes;
        domain->pending_import_bytes += input.capacity_bytes;
        domain->peak_bytes = std::max(domain->peak_bytes, total);
        ++domain->pending;
        lock.unlock();
        mgmem_status status = MGMEM_PROVIDER_ERROR;
        bool retained = false, uncertain_retain = false;
        try { status = input.retain(input.context); retained = status == MGMEM_OK; }
        catch (...) { uncertain_retain = true; }
        lock.lock();
        if (status == MGMEM_OK && domain->state != MGMEM_OPEN) status = MGMEM_IS_CLOSED;
        if (status == MGMEM_OK && domain->provider_poisoned) status = MGMEM_PROVIDER_ERROR;
        if (status == MGMEM_OK && (!counters_fit(*domain, input.capacity_bytes, false) ||
                                  domain->import_count == max_u64 || domain->external_count == max_u64))
            status = MGMEM_OVERFLOW;
        // No budget recheck: this operation owns its reservation. A concurrent
        // soft reduction changes subsequent admission, not accepted work.
        if (status == MGMEM_OK) {
            backing_it->pending = false;
            domain->reserved_bytes -= input.capacity_bytes;
            domain->pending_import_bytes -= input.capacity_bytes;
            domain->backing_bytes += input.capacity_bytes;
            domain->external_bytes += input.capacity_bytes;
            ++domain->external_count;
            ++domain->import_count;
            --domain->pending;
            commit_allocation(*domain, domain->allocations.find(id)->value, input.capacity_bytes);
            *out = {domain->id, id, runtime_origin()};
            notify_drain(*domain);
            return MGMEM_OK;
        }
        Backing failed = *backing_it;
        domain->allocations.erase(id);
        invalidate_arena_search(*domain);
        domain->backing.erase(backing_it);
        lock.unlock();
        // Retain failure is contractually failure-atomic. If it threw, ownership
        // is unknowable; do not call release and risk freeing the caller's owner.
        bool reclaimed = !uncertain_retain;
        if (retained) reclaimed = deallocate_backing(*domain, failed);
        else if (reclaimed) drop_claim(failed);
        lock.lock();
        domain->reserved_bytes -= input.capacity_bytes;
        domain->pending_import_bytes -= input.capacity_bytes;
        --domain->pending;
        if (!reclaimed) {
            domain->provider_poisoned = true;
            domain->backing_bytes += input.capacity_bytes;
            domain->external_bytes += input.capacity_bytes;
            ++domain->external_count;
            status = MGMEM_PROVIDER_ERROR;
        }
        notify_drain(*domain);
        return status;
    });
}

mgmem_status mgmem_import_external_v1(mgmem_domain *domain,
    const mgmem_external_buffer_v1 *buffer, mgmem_handle_v1 *out) {
    return import_external_impl(domain, buffer, nullptr, false, out);
}

static mgmem_status import_external_checked_impl(mgmem_domain *domain,
    const mgmem_external_buffer_checked_v1 *buffer, bool arena_only, mgmem_handle_v1 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = {};
    if (!domain || !buffer || buffer->struct_size < sizeof(*buffer)) return MGMEM_INVALID_ARGUMENT;
    const auto input = *buffer;
    if (!input.release_checked) return MGMEM_INVALID_ARGUMENT;
    const mgmem_external_buffer_v1 legacy{
        sizeof(mgmem_external_buffer_v1), input.abi_major, input.abi_minor, input.required_flags,
        input.data, input.capacity_bytes, input.alignment, input.external_id, input.access,
        input.reserved, input.context, input.retain, nullptr, input.managing_runtime_origin};
    return import_external_impl(domain, &legacy, input.release_checked, arena_only, out);
}

mgmem_status mgmem_import_external_checked_v1(mgmem_domain *domain,
    const mgmem_external_buffer_checked_v1 *buffer, mgmem_handle_v1 *out) {
    return import_external_checked_impl(domain, buffer, false, out);
}

mgmem_status mgmem_import_external_arena_v1(mgmem_domain *domain,
    const mgmem_external_buffer_v1 *buffer, mgmem_handle_v1 *out) {
    return import_external_impl(domain, buffer, nullptr, true, out);
}

mgmem_status mgmem_import_external_arena_checked_v1(mgmem_domain *domain,
    const mgmem_external_buffer_checked_v1 *buffer, mgmem_handle_v1 *out) {
    return import_external_checked_impl(domain, buffer, true, out);
}

mgmem_status mgmem_domain_set_budget_v1(mgmem_domain *domain, u64 bytes) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::lock_guard<std::mutex> lock(domain->mutex);
        if (domain->state != MGMEM_OPEN) return MGMEM_IS_CLOSED;
        if (domain->provider_poisoned) return MGMEM_PROVIDER_ERROR;
        domain->budget = bytes;
        notify_drain(*domain);
        return MGMEM_OK;
    });
}

mgmem_status mgmem_release_v1(mgmem_domain *domain, mgmem_handle_v1 handle) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (!valid_handle(*domain, handle)) return MGMEM_STALE_HANDLE;
        auto found = domain->allocations.find(handle.id);
        if (!found || !found->value.owner || found->value.pending)
            return MGMEM_STALE_HANDLE;
        auto &allocation = found->value;
        allocation.owner = false;
        domain->live_bytes -= allocation.bytes;
        if (allocation.readers != 0 || allocation.writer) {
            domain->retiring_bytes += allocation.bytes;
            return MGMEM_OK;
        }
        Backing *backing = allocation.backing;
        domain->allocations.erase_found(found);
        notify_drain(*domain);
        return domain->mode == MGMEM_SYSTEM || backing->external
            ? reclaim_standalone(*domain, backing, lock) : MGMEM_OK;
    });
}

mgmem_status mgmem_borrow_acquire_v1(mgmem_domain *domain, mgmem_handle_v1 handle,
    u64 offset, u64 length, uint32_t access, mgmem_borrow_v1 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = {};
    if (!domain || length == 0 || access > MGMEM_WRITE) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::lock_guard<std::mutex> lock(domain->mutex);
        if (domain->state != MGMEM_OPEN) return MGMEM_IS_CLOSED;
        if (!valid_handle(*domain, handle)) return MGMEM_STALE_HANDLE;
        auto found = domain->allocations.find(handle.id);
        if (!found || !found->value.owner || found->value.pending)
            return MGMEM_STALE_HANDLE;
        auto &allocation = found->value;
        if (access == MGMEM_WRITE && allocation.backing->external && allocation.backing->access != MGMEM_WRITE)
            return MGMEM_UNSUPPORTED;
        if (offset > allocation.bytes || length > allocation.bytes - offset)
            return MGMEM_INVALID_ARGUMENT;
        if (allocation.writer || (access == MGMEM_WRITE && allocation.readers != 0)) return MGMEM_BUSY;
        if (allocation.readers == max_u64) return MGMEM_OVERFLOW;
        u64 id = 0;
        if (!take_id(*domain, id)) return MGMEM_OVERFLOW;
        domain->leases.insert_fresh(id, Lease{handle.id, access == MGMEM_WRITE});
        if (access == MGMEM_WRITE) allocation.writer = true;
        else ++allocation.readers;
        out->lease = {domain->id, id, runtime_origin()};
        out->data = static_cast<unsigned char *>(allocation.backing->address) + allocation.offset + offset;
        out->length = length;
        return MGMEM_OK;
    });
}

mgmem_status mgmem_borrow_release_v1(mgmem_domain *domain, mgmem_handle_v1 handle) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (!valid_handle(*domain, handle)) return MGMEM_STALE_HANDLE;
        const auto record = domain->leases.find(handle.id);
        if (!record) return MGMEM_STALE_HANDLE;
        const auto found = domain->allocations.find(record->value.allocation);
        if (!found) return MGMEM_PROVIDER_ERROR;
        auto &allocation = found->value;
        if (record->value.writer) allocation.writer = false;
        else --allocation.readers;
        domain->leases.erase_found(record);
        if (!allocation.owner && allocation.readers == 0 && !allocation.writer) {
            Backing *backing = allocation.backing;
            domain->retiring_bytes -= allocation.bytes;
            domain->allocations.erase_found(found);
            notify_drain(*domain);
            return domain->mode == MGMEM_SYSTEM || backing->external
                ? reclaim_standalone(*domain, backing, lock) : MGMEM_OK;
        }
        notify_drain(*domain);
        return MGMEM_OK;
    });
}

mgmem_status mgmem_arena_reset_v1(mgmem_domain *domain) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::lock_guard<std::mutex> lock(domain->mutex);
        if (domain->state != MGMEM_OPEN) return MGMEM_IS_CLOSED;
        if (domain->mode != MGMEM_ARENA) return MGMEM_UNSUPPORTED;
        if (domain->provider_poisoned) return MGMEM_PROVIDER_ERROR;
        if (!quiet(*domain)) return MGMEM_BUSY;
        if (domain->generation == max_u64) return MGMEM_OVERFLOW;
        invalidate_arena_search(*domain);
        for (auto &backing : domain->backing) if (!backing.external) backing.used = 0;
        ++domain->generation;
        return MGMEM_OK;
    });
}

mgmem_status mgmem_domain_trim_v1(mgmem_domain *domain) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (domain->state != MGMEM_OPEN) return MGMEM_IS_CLOSED;
        if (!quiet(*domain)) return MGMEM_BUSY;
        return reclaim_all(*domain, lock, false);
    });
}

mgmem_status mgmem_domain_begin_close_v1(mgmem_domain *domain) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::lock_guard<std::mutex> lock(domain->mutex);
        if (domain->state == MGMEM_OPEN) domain->state = MGMEM_CLOSING;
        notify_drain(*domain);
        return MGMEM_OK;
    });
}

mgmem_status mgmem_domain_drain_v1(mgmem_domain *domain, u64 timeout_ms) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    using milliseconds = std::chrono::milliseconds;
    using clock = std::chrono::steady_clock;
    // Bound both the duration conversion and deadline addition.
    const auto maximum = std::chrono::duration_cast<milliseconds>(clock::duration::max() / 4).count();
    if (timeout_ms > static_cast<u64>(maximum)) return MGMEM_OVERFLOW;
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (domain->state == MGMEM_OPEN) return MGMEM_INVALID_ARGUMENT;
        if (domain->state == MGMEM_CLOSED) return MGMEM_OK;
        if (domain->provider_poisoned) return MGMEM_PROVIDER_ERROR;
        if (timeout_ms == 0) return MGMEM_TIMEOUT;
        const auto deadline = clock::now() + milliseconds(timeout_ms);
        if (!domain->changed.wait_until(lock, deadline, [&] {
                return domain->state == MGMEM_CLOSED || domain->provider_poisoned;
            })) return MGMEM_TIMEOUT;
        if (domain->provider_poisoned) return MGMEM_PROVIDER_ERROR;
        return MGMEM_OK;
    });
}

mgmem_status mgmem_domain_collect_v1(mgmem_domain *domain) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::unique_lock<std::mutex> lock(domain->mutex);
        if (domain->state == MGMEM_OPEN) return MGMEM_INVALID_ARGUMENT;
        if (domain->state == MGMEM_CLOSED) return MGMEM_OK;
        if (!quiet(*domain)) return MGMEM_BUSY;
        return reclaim_all(*domain, lock, true);
    });
}

mgmem_status mgmem_domain_destroy_v1(mgmem_domain *domain) {
    if (!domain) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        {
            std::lock_guard<std::mutex> lock(domain->mutex);
            if (domain->state != MGMEM_CLOSED || !quiet(*domain) || domain->backing_bytes != 0)
                return MGMEM_BUSY;
        }
        // Caller serializes destruction with all other APIs; callbacks stay outside
        // the lock and the provider context survives every backing release.
        if (domain->custom_provider) {
            const auto provider = domain->provider;
            domain->custom_provider = false;
            try {
                provider.release(provider.context);
            } catch (...) {
                delete domain;
                return MGMEM_PROVIDER_ERROR;
            }
        }
        delete domain;
        return MGMEM_OK;
    });
}

mgmem_status mgmem_domain_snapshot_v1(mgmem_domain *domain, mgmem_snapshot_v1 *out,
                                      uint32_t out_size) {
    if (!domain || !out || out_size < sizeof(*out)) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::lock_guard<std::mutex> lock(domain->mutex);
        mgmem_snapshot_v1 result{};
        result.struct_size = sizeof(result);
        result.abi_major = MGMEM_ABI_MAJOR;
        result.abi_minor = MGMEM_ABI_MINOR;
        result.state = domain->state;
        result.domain_id = domain->id;
        result.generation = domain->generation;
        result.budget_bytes = domain->budget;
        result.backing_bytes = domain->backing_bytes;
        result.reserved_bytes = domain->reserved_bytes;
        result.peak_backing_bytes = domain->peak_bytes;
        result.live_bytes = domain->live_bytes;
        result.retiring_bytes = domain->retiring_bytes;
        result.retained_bytes = domain->backing_bytes - domain->live_bytes - domain->retiring_bytes;
        result.allocation_count = domain->allocation_count;
        result.provider_allocation_count = domain->provider_allocation_count;
        result.cumulative_requested_bytes = domain->cumulative_requested;
        result.active_allocations = domain->allocations.size();
        result.active_leases = domain->leases.size();
        result.page_size = domain->page;
        *out = result;
        return MGMEM_OK;
    });
}

mgmem_status mgmem_domain_import_snapshot_v1(mgmem_domain *domain, mgmem_import_snapshot_v1 *out,
                                             uint32_t out_size) {
    if (!domain || !out || out_size < sizeof(*out)) return MGMEM_INVALID_ARGUMENT;
    return guarded([&]() -> mgmem_status {
        std::lock_guard<std::mutex> lock(domain->mutex);
        const mgmem_import_snapshot_v1 result{sizeof(mgmem_import_snapshot_v1), MGMEM_ABI_MAJOR,
            MGMEM_ABI_MINOR, domain->provider_poisoned ? 1u : 0u,
            domain->external_bytes, domain->external_count, domain->pending_import_bytes, domain->import_count};
        *out = result;
        return MGMEM_OK;
    });
}
} // extern "C"
