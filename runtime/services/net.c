// Network services, as on a console with no internet connection: nifm (network interface manager) turns every
// connection request down, bsd (sockets) fails to create sockets, ssl hands out contexts that never get used. The game
// keeps working offline, as it does on a Switch without a connection.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../ui/menu.h"

// nifm results (module 110): the connection test failed / no network
#define NIFM_RESULT_NO_CONNECTION (110u | (3127u << 9))

static uint32_t signaled_event(void) {  // readable handle of an event that is already signaled
    uint32_t w, r;
    KEvent *e = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal((KObject *)e);
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ nifm
enum { REQ_STATE_FREE = 1 };  // (Invalid 0, Free 1, OnHold 2, Accepted 3, Blocking 4): a request that ended

static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = REQ_STATE_FREE; resp_size(resp, 4); break;  // GetRequestState
    case 1: resp->result = NIFM_RESULT_NO_CONNECTION; break;               // GetResult
    case 2:  // GetSystemEventReadableHandles: the state changed (to Free) at once
        resp->copy_handles[resp->ncopy++] = signaled_event();
        resp->copy_handles[resp->ncopy++] = signaled_event();
        break;
    case 3: case 4: break;  // Cancel, Submit
    default: LOG("[nifm] IRequest cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: {  // GetClientId
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = 1;
        break;
    }
    case 2: ipc_return_object(resp, service_new("nifm:IScanRequest", nifm_request, NULL)); break;
    case 4: ipc_return_object(resp, service_new("nifm:IRequest", nifm_request, NULL)); break;  // CreateRequest
    case 5: case 12: case 15: case 18:  // GetCurrentNetworkProfile, GetCurrentIpAddress, ..IpConfigInfo, ..ConnectionStatus
        resp->result = NIFM_RESULT_NO_CONNECTION;
        break;
    case 21: case 22: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsAnyInternetRequestAccepted, IsAnyForegroundRequestAccepted
    default: LOG("[nifm] IGeneralService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void nifm_static(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5)  // CreateGeneralServiceOld, CreateGeneralService
        ipc_return_object(resp, service_new("nifm:IGeneralService", nifm_general, NULL));
    else
        LOG("[nifm] IStaticService cmd %u (ok)\n", req->cmd);
}
SERVICE("nifm:u", nifm_u) { nifm_static(s, req, resp); }
SERVICE("nifm:a", nifm_a) { nifm_static(s, req, resp); }
SERVICE("nifm:s", nifm_s) { nifm_static(s, req, resp); }

// ------------------------------------------------------------------ bsd (sockets)
// The game's own nifm calls (imports of main), answered on the host: the network is never available, and the error
// handling that would show the system's "couldn't connect" applet is done at once, as if the player had closed it.
// (On a console that applet comes from the applet manager; the runtime has no library applets.)
HLE_FUNC("_ZN2nn4nifm10InitializeEv", nifm_api_initialize) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn4nifm18IsNetworkAvailableEv", nifm_api_available) {
    static int nlog;
    if (nlog++ < 20) LOG("[nifm] IsNetworkAvailable: no\n");
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_api_submit_wait) {
    (void)c;
    static int nlog;
    if (nlog++ < 20) LOG("[nifm] SubmitNetworkRequestAndWait: no connection\n");
    // The player asked to go online (Party Match, Friends, ...): on a console the system's "couldn't connect" dialog
    // would say why the game goes back. (Not the game's own quiet check while it starts.)
    if (menu_frames_shown() > 0) menu_notice("Online play isn't available in ARMS Rearmed.", 5);
}
HLE_FUNC("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_api_handle_error) {
    static int nlog;
    if (nlog++ < 3) LOG("[nifm] HandleNetworkRequestErrorResult: no network (the error applet is skipped)\n");
    c->x[0] = NIFM_RESULT_NO_CONNECTION;
}
HLE_FUNC("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_api_proxy) { c->x[0] = NIFM_RESULT_NO_CONNECTION; }
HLE_FUNC("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_api_ip) { c->x[0] = NIFM_RESULT_NO_CONNECTION; }

#define BSD_ENETDOWN 50

static void bsd(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: OUT(int32_t, 0) = 0; resp_size(resp, 4); break;  // RegisterClient
    case 1: break;                                            // StartMonitoring
    case 2: case 3:                                           // Socket, SocketExempt: (ret, errno)
        OUT(int32_t, 0) = -1;
        OUT(uint32_t, 4) = BSD_ENETDOWN;
        resp_size(resp, 8);
        break;
    default:  // anything on a socket that can't exist
        LOG("[bsd] %s cmd %u -> ENETDOWN\n", s->name, req->cmd);
        OUT(int32_t, 0) = -1;
        OUT(uint32_t, 4) = BSD_ENETDOWN;
        resp_size(resp, 8);
    }
}
SERVICE("bsd:u", bsd_u) { bsd(s, req, resp); }
SERVICE("bsd:s", bsd_s) { bsd(s, req, resp); }

// ------------------------------------------------------------------ ldn (local wireless)
// Local play between consoles, as on a console in airplane mode: the service is there, Initialize says local
// communication is off, and the game shows its own message and stays in the menu. (Before this, the generic stub
// answered CreateUserLocalCommunicationService without an object and "Local" crashed the game.)
#define LDN_RESULT(desc) (203u | ((uint32_t)(desc) << 9))
#define LDN_RESULT_AIRPLANE_MODE LDN_RESULT(23)
#define LDN_RESULT_BAD_STATE LDN_RESULT(32)
enum { LDN_STATE_NONE = 0, LDN_STATE_INITIALIZED = 1, LDN_STATE_ERROR = 6 };

static void ldn_user(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = LDN_STATE_ERROR; resp_size(resp, 4); break;  // GetState: never initialised
    case 100: {  // AttachStateChangeEvent: an event that never fires (the state never changes)
        uint32_t w, r;
        kernel_create_event_pair(&w, &r);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 400: case 402:  // Initialize, InitializeSystem2
        LOG("[ldn] Initialize: local wireless is off (as in airplane mode)\n");
        menu_notice("Local wireless play isn't available in ARMS Rearmed.", 5);
        resp->result = LDN_RESULT_AIRPLANE_MODE;
        break;
    case 401: break;  // Finalize
    default:          // scan, open an access point or a station, network info...: not in this state
        LOG("[ldn] IUserLocalCommunicationService cmd %u -> bad state\n", req->cmd);
        resp->result = LDN_RESULT_BAD_STATE;
    }
}
SERVICE("ldn:u", ldn_u) {
    (void)s;
    if (req->cmd == 0)  // CreateUserLocalCommunicationService
        ipc_return_object(resp, service_new("ldn:IUserLocalCommunicationService", ldn_user, NULL));
    else
        LOG("[ldn] IUserServiceCreator cmd %u (ok)\n", req->cmd);
}

// ------------------------------------------------------------------ ssl
static void ssl_context(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s, (void)resp;
    LOG("[ssl] ISslContext cmd %u (ok)\n", req->cmd);
}
SERVICE("ssl", ssl) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("ssl:ISslContext", ssl_context, NULL)); break;  // CreateContext
    case 5: break;  // SetInterfaceVersion
    default: LOG("[ssl] cmd %u (ok)\n", req->cmd);
    }
}
