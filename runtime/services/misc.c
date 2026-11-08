// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u
// The add-on content the player has: the indices whose images tools/nsp_extract.py --dlc saved (runtime/services/fs.c
// serves them). MUA3's DLC characters and stories are in its update; the Expansion Pass (index 1) unlocks them.
#define AOC_BASE 0x01009B500007D000ull  // (app id + 0x1000; ARMS has no add-on content)
#define AOC_MAX 16

bool aoc_image_path(uint64_t id, char *out, size_t n);

static int aoc_owned(uint32_t *out) {
    int n = 0;
    char path[1200];
    for (uint32_t i = 1; i <= AOC_MAX; i++)
        if (aoc_image_path(AOC_BASE + i, path, sizeof path)) out[n++] = i;
    return n;
}

SERVICE("aoc:u", aoc_u) {
    (void)s;
    uint32_t owned[AOC_MAX];
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = (uint32_t)aoc_owned(owned); resp_size(resp, 4); break;  // CountAddOnContent
    case 3: {  // ListAddOnContent(offset, count) -> indices
        uint32_t off = IN(uint32_t, 0), count = IN(uint32_t, 4), n = (uint32_t)aoc_owned(owned), k = 0;
        IpcBuf *b = out_buf(req, 0);
        for (uint32_t i = off; b && i < n && k < count && (k + 1) * 4 <= b->size; i++, k++)
            ((uint32_t *)(uintptr_t)b->addr)[k] = owned[i];
        LOG("[aoc] ListAddOnContent: %u of %u\n", k, n);
        OUT(uint32_t, 0) = k;
        resp_size(resp, 4);
        break;
    }
    case 5: OUT(uint64_t, 0) = AOC_BASE; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}
