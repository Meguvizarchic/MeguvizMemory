#include <mgmem/memory.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
std::atomic<unsigned> checks{0};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr,"FAIL import:%d: %s\n",__LINE__,#value); std::abort(); } } while(false)
std::atomic<uint64_t> next_external{1};

mgmem_domain_options_v1 options(uint32_t mode=MGMEM_SYSTEM, uint64_t budget=512) {
    return {sizeof(mgmem_domain_options_v1),1,0,0,mode,0,budget,128,nullptr};
}
mgmem_domain *make(uint32_t mode=MGMEM_SYSTEM,uint64_t budget=512) {
    auto o=options(mode,budget);mgmem_domain *d=nullptr;
    CHECK(mgmem_domain_create_v1(&o,&d)==MGMEM_OK);return d;
}
mgmem_snapshot_v1 snapshot(mgmem_domain *d) {
    mgmem_snapshot_v1 s{};CHECK(mgmem_domain_snapshot_v1(d,&s,sizeof(s))==MGMEM_OK);
    CHECK(s.backing_bytes==s.live_bytes+s.retiring_bytes+s.retained_bytes);return s;
}
mgmem_import_snapshot_v1 imported(mgmem_domain *d) {
    mgmem_import_snapshot_v1 s{};CHECK(mgmem_domain_import_snapshot_v1(d,&s,sizeof(s))==MGMEM_OK);return s;
}
void finish(mgmem_domain *d) {
    CHECK(mgmem_domain_begin_close_v1(d)==MGMEM_OK);CHECK(mgmem_domain_collect_v1(d)==MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(d,0)==MGMEM_OK);CHECK(snapshot(d).backing_bytes==0);
    CHECK(imported(d).external_backing_bytes==0);CHECK(mgmem_domain_destroy_v1(d)==MGMEM_OK);
}
struct Owner {
    unsigned char *data=nullptr;
    uint64_t capacity;
    uint64_t identity=next_external.fetch_add(1);
    std::atomic<unsigned> refs{1}, retains{0}, releases{0};
    std::atomic<bool> other_release{false};
    std::thread::id creator=std::this_thread::get_id();
    std::mutex mutex;std::condition_variable changed;
    bool block_retain=false,block_release=false,entered=false,proceed=false;
    bool throw_retain=false,throw_release=false;
    mgmem_status failure=MGMEM_OK;
    explicit Owner(uint64_t bytes=64) : capacity(bytes) {
#if defined(_WIN32)
        data=static_cast<unsigned char *>(_aligned_malloc(static_cast<size_t>(bytes),64));
#else
        void *address=nullptr;if(posix_memalign(&address,64,static_cast<size_t>(bytes))==0)data=static_cast<unsigned char *>(address);
#endif
        CHECK(data!=nullptr);std::memset(data,0,static_cast<size_t>(bytes));
    }
    ~Owner() {
        CHECK(refs==1);
#if defined(_WIN32)
        _aligned_free(data);
#else
        std::free(data);
#endif
    }
    static mgmem_status retain(void *p) {
        auto &o=*static_cast<Owner *>(p);++o.retains;
        std::unique_lock<std::mutex> lock(o.mutex);
        if(o.block_retain) { o.entered=true;o.changed.notify_all();o.changed.wait(lock,[&]{return o.proceed;}); }
        if(o.throw_retain) throw std::runtime_error("injected retain violation");
        if(o.failure!=MGMEM_OK)return o.failure;
        ++o.refs;return MGMEM_OK;
    }
    static void release(void *p) {
        auto &o=*static_cast<Owner *>(p);++o.releases;
        o.other_release=std::this_thread::get_id()!=o.creator;
        std::unique_lock<std::mutex> lock(o.mutex);
        if(o.block_release) { o.entered=true;o.changed.notify_all();o.changed.wait(lock,[&]{return o.proceed;}); }
        if(o.throw_release)throw std::runtime_error("injected release violation");
        --o.refs;
    }
    void wait() { std::unique_lock<std::mutex> lock(mutex);CHECK(changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered;})); }
    void unblock() {std::lock_guard<std::mutex> lock(mutex);proceed=true;changed.notify_all();}
    mgmem_external_buffer_v1 table(uint64_t bytes=0,uint32_t access=MGMEM_WRITE) {
        return {sizeof(mgmem_external_buffer_v1),1,1,0,data,bytes ? bytes : capacity,64,identity,access,0,this,retain,release,mgmem_runtime_origin_v1()};
    }
};
mgmem_handle_v1 acquire(mgmem_domain *d,Owner &o,uint64_t bytes=64,uint32_t access=MGMEM_WRITE) {
    auto b=o.table(bytes,access);mgmem_handle_v1 h{};CHECK(mgmem_import_external_v1(d,&b,&h)==MGMEM_OK);return h;
}

