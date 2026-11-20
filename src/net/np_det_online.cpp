// DOA4 netplay phase 1 (Task 13): the determinism layer's fix "port1000" (np_det.cpp lists every fix).
//
// The game's online manager *(0x82E6EC10) is started on every boot (sub_824F4210 -> sub_82449D48 ->
// sub_82445178): sub_82445178 opens socket(AF_INET, DGRAM, VDP 254), binds it to port 1000 on every
// interface and makes it non-blocking; every frame sub_82449EA8 (sub_824F4878, the main loop) reads it with
// recvfrom until it is empty and hands each datagram of >= 21 bytes to the packet handler sub_8242BD10. In a
// session, a datagram from the network would be host-timed input to the game, and two copies on one PC
// race for the port. So in a session sub_82445178 takes the path a failed socket() already takes, without
// asking the host for a socket:
//   - the manager's socket field (manager+0, the r3 the function gets) is -1, as after a failed socket();
//   - the guest thread's last error and the return value are 0x1F, what this SDK's NetDll_socket leaves when
//     the host socket() fails (X_STATUS_UNSUCCESSFUL through xeRtlNtStatusToDosError, sdk-np1
//     src/kernel/xboxkrnl/xboxkrnl_error.cpp: error_table_0xC0000001[0]), which sub_82445178 returns after
//     WSAGetLastError.
// Census: one caller (sub_82449D48, main thread, once at boot), which hands the value back to
// sub_824F4210, which drops it; the field is read only by sub_82449EA8 (skips its recvfrom loop when it is -1)
// and the sendto wrapper sub_82445250 (Xbox Live flows only; with -1 the SDK's NetDll_sendto answers
// WSAENOTSOCK without a host call).
//
// Outside a session the function runs as the game wrote it, with one repair after it (issue #12, the Windows build
// under Proton). When its bind (the call at 0x82445204) or its ioctlsocket (0x82445228) fails, sub_82445178 returns
// the error and leaves the open, still blocking socket in the field (both go to 0x824451CC, which only reads the
// last error); sub_824F4210 drops the result, and sub_82449EA8 then calls recvfrom on that socket every frame. Linux
// refuses a normal user every port below 1024, so there the bind fails; the reporters of issue #12 saw the game
// freeze on a black screen at boot until the port was allowed (their test, not run here). On Windows a refused bind
// does not freeze the game (run b0 of np_runs/p1k: the 0.3.7 build with the port held boots and plays its attract
// demo), as Winsock documents for a recvfrom on an unbound datagram socket (WSAEINVAL). The repair closes that
// socket with the game's own closesocket thunk (sub_82963578, the call the function's re-open branch makes) and
// stores -1, the state a failed socket() leaves and both readers of the field already handle. The game's result is
// handed back unchanged.
// With a session active nothing is added, whatever the fix set: two peers must not differ by one host's port table.

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "np_core.h"
#include "np_det_internal.h"
#include "np_session.h"

extern "C" {
REX_EXTERN(__imp__sub_82445178);
REX_EXTERN(sub_82963578);  // closesocket(r3): the game's thunk to NetDll_closesocket
}

namespace {
// What sub_82445178 returns (and the thread's last error) when the host socket() fails in this SDK.
constexpr uint32_t kSocketFailedError = 0x1F;
// INVALID_SOCKET: what sub_82449EA8 tests before its recvfrom loop
constexpr uint32_t kNoSocket = doa4::np::kNoGuestSocket;
}  // namespace

// sub_82445178(manager r3): (re)opens the online manager's port-1000 socket into *(u32*)manager; 0 or an error.
REX_HOOK_RAW(sub_82445178) {
  using namespace doa4::np::det;
  const uint32_t field = ctx.r3.u32;
  if (doa4::np::Active() && On(kPort1000)) {
    const uint32_t before = rex::memory::load_and_swap<uint32_t>(base + field);
    rex::memory::store_and_swap<uint32_t>(base + field, kNoSocket);
    if (rex::system::XThread::IsInThread()) rex::system::XThread::SetLastError(kSocketFailedError);
    ctx.r3.u64 = kSocketFailedError;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) == 0) {
      doa4::np::LogLine(fmt::format(
          "port 1000: the game's own socket is not opened in a session (sub_82445178 -> {})", kSocketFailedError));
      doa4::np::LogLine(fmt::format("port 1000: manager socket field {:08X} was {:08X}, now {:08X} (frame {}, lr {:08X})",
                                    field, before, kNoSocket, Frame(), uint32_t(ctx.lr)));
    }
    return;
  }
  __imp__sub_82445178(ctx, base);
  if (doa4::np::Active()) return;  // a session with the fix off: the game's function alone, as before
  const uint32_t result = ctx.r3.u32;
  const uint32_t handle = rex::memory::load_and_swap<uint32_t>(base + field);
  if (!doa4::np::Port1000LeftOpen(result, handle)) return;
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = handle;
  sub_82963578(ctx, base);
  const uint32_t closed = ctx.r3.u32;
  ctx.lr = lr;
  rex::memory::store_and_swap<uint32_t>(base + field, kNoSocket);
  ctx.r3.u64 = result;
  static std::atomic<int> repaired{0};
  if (repaired.fetch_add(1) == 0) {
    REXLOG_WARN("[doa4][PORT1000] the host refused the game's own UDP port 1000 (sub_82445178 -> {:#x}); its socket "
                "{:#010x} is closed (closesocket -> {:#x}) and the manager's field now reads {:#010x}, so the game "
                "skips its per-frame read. Expected under Linux/Proton; on Windows the port is held by another "
                "program or reserved.",
                result, handle, closed, rex::memory::load_and_swap<uint32_t>(base + field));
  }
}
