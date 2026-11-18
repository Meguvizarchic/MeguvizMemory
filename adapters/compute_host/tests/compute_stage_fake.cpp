#include <mgmem/compute_stage.h>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "fail %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
static int resource, host, device, ticket, host_refs, device_refs, retains, releases, callbacks, result_mode;
static bool send_returned;
static mgmem_stage *stage;
static void error(void *, mgmem_status) { std::abort(); }
static void retain_user(void *) { ++retains; }
static void release_user(void *) { ++releases; CHECK(mgmem_stage_close(&stage) == MGMEM_BUSY); }
static void complete_user(void *, const MgcCompletion *) {
    ++callbacks; CHECK(!send_returned); CHECK(mgmem_stage_close(&stage) == MGMEM_BUSY);
}
extern "C" mgmem_status mgmem_compute_create_host_resource_v1(mgmem_domain *, mgmem_handle_v1, uint64_t,
    MgcDomain *, const MgcResourceDesc *, uint32_t, mgmem_compute_host_error_fn, void *, MgcResource **out, int32_t *origin) {
    *out = reinterpret_cast<MgcResource *>(&resource); *origin = MGC_SUCCESS; return MGMEM_OK;
}
extern "C" MgcStatus mgc_lease_create(MgcResource *, const MgcLeaseDesc *, MgcLease **out) {
    *out = reinterpret_cast<MgcLease *>(&host); host_refs = 1; return MGC_SUCCESS;
}
extern "C" void mgc_resource_release(MgcResource *) {}
extern "C" void mgc_lease_retain(MgcLease *lease) { if (lease == reinterpret_cast<MgcLease *>(&host)) ++host_refs; else ++device_refs; }
extern "C" void mgc_lease_release(MgcLease *lease) { if (lease == reinterpret_cast<MgcLease *>(&host)) --host_refs; else --device_refs; }
extern "C" MgcStatus mgc_lease_map(MgcLease *, void **, uint64_t *) { return MGC_BUSY; }
extern "C" MgcStatus mgc_lease_unmap(MgcLease *) { return MGC_BUSY; }
extern "C" MgcStatus mgc_submit(MgcSession *, const MgcSubmitDesc *desc, MgcTicket **out) {
    *out = nullptr;
    if (result_mode == 0) return MGC_INVALID_ARGUMENT; /* rejected before retain */
    desc->retain_context(desc->context);
    if (result_mode == 1) { desc->release_context(desc->context); return MGC_BACKPRESSURE; }
    MgcCompletion completion{}; desc->completion(desc->context, &completion); desc->release_context(desc->context);
    mgc_lease_release(desc->input); mgc_lease_release(desc->output); /* accepted references retired */
    *out = reinterpret_cast<MgcTicket *>(&ticket); return MGC_ACCEPTED;
}
int main() {
    mgmem_transfer args{};
    args.memory = reinterpret_cast<mgmem_domain *>(&resource);
    args.compute = reinterpret_cast<MgcDomain *>(&resource);
    args.session = reinterpret_cast<MgcSession *>(&resource);
    args.device = reinterpret_cast<MgcLease *>(&device);
    args.bytes = 64; args.on_error = error; args.completion = complete_user;
    args.retain_context = retain_user; args.release_context = release_user;
    auto prepared = mgmem_prepare_upload(&args); CHECK(prepared.status == MGMEM_OK && prepared.stage);
    stage = prepared.stage; device_refs = 1;
    result_mode = 0;
    auto rejected = mgmem_stage_send(stage, &args); CHECK(rejected.status == MGMEM_INVALID_ARGUMENT && !rejected.ticket);
    CHECK(host_refs == 1 && device_refs == 1 && retains == 0 && releases == 0);
    result_mode = 1;
    rejected = mgmem_stage_send(stage, &args); CHECK(rejected.status == MGMEM_BUSY && !rejected.ticket);
    CHECK(host_refs == 1 && device_refs == 1 && retains == 1 && releases == 1);
    result_mode = 2; send_returned = false;
    auto accepted = mgmem_stage_send(stage, &args); send_returned = true;
    CHECK(accepted.status == MGMEM_OK && accepted.origin_status == MGC_ACCEPTED && accepted.ticket);
    CHECK(callbacks == 1 && retains == 2 && releases == 2 && host_refs == 1 && device_refs == 1);
    CHECK(mgmem_stage_close(&stage) == MGMEM_OK && !stage && host_refs == 0);
    std::puts("PASS rejection and completion-before-return");
}