void abi_validation_and_failure_atomicity() {
    CHECK((mgmem_abi_version_v1()&65535u)==1);CHECK((mgmem_capabilities_v1()&MGMEM_CAP_EXTERNAL_IMPORT)!=0);
    auto *d=make();Owner o;auto b=o.table();mgmem_handle_v1 out{1,2,3};
    auto bad=b;bad.struct_size=0;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_INVALID_ARGUMENT && out.id==0);
    bad=b;bad.abi_minor=2;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_UNSUPPORTED);
    bad=b;bad.required_flags=1;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_UNSUPPORTED);
    bad=b;bad.managing_runtime_origin=0;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_STALE_HANDLE);
    bad=b;bad.managing_runtime_origin^=UINT64_C(0x5555);CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_STALE_HANDLE);
    bad=b;bad.external_id=0;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_INVALID_ARGUMENT);
    bad=b;bad.data=nullptr;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_INVALID_ARGUMENT);
    bad=b;bad.alignment=3;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_INVALID_ARGUMENT);
    bad=b;bad.data=o.data+1;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_INVALID_ARGUMENT);
    bad=b;bad.capacity_bytes=0;CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_INVALID_ARGUMENT);
    bad=b;bad.data=reinterpret_cast<void *>(std::numeric_limits<uintptr_t>::max()-7);bad.alignment=1;bad.capacity_bytes=16;
    CHECK(mgmem_import_external_v1(d,&bad,&out)==MGMEM_OVERFLOW);
    CHECK(o.retains==0 && o.releases==0 && snapshot(d).backing_bytes==0);
    auto *arena=make(MGMEM_ARENA);CHECK(mgmem_import_external_v1(arena,&b,&out)==MGMEM_UNSUPPORTED);finish(arena);
    CHECK(mgmem_import_external_v1(d,&b,nullptr)==MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_domain_set_budget_v1(d,32)==MGMEM_OK);
    CHECK(mgmem_import_external_v1(d,&b,&out)==MGMEM_OUT_OF_BUDGET && o.retains==0);
    CHECK(mgmem_domain_set_budget_v1(d,512)==MGMEM_OK);o.failure=MGMEM_OUT_OF_MEMORY;
    CHECK(mgmem_import_external_v1(d,&b,&out)==MGMEM_OUT_OF_MEMORY && out.id==0);
    CHECK(o.retains==1 && o.releases==0 && o.refs==1 && snapshot(d).reserved_bytes==0);
    o.failure=MGMEM_OK;auto h=acquire(d,o);CHECK(mgmem_release_v1(d,h)==MGMEM_OK);finish(d);CHECK(o.refs==1);
}

void readonly_views_identity_and_unique_charge() {
    auto *a=make(),*b=make();Owner o(128);auto h=acquire(a,o,128,MGMEM_READ);
    auto stats=imported(a);CHECK(stats.external_backing_bytes==128 && stats.external_backing_count==1 && stats.import_count==1);
    CHECK(snapshot(a).provider_allocation_count==0 && snapshot(a).allocation_count==1);
    auto descriptor=o.table(128,MGMEM_READ);mgmem_handle_v1 rejected{};
    CHECK(mgmem_import_external_v1(a,&descriptor,&rejected)==MGMEM_BUSY);
    CHECK(mgmem_import_external_v1(b,&descriptor,&rejected)==MGMEM_BUSY);
    descriptor.external_id+=10000;descriptor.data=o.data+64;descriptor.capacity_bytes=64;
    CHECK(mgmem_import_external_v1(b,&descriptor,&rejected)==MGMEM_BUSY && o.retains==1);
    mgmem_borrow_v1 first{},second{},writer{};
    CHECK(mgmem_borrow_acquire_v1(a,h,1,63,MGMEM_READ,&first)==MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(a,h,64,64,MGMEM_READ,&second)==MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(a,h,0,1,MGMEM_WRITE,&writer)==MGMEM_UNSUPPORTED);
    CHECK(mgmem_borrow_acquire_v1(a,h,127,2,MGMEM_READ,&writer)==MGMEM_INVALID_ARGUMENT);
    CHECK(snapshot(a).backing_bytes==128 && imported(a).external_backing_count==1);
    CHECK(mgmem_release_v1(a,h)==MGMEM_OK);CHECK(snapshot(a).retiring_bytes==128 && o.releases==0);
    CHECK(mgmem_domain_begin_close_v1(a)==MGMEM_OK);CHECK(mgmem_domain_collect_v1(a)==MGMEM_BUSY);
    CHECK(mgmem_domain_drain_v1(a,0)==MGMEM_TIMEOUT);
    CHECK(mgmem_borrow_release_v1(a,first.lease)==MGMEM_OK && o.releases==0);
    std::thread last([&]{CHECK(mgmem_borrow_release_v1(a,second.lease)==MGMEM_OK);});last.join();
    CHECK(o.releases==1 && o.refs==1 && o.other_release);
    CHECK(mgmem_borrow_release_v1(a,second.lease)==MGMEM_STALE_HANDLE);finish(a);
    auto again=acquire(b,o,128);CHECK(mgmem_release_v1(b,again)==MGMEM_OK);finish(b);
}

