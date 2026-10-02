/* selfproc.c -- a real handle to our own process.
 *
 * so_util maps the game's code with svcMapProcessCodeMemory and patches it via
 * svcMapProcessMemory. Mesosphere looks the process handle up for both with
 * GetObjectWithoutPseudoHandle, so CUR_PROCESS_HANDLE is rejected there (it is
 * accepted by SetProcessMemoryPermission, which is why crt0_reloc.c can use
 * it). Launched as an NSO we get no handle from hbloader, so we make one the
 * way hbloader itself does: send CUR_PROCESS_HANDLE as a COPY handle over a
 * session to ourselves. IPC handle translation does resolve the pseudo-handle
 * (KHandleTable::GetObjectForIpc), and the receiver ends up with a real handle
 * to this process. The receiving thread closes the session without replying,
 * which is what ends the sender's svcSendSyncRequest. MIT.
 */
#include <switch.h>

#include "selfproc.h"
#include "util.h"

/* libnx32 (2026-10-02 and later): its envAcquireOwnProcessHandle, which
 * armICacheInvalidate uses, makes its own session to get the handle, but an
 * application may hold only one session it created itself, and crt0_reloc.c
 * has used it. The handle we have is handed over instead. Weak: an older
 * libnx32 has no such function, and the call is skipped. */
void envSetOwnProcessHandle(Handle handle) __attribute__((weak));

static Handle share(Handle h) {
  if (h != INVALID_HANDLE && envSetOwnProcessHandle)
    envSetOwnProcessHandle(h);
  return h;
}

static Handle g_self = INVALID_HANDLE;
static Result g_self_rc;

static void receive_thread(void *arg) {
  Handle session = (Handle)(uintptr_t)arg;
  void *tls = armGetTls();
  hipcMakeRequestInline(tls);
  s32 idx = 0;
  Result rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
  if (R_SUCCEEDED(rc)) {
    HipcParsedRequest r = hipcParseRequest(tls);
    if (r.meta.num_copy_handles == 1)
      g_self = r.data.copy_handles[0];
    else
      rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }
  g_self_rc = rc;
  svcCloseHandle(session);
}

/* crt0_reloc.c already obtained one (hardware) to relocate our own code. */
extern volatile uint32_t __dcr_self_handle __attribute__((visibility("hidden")));

Handle dcr_self_process(void) {
  if (g_self != INVALID_HANDLE)
    return g_self;
  if (__dcr_self_handle) {
    g_self = share(__dcr_self_handle);
    debugPrintf("[self] own process handle 0x%x (from startup)\n", g_self);
    return g_self;
  }

  Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
  Result rc = svcCreateSession(&server, &client, 0, 0);
  if (R_FAILED(rc)) {
    debugPrintf("[self] svcCreateSession failed 0x%x\n", rc);
    return INVALID_HANDLE;
  }

  Thread t;
  rc = threadCreate(&t, receive_thread, (void *)(uintptr_t)server, NULL, 0x4000, 0x20, -2);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&t);
  if (R_FAILED(rc)) {
    debugPrintf("[self] receive thread failed 0x%x\n", rc);
    svcCloseHandle(server);
    svcCloseHandle(client);
    return INVALID_HANDLE;
  }

  hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
  svcSendSyncRequest(client); /* returns once the receiver closes the session */
  svcCloseHandle(client);
  threadWaitForExit(&t);
  threadClose(&t);

  if (g_self == INVALID_HANDLE)
    debugPrintf("[self] no process handle (rc 0x%x)\n", g_self_rc);
  else
    debugPrintf("[self] own process handle 0x%x\n", g_self);
  return share(g_self);
}
