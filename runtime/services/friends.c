// friend:u / friend:a (nn::friends), as on a console whose user has no friends registered and no connection: the
// lists are empty, presence and play-history calls succeed and do nothing. ARMS asks for its friend list at the
// title screen (to show which friends are online) and used a null session when the service had no object to give.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static uint32_t signaled_event(void) {  // readable handle of an event that is already signaled
    uint32_t w, r;
    KEvent *e = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal((KObject *)e);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void friend_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = signaled_event(); break;  // GetCompletionEvent
    case 1: break;                                                         // Cancel
    case 10100: case 10101: case 10400: case 20100: case 20101: case 20102: case 20103: case 20104: case 20200:
    case 20300: case 20400: case 20500:  // friend lists, blocked users, requests, play history...: none (count 0)
        OUT(int32_t, 0) = 0;
        resp_size(resp, 4);
        break;
    default: {
        static uint32_t seen[64];
        static int n;
        int known = 0;
        for (int i = 0; i < n; i++) known |= seen[i] == req->cmd;
        if (!known && n < 64) {
            seen[n++] = req->cmd;
            LOG("[friends] IFriendService cmd %u (ok, nothing)\n", req->cmd);
        }
        break;
    }
    }
}

static void notification_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // GetEvent: never signaled (no notifications)
        uint32_t w, r;
        kernel_create_event_pair(&w, &r);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 1: break;  // Clear (Pop is never reached: the event above is never signaled)
    default: LOG("[friends] INotificationService cmd %u (ok)\n", req->cmd); break;
    }
}

static void friends_static(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("friends:IFriendService", friend_service, NULL)); break;
    case 1: ipc_return_object(resp, service_new("friends:INotificationService", notification_service, NULL)); break;
    default: LOG("[friends] IServiceCreator cmd %u (ok)\n", req->cmd); break;
    }
}

SERVICE("friend:u", friend_u) { friends_static(s, req, resp); }
SERVICE("friend:a", friend_a) { friends_static(s, req, resp); }
SERVICE("friend:m", friend_m) { friends_static(s, req, resp); }
SERVICE("friend:v", friend_v) { friends_static(s, req, resp); }
SERVICE("friend:s", friend_s) { friends_static(s, req, resp); }