void owned_alias_and_whole_region_writer() {
    auto *a=make(),*b=make();mgmem_handle_v1 owned{};CHECK(mgmem_allocate_v1(a,128,64,&owned)==MGMEM_OK);
    mgmem_borrow_v1 read{};CHECK(mgmem_borrow_acquire_v1(a,owned,0,128,MGMEM_READ,&read)==MGMEM_OK);
    Owner o(128);auto descriptor=o.table();descriptor.data=read.data;mgmem_handle_v1 invalid{};
    CHECK(mgmem_import_external_v1(b,&descriptor,&invalid)==MGMEM_BUSY && o.retains==0);
    CHECK(mgmem_borrow_release_v1(a,read.lease)==MGMEM_OK);CHECK(mgmem_release_v1(a,owned)==MGMEM_OK);finish(a);
    auto h=acquire(b,o,128);mgmem_borrow_v1 writer{},other{};
    CHECK(mgmem_borrow_acquire_v1(b,h,0,1,MGMEM_WRITE,&writer)==MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(b,h,127,1,MGMEM_READ,&other)==MGMEM_BUSY);
    CHECK(mgmem_borrow_release_v1(b,writer.lease)==MGMEM_OK);CHECK(mgmem_release_v1(b,h)==MGMEM_OK);finish(b);
}

void pending_budget_and_close_races() {
    Owner o;o.block_retain=true;auto *d=make(MGMEM_SYSTEM,64);auto desc=o.table();mgmem_handle_v1 h{};mgmem_status status=MGMEM_BUSY;
    std::thread worker([&]{status=mgmem_import_external_v1(d,&desc,&h);});o.wait();
    CHECK(snapshot(d).reserved_bytes==64 && imported(d).pending_import_bytes==64);
    mgmem_handle_v1 rejected{};CHECK(mgmem_allocate_v1(d,1,1,&rejected)==MGMEM_OUT_OF_BUDGET);
    CHECK(mgmem_domain_set_budget_v1(d,0)==MGMEM_OK);o.unblock();worker.join();
    CHECK(status==MGMEM_OK && snapshot(d).backing_bytes==64 && snapshot(d).budget_bytes==0);
    CHECK(mgmem_allocate_v1(d,1,1,&rejected)==MGMEM_OUT_OF_BUDGET);
    CHECK(mgmem_release_v1(d,h)==MGMEM_OK);CHECK(mgmem_domain_set_budget_v1(d,64)==MGMEM_OK);
    CHECK(mgmem_allocate_v1(d,64,16,&h)==MGMEM_OK);CHECK(mgmem_release_v1(d,h)==MGMEM_OK);finish(d);
    Owner closing;closing.block_retain=true;d=make();desc=closing.table();status=MGMEM_BUSY;h={};
    std::thread closer([&]{status=mgmem_import_external_v1(d,&desc,&h);});closing.wait();
    CHECK(mgmem_domain_begin_close_v1(d)==MGMEM_OK);CHECK(mgmem_domain_collect_v1(d)==MGMEM_BUSY);
    CHECK(mgmem_domain_drain_v1(d,0)==MGMEM_TIMEOUT);closing.unblock();closer.join();
    CHECK(status==MGMEM_IS_CLOSED && h.id==0 && closing.refs==1 && closing.retains==closing.releases);
    CHECK(imported(d).import_count==0 && snapshot(d).backing_bytes==0 && snapshot(d).reserved_bytes==0);finish(d);
}

