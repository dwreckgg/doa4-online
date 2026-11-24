// src/net/np_posix.h - the Winsock and thread-id spellings the netplay code uses, for POSIX builds only.
// The socket names follow miniupnpc's miniupnpc_socketdef.h (third_party/miniupnpc/src, BSD 3-Clause, Thomas Bernard):
// SOCKET is an int and INVALID_SOCKET is -1 outside Windows.
#pragma once

#if defined(_WIN32)
#error "np_posix.h is for POSIX builds; Windows uses winsock2.h and windows.h"
#endif

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

using SOCKET = int;
using DWORD = uint32_t;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

inline int closesocket(SOCKET s) { return close(s); }
inline int WSAGetLastError() { return errno; }
inline DWORD GetCurrentThreadId() { return DWORD(gettid()); }
inline DWORD GetCurrentProcessId() { return DWORD(getpid()); }

namespace doa4::np::posix {

// An auto-reset event as an eventfd: Signal makes it readable, Consume reads it back to unset (a Win32 auto-reset
// event resets when a wait returns; a waiter here polls the fd for POLLIN, then calls Consume).
inline int CreateEvent() { return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }
inline void Signal(int event) {
  const uint64_t one = 1;
  [[maybe_unused]] const ssize_t n = write(event, &one, sizeof one);  // EAGAIN: the counter is already set
}
inline void Consume(int event) {
  uint64_t count;
  [[maybe_unused]] const ssize_t n = read(event, &count, sizeof count);  // EAGAIN: it was not set
}

}  // namespace doa4::np::posix