struct PendingProvider {
    alignas(64) std::array<unsigned char,128> bytes{};
    std::mutex mutex;std::condition_variable changed;
    bool entered=false,proceed=false;
    unsigned retains=0,releases=0,deallocations=0;
    static mgmem_status allocate(void *p,uint64_t,uint64_t,void **out) {
        auto &self=*static_cast<PendingProvider *>(p);std::unique_lock<std::mutex> lock(self.mutex);
        self.entered=true;self.changed.notify_all();self.changed.wait(lock,[&]{return self.proceed;});
        *out=self.bytes.data();return MGMEM_OK;
    }
    static void deallocate(void *p,void *,uint64_t,uint64_t) {++static_cast<PendingProvider *>(p)->deallocations;}
    static void retain(void *p) {++static_cast<PendingProvider *>(p)->retains;}
    static void release(void *p) {++static_cast<PendingProvider *>(p)->releases;}
};
void pending_provider_survives_soft_reduction() {
    PendingProvider provider;mgmem_provider_v1 table{sizeof(table),1,0,0,&provider,
        PendingProvider::allocate,PendingProvider::deallocate,PendingProvider::retain,PendingProvider::release};
    auto opts=options(MGMEM_SYSTEM,128);opts.provider=&table;mgmem_domain *d=nullptr;
    CHECK(mgmem_domain_create_v1(&opts,&d)==MGMEM_OK);
    mgmem_handle_v1 h{};mgmem_status status=MGMEM_BUSY;
    std::thread worker([&]{status=mgmem_allocate_v1(d,128,64,&h);});
    {std::unique_lock<std::mutex> lock(provider.mutex);CHECK(provider.changed.wait_for(lock,std::chrono::seconds(5),[&]{return provider.entered;}));}
    CHECK(snapshot(d).reserved_bytes==128);CHECK(mgmem_domain_set_budget_v1(d,0)==MGMEM_OK);
    {std::lock_guard<std::mutex> lock(provider.mutex);provider.proceed=true;provider.changed.notify_all();}
    worker.join();CHECK(status==MGMEM_OK && snapshot(d).backing_bytes==128 && snapshot(d).budget_bytes==0);
    mgmem_handle_v1 fail{};CHECK(mgmem_allocate_v1(d,1,1,&fail)==MGMEM_OUT_OF_BUDGET);
    CHECK(mgmem_release_v1(d,h)==MGMEM_OK);finish(d);
    CHECK(provider.retains==1 && provider.releases==1 && provider.deallocations==1);
}

void release_keeps_charge_and_claim_until_callback() {
    auto *d=make(MGMEM_SYSTEM,64),*other=make();Owner o;o.block_release=true;auto h=acquire(d,o);mgmem_status status=MGMEM_BUSY;
    std::thread worker([&]{status=mgmem_release_v1(d,h);});o.wait();
    CHECK(snapshot(d).backing_bytes==64 && imported(d).external_backing_bytes==64);
    mgmem_handle_v1 fail{};auto desc=o.table();CHECK(mgmem_import_external_v1(other,&desc,&fail)==MGMEM_BUSY);
    CHECK(mgmem_allocate_v1(d,1,1,&fail)==MGMEM_OUT_OF_BUDGET);
    CHECK(mgmem_domain_begin_close_v1(d)==MGMEM_OK);CHECK(mgmem_domain_collect_v1(d)==MGMEM_BUSY);
    o.unblock();worker.join();CHECK(status==MGMEM_OK && o.refs==1);finish(d);finish(other);
}

void soft_arena_limit_and_cpp_raii() {
    auto *d=make(MGMEM_ARENA,128);mgmem_handle_v1 h{};CHECK(mgmem_allocate_v1(d,16,16,&h)==MGMEM_OK);
    CHECK(mgmem_release_v1(d,h)==MGMEM_OK);CHECK(mgmem_arena_reset_v1(d)==MGMEM_OK);
    CHECK(mgmem_domain_set_budget_v1(d,0)==MGMEM_OK);CHECK(mgmem_allocate_v1(d,16,16,&h)==MGMEM_OUT_OF_BUDGET);
    CHECK(mgmem_domain_trim_v1(d)==MGMEM_OK);CHECK(snapshot(d).backing_bytes==0);finish(d);
    Owner o;mgmem::borrow late;
    { mgmem::domain owner;auto opts=options();CHECK(mgmem::domain::create(opts,owner)==MGMEM_OK);
      auto unbound=o.table(64,MGMEM_READ);unbound.managing_runtime_origin=0;
      mgmem::owned_allocation imported_owner;CHECK(owner.import_external(unbound,imported_owner)==MGMEM_STALE_HANDLE && !imported_owner && o.retains==0);
      unbound.managing_runtime_origin=mgmem_runtime_origin_v1()^UINT64_C(0x5555);
      CHECK(owner.import_external(unbound,imported_owner)==MGMEM_STALE_HANDLE && !imported_owner && o.retains==0);
      unbound.managing_runtime_origin=mgmem_runtime_origin_v1();CHECK(owner.import_external(unbound,imported_owner)==MGMEM_OK);
      CHECK(imported_owner.acquire_borrow(0,64,MGMEM_READ,late)==MGMEM_OK && late.write_data()==nullptr);
      CHECK(imported_owner.release()==MGMEM_OK);CHECK(owner.set_budget(0)==MGMEM_OK);CHECK(owner.begin_close()==MGMEM_OK); }
    std::thread worker([lease=std::move(late)]() mutable {CHECK(lease.release()==MGMEM_OK);});worker.join();
    CHECK(o.refs==1 && o.releases==1 && o.other_release);
}

void arena_import_budget_close_and_dedup() {
    CHECK((mgmem_capabilities_v1()&MGMEM_CAP_ARENA_EXTERNAL_IMPORT)!=0);
    auto *arena=make(MGMEM_ARENA,128),*other=make(MGMEM_ARENA,128);
    Owner source(64);source.data[0]=0x5a;auto b=source.table(64,MGMEM_READ);mgmem_handle_v1 h{},denied{};
    auto *system=make();CHECK(mgmem_import_external_arena_v1(system,&b,&denied)==MGMEM_UNSUPPORTED && source.retains==0);finish(system);
    CHECK(mgmem_import_external_v1(arena,&b,&denied)==MGMEM_UNSUPPORTED && source.retains==0);
    CHECK(mgmem_import_external_arena_v1(arena,&b,&h)==MGMEM_OK && source.retains==1 && source.refs==2);
    CHECK(snapshot(arena).backing_bytes==64 && imported(arena).external_backing_bytes==64);
    CHECK(mgmem_import_external_arena_v1(other,&b,&denied)==MGMEM_BUSY && source.retains==1);
    mgmem_borrow_v1 first{},second{},writer{};
    CHECK(mgmem_borrow_acquire_v1(arena,h,0,32,MGMEM_READ,&first)==MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(arena,h,0,64,MGMEM_READ,&second)==MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(arena,h,0,1,MGMEM_WRITE,&writer)==MGMEM_UNSUPPORTED);
    CHECK(mgmem_arena_reset_v1(arena)==MGMEM_BUSY && mgmem_domain_trim_v1(arena)==MGMEM_BUSY);
    CHECK(mgmem_domain_set_budget_v1(arena,0)==MGMEM_OK);
    Owner distinct(64);auto d=distinct.table();CHECK(mgmem_import_external_arena_v1(arena,&d,&denied)==MGMEM_OUT_OF_BUDGET);
    CHECK(distinct.retains==0 && static_cast<const unsigned char *>(first.data)[0]==0x5a);
    CHECK(mgmem_release_v1(arena,h)==MGMEM_OK && source.releases==0 && snapshot(arena).retiring_bytes==64);
    CHECK(mgmem_domain_begin_close_v1(arena)==MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(arena,h,0,1,MGMEM_READ,&writer)==MGMEM_IS_CLOSED);
    CHECK(mgmem_import_external_arena_v1(arena,&d,&denied)==MGMEM_IS_CLOSED);
    CHECK(mgmem_domain_collect_v1(arena)==MGMEM_BUSY && mgmem_domain_drain_v1(arena,0)==MGMEM_TIMEOUT);
    CHECK(static_cast<const unsigned char *>(second.data)[0]==0x5a);
    CHECK(mgmem_borrow_release_v1(arena,first.lease)==MGMEM_OK && source.releases==0);
    CHECK(mgmem_borrow_release_v1(arena,second.lease)==MGMEM_OK && source.releases==1 && source.refs==1);
    CHECK(mgmem_borrow_release_v1(arena,second.lease)==MGMEM_STALE_HANDLE);
    CHECK(imported(arena).external_backing_bytes==0 && snapshot(arena).backing_bytes==0);finish(arena);
    CHECK(mgmem_import_external_arena_v1(other,&b,&h)==MGMEM_OK);
    CHECK(mgmem_release_v1(other,h)==MGMEM_OK && source.releases==2 && source.refs==1);
    CHECK(imported(other).external_backing_count==0 && snapshot(other).backing_bytes==0);finish(other);
}

void arena_import_does_not_supply_arena_storage() {
    auto *d=make(MGMEM_ARENA,256);Owner source(64);auto b=source.table();mgmem_handle_v1 external{},owned{};
    CHECK(mgmem_import_external_arena_v1(d,&b,&external)==MGMEM_OK);
    CHECK(mgmem_allocate_v1(d,32,16,&owned)==MGMEM_OK);
    mgmem_borrow_v1 v{};CHECK(mgmem_borrow_acquire_v1(d,owned,0,32,MGMEM_WRITE,&v)==MGMEM_OK);
    CHECK(v.data!=source.data && snapshot(d).backing_bytes==192);
    CHECK(mgmem_borrow_release_v1(d,v.lease)==MGMEM_OK && mgmem_release_v1(d,owned)==MGMEM_OK);
    CHECK(mgmem_arena_reset_v1(d)==MGMEM_BUSY && mgmem_domain_trim_v1(d)==MGMEM_BUSY);
    CHECK(mgmem_release_v1(d,external)==MGMEM_OK && source.releases==1);
    CHECK(imported(d).external_backing_bytes==0 && snapshot(d).backing_bytes==128);
    CHECK(mgmem_arena_reset_v1(d)==MGMEM_OK && snapshot(d).backing_bytes==128);
    CHECK(mgmem_domain_trim_v1(d)==MGMEM_OK && snapshot(d).backing_bytes==0);finish(d);
}

void arena_import_pending_reservation_and_close() {
    Owner o;o.block_retain=true;auto *d=make(MGMEM_ARENA,64),*other=make(MGMEM_ARENA,64);
    auto b=o.table();mgmem_handle_v1 h{},denied{};mgmem_status status=MGMEM_BUSY;
    std::thread worker([&]{status=mgmem_import_external_arena_v1(d,&b,&h);});o.wait();
    CHECK(imported(d).pending_import_bytes==64 && snapshot(d).reserved_bytes==64);
    CHECK(mgmem_import_external_arena_v1(other,&b,&denied)==MGMEM_BUSY && o.retains==1);
    CHECK(mgmem_arena_reset_v1(d)==MGMEM_BUSY && mgmem_domain_trim_v1(d)==MGMEM_BUSY);
    CHECK(mgmem_domain_set_budget_v1(d,0)==MGMEM_OK);o.unblock();worker.join();
    CHECK(status==MGMEM_OK && snapshot(d).budget_bytes==0 && snapshot(d).backing_bytes==64);
    CHECK(mgmem_release_v1(d,h)==MGMEM_OK && o.releases==1 && o.refs==1);finish(d);finish(other);
    Owner closing;closing.block_retain=true;d=make(MGMEM_ARENA);b=closing.table();h={};
    std::thread pending([&]{status=mgmem_import_external_arena_v1(d,&b,&h);});closing.wait();
    CHECK(mgmem_domain_begin_close_v1(d)==MGMEM_OK && mgmem_domain_collect_v1(d)==MGMEM_BUSY);
    closing.unblock();pending.join();
    CHECK(status==MGMEM_IS_CLOSED && h.id==0 && closing.retains==1 && closing.releases==1 && closing.refs==1);
    CHECK(snapshot(d).backing_bytes==0 && snapshot(d).reserved_bytes==0 && imported(d).import_count==0);finish(d);
}

void arena_checked_import_success() {
    auto *d=make(MGMEM_ARENA);Owner owner;auto b=owner.table();
    mgmem_external_buffer_checked_v1 cb{sizeof(cb),b.abi_major,b.abi_minor,b.required_flags,b.data,b.capacity_bytes,
        b.alignment,b.external_id,b.access,b.reserved,b.context,b.retain,
        [](void *p)->mgmem_status {Owner::release(p);return MGMEM_OK;},b.managing_runtime_origin};
    mgmem_handle_v1 h{},denied{};CHECK(mgmem_import_external_checked_v1(d,&cb,&denied)==MGMEM_UNSUPPORTED);
    auto *system=make();CHECK(mgmem_import_external_arena_checked_v1(system,&cb,&denied)==MGMEM_UNSUPPORTED && owner.retains==0);finish(system);
    CHECK(mgmem_import_external_arena_checked_v1(d,&cb,&h)==MGMEM_OK);
    mgmem_borrow_v1 v{};CHECK(mgmem_borrow_acquire_v1(d,h,0,64,MGMEM_READ,&v)==MGMEM_OK);
    CHECK(mgmem_release_v1(d,h)==MGMEM_OK && owner.releases==0);
    CHECK(mgmem_borrow_release_v1(d,v.lease)==MGMEM_OK && owner.releases==1 && owner.refs==1);
    CHECK(imported(d).external_backing_count==0 && snapshot(d).backing_bytes==0);finish(d);
}

void arena_import_release_callback_keeps_charge() {
    auto *d=make(MGMEM_ARENA,64),*other=make();Owner owner;owner.block_release=true;
    auto b=owner.table();mgmem_handle_v1 h{},denied{};mgmem_status status=MGMEM_BUSY;
    CHECK(mgmem_import_external_arena_v1(d,&b,&h)==MGMEM_OK);
    std::thread worker([&]{status=mgmem_release_v1(d,h);});owner.wait();
    CHECK(snapshot(d).backing_bytes==64 && imported(d).external_backing_bytes==64);
    CHECK(mgmem_import_external_v1(other,&b,&denied)==MGMEM_BUSY && owner.retains==1);
    CHECK(mgmem_allocate_v1(d,1,1,&denied)==MGMEM_OUT_OF_BUDGET);
    CHECK(mgmem_arena_reset_v1(d)==MGMEM_BUSY && mgmem_domain_trim_v1(d)==MGMEM_BUSY);
    owner.unblock();worker.join();
    CHECK(status==MGMEM_OK && owner.releases==1 && owner.refs==1 && snapshot(d).backing_bytes==0);
    CHECK(mgmem_arena_reset_v1(d)==MGMEM_OK && mgmem_domain_trim_v1(d)==MGMEM_OK);finish(d);finish(other);
}

struct AliasProvider {
    void *address;std::atomic<unsigned> deallocations{0};
    static mgmem_status allocate(void *p,uint64_t,uint64_t,void **out) { *out=static_cast<AliasProvider *>(p)->address;return MGMEM_OK; }
    static void deallocate(void *p,void *,uint64_t,uint64_t) { ++static_cast<AliasProvider *>(p)->deallocations; }
    static void retain(void *) {} static void release(void *) {}
};
void poison_case(const char *name) {
    // These are deliberately violating callbacks. Quarantine cannot safely be
    // destroyed; this isolated mode validates it then exits without pretending
    // normal ownership cleanup succeeded. Ordinary tests perform full cleanup.
    Owner o;auto *d=make(std::strcmp(name,"--poison-arena-checked")==0 ? MGMEM_ARENA : MGMEM_SYSTEM);
    auto desc=o.table();mgmem_handle_v1 h{};
    if(std::strcmp(name,"--poison-retain")==0) {
        o.throw_retain=true;CHECK(mgmem_import_external_v1(d,&desc,&h)==MGMEM_PROVIDER_ERROR && h.id==0);
        CHECK(o.releases==0 && imported(d).poisoned==1 && snapshot(d).backing_bytes==64);
    } else if(std::strcmp(name,"--poison-release")==0) {
        h=acquire(d,o);o.throw_release=true;CHECK(mgmem_release_v1(d,h)==MGMEM_PROVIDER_ERROR);
        CHECK(o.releases==1 && imported(d).poisoned==1 && snapshot(d).backing_bytes==64);
        CHECK(mgmem_release_v1(d,h)==MGMEM_STALE_HANDLE && o.releases==1);
    } else if(std::strcmp(name,"--poison-arena-checked")==0) {
        mgmem_external_buffer_checked_v1 cb{sizeof(cb),desc.abi_major,desc.abi_minor,desc.required_flags,
            desc.data,desc.capacity_bytes,desc.alignment,desc.external_id,desc.access,desc.reserved,
            desc.context,desc.retain,[](void *p)->mgmem_status {++static_cast<Owner *>(p)->releases;return MGMEM_BUSY;},
            desc.managing_runtime_origin};
        CHECK(mgmem_import_external_arena_checked_v1(d,&cb,&h)==MGMEM_OK);
        CHECK(mgmem_release_v1(d,h)==MGMEM_PROVIDER_ERROR && o.releases==1 && o.refs==2);
        CHECK(mgmem_release_v1(d,h)==MGMEM_STALE_HANDLE && o.releases==1);
        CHECK(imported(d).poisoned==1 && imported(d).external_backing_bytes==64 && snapshot(d).backing_bytes==64);
        auto *other=make(MGMEM_ARENA);mgmem_handle_v1 denied{};
        CHECK(mgmem_import_external_arena_checked_v1(other,&cb,&denied)==MGMEM_BUSY && denied.id==0);finish(other);
    } else {
        h=acquire(d,o);AliasProvider provider{const_cast<void *>(desc.data)};
        mgmem_provider_v1 table{sizeof(table),1,1,0,&provider,AliasProvider::allocate,AliasProvider::deallocate,AliasProvider::retain,AliasProvider::release};
        auto opts=options();opts.provider=&table;mgmem_domain *bad=nullptr;CHECK(mgmem_domain_create_v1(&opts,&bad)==MGMEM_OK);
        mgmem_handle_v1 invalid{};CHECK(mgmem_allocate_v1(bad,64,64,&invalid)==MGMEM_PROVIDER_ERROR && invalid.id==0);
        CHECK(provider.deallocations==0 && imported(bad).poisoned==1 && snapshot(bad).backing_bytes==64);
        CHECK(o.refs==2 && o.releases==0);CHECK(mgmem_release_v1(d,h)==MGMEM_OK && o.refs==1);finish(d);
        CHECK(mgmem_domain_begin_close_v1(bad)==MGMEM_OK && mgmem_domain_collect_v1(bad)==MGMEM_PROVIDER_ERROR);
        CHECK(mgmem_domain_destroy_v1(bad)==MGMEM_BUSY);
        auto *original=make();mgmem_handle_v1 original_handle{};
        CHECK(mgmem_allocate_v1(original,64,64,&original_handle)==MGMEM_OK);
        mgmem_borrow_v1 original_view{};CHECK(mgmem_borrow_acquire_v1(original,original_handle,0,64,MGMEM_WRITE,&original_view)==MGMEM_OK);
        std::memset(original_view.data,0x5a,64);
        AliasProvider owned_alias{original_view.data};table.context=&owned_alias;opts.provider=&table;mgmem_domain *bad_owned=nullptr;
        CHECK(mgmem_domain_create_v1(&opts,&bad_owned)==MGMEM_OK);
        CHECK(mgmem_allocate_v1(bad_owned,64,64,&invalid)==MGMEM_PROVIDER_ERROR && invalid.id==0 && owned_alias.deallocations==0);
        for(unsigned i=0;i<64;++i)CHECK(static_cast<unsigned char *>(original_view.data)[i]==0x5a);
        CHECK(mgmem_borrow_release_v1(original,original_view.lease)==MGMEM_OK);
        CHECK(mgmem_release_v1(original,original_handle)==MGMEM_OK);finish(original);
        CHECK(imported(bad_owned).poisoned==1 && snapshot(bad_owned).backing_bytes==64);
        std::printf("{\"suite\":\"mgmem-import-poison\",\"case\":\"provider_alias_external_and_owned\",\"status\":\"PASS\"}\n");std::fflush(stdout);std::_Exit(0);
    }
    CHECK(mgmem_domain_begin_close_v1(d)==MGMEM_OK && mgmem_domain_collect_v1(d)==MGMEM_PROVIDER_ERROR);
    CHECK(mgmem_domain_drain_v1(d,0)==MGMEM_PROVIDER_ERROR && mgmem_domain_destroy_v1(d)==MGMEM_BUSY);
    std::printf("{\"suite\":\"mgmem-import-poison\",\"case\":\"%s\",\"status\":\"PASS\"}\n",name);std::fflush(stdout);std::_Exit(0);
}
} // namespace

int main(int argc,char **argv) {
    if(argc==2) {
        if(std::strcmp(argv[1],"--poison-retain") && std::strcmp(argv[1],"--poison-release") &&
           std::strcmp(argv[1],"--poison-alias") && std::strcmp(argv[1],"--poison-arena-checked")) return 2;
        poison_case(argv[1]);return 1;
    }
    if(argc!=1)return 2;
    abi_validation_and_failure_atomicity();readonly_views_identity_and_unique_charge();
    owned_alias_and_whole_region_writer();pending_budget_and_close_races();
    pending_provider_survives_soft_reduction();release_keeps_charge_and_claim_until_callback();soft_arena_limit_and_cpp_raii();
    arena_import_budget_close_and_dedup();arena_import_does_not_supply_arena_storage();
    arena_import_pending_reservation_and_close();arena_checked_import_success();arena_import_release_callback_keeps_charge();
    // Preserve the v1 consumer's seven legacy groups; report new groups separately.
    std::printf("{\"suite\":\"mgmem-import-v1\",\"groups\":7,\"arena_groups\":5,\"checks\":%u,\"status\":\"PASS\"}\n",checks.load());
    return 0;
}
