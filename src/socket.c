/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2023-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io CUtil.
 *
 * Ghoti.io CUtil is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io CUtil is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// SOCK_NONBLOCK and friends are not in strict ISO C.
#ifndef _WIN32
#define _GNU_SOURCE
#endif

/**
 * @file
 *
 * Sockets over BSD sockets and Winsock.  See socket.h for the contract and
 * `documentation/loop.md` for the reasoning.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/macros.h>

// Winsock's header has to be seen before windows.h, which once.h and mutex.h
// pull in, or the old winsock.h arrives first and the two collide.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <ghoti.io/cutil/once.h>
#include <ghoti.io/cutil/socket.h>

#include "socket_internal.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
typedef int gcu_socklen_t;
#define GCU_SOCKOPT_PTR(p) ((const char *)(p))
#define GCU_SOCKOPT_OUT(p) ((char *)(p))
#else
typedef socklen_t gcu_socklen_t;
#define GCU_SOCKOPT_PTR(p) (p)
#define GCU_SOCKOPT_OUT(p) (p)
#endif

static const char * const gcu_socket_result_names[GCU_SOCKET_RESULT_COUNT] = {
  [GCU_SOCKET_OK] = "ok",
  [GCU_SOCKET_ERR_INVALID] = "invalid argument",
  [GCU_SOCKET_ERR_OOM] = "out of memory",
  [GCU_SOCKET_ERR_UNSUPPORTED] = "not supported here",
  [GCU_SOCKET_ERR_STATE] = "operations are in flight",
  [GCU_SOCKET_ERR_OS] = "the operating system refused",
};

const char * gcu_socket_result_string(GCU_Socket_Result result) {
  if ((unsigned)result >= (unsigned)GCU_SOCKET_RESULT_COUNT) {
    return "unknown socket result";
  }
  return gcu_socket_result_names[result];
}

//
// The last OS error, kept across the cleanup a failed call does before it
// returns, so that "read it on the next line" stays true.
//
static inline int gcu_socket_save_error(void) {
#ifdef _WIN32
  return (int)GetLastError();
#else
  return errno;
#endif
}

static inline void gcu_socket_restore_error(int error) {
#ifdef _WIN32
  SetLastError((DWORD)error);
#else
  errno = error;
#endif
}

#ifdef _WIN32
static GCU_Once gcu_socket_startup_once = GCU_ONCE_INIT;

static void gcu_socket_startup_routine(void) {
  WSADATA data;
  // Winsock 2.2, which every supported Windows has.  Not undone: the loop
  // and the sockets are process-lifetime users, and a WSACleanup racing a
  // socket that another thread still holds is the worse hazard.
  WSAStartup(MAKEWORD(2, 2), &data);
}
#endif

void gcu_socket_startup_(void) {
#ifdef _WIN32
  gcu_once(&gcu_socket_startup_once, gcu_socket_startup_routine);
#endif
}

//
// Addresses.
//

static void gcu_socket_address_clear(GCU_Socket_Address * out) {
  memset(out, 0, sizeof(*out));
  out->size = sizeof(*out);
}

GCU_Socket_Result gcu_socket_address_ipv4(
  GCU_Socket_Address * out, const uint8_t bytes[4], uint16_t port) {
  if (!out || !bytes) {
    return GCU_SOCKET_ERR_INVALID;
  }
  gcu_socket_address_clear(out);
  out->family = GCU_SOCKET_IPV4;
  out->port = port;
  memcpy(out->bytes, bytes, 4);
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_address_ipv6(GCU_Socket_Address * out,
  const uint8_t bytes[16], uint32_t scope_id, uint16_t port) {
  if (!out || !bytes) {
    return GCU_SOCKET_ERR_INVALID;
  }
  gcu_socket_address_clear(out);
  out->family = GCU_SOCKET_IPV6;
  out->port = port;
  out->scope_id = scope_id;
  memcpy(out->bytes, bytes, 16);
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_address_loopback(
  GCU_Socket_Address * out, GCU_Socket_Family family, uint16_t port) {
  static const uint8_t v4[4] = {127, 0, 0, 1};
  static const uint8_t v6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1};
  if (family == GCU_SOCKET_IPV4) {
    return gcu_socket_address_ipv4(out, v4, port);
  }
  if (family == GCU_SOCKET_IPV6) {
    return gcu_socket_address_ipv6(out, v6, 0, port);
  }
  return GCU_SOCKET_ERR_INVALID;
}

GCU_Socket_Result gcu_socket_address_any(
  GCU_Socket_Address * out, GCU_Socket_Family family, uint16_t port) {
  static const uint8_t zero[16] = {0};
  if (family == GCU_SOCKET_IPV4) {
    return gcu_socket_address_ipv4(out, zero, port);
  }
  if (family == GCU_SOCKET_IPV6) {
    return gcu_socket_address_ipv6(out, zero, 0, port);
  }
  return GCU_SOCKET_ERR_INVALID;
}

GCU_Socket_Result gcu_socket_address_parse(
  GCU_Socket_Address * out, const char * text, uint16_t port) {
  if (!out || !text) {
    return GCU_SOCKET_ERR_INVALID;
  }
  gcu_socket_startup_();
  uint8_t bytes[16];
  if (inet_pton(AF_INET, text, bytes) == 1) {
    return gcu_socket_address_ipv4(out, bytes, port);
  }
  if (inet_pton(AF_INET6, text, bytes) == 1) {
    return gcu_socket_address_ipv6(out, bytes, 0, port);
  }
  return GCU_SOCKET_ERR_INVALID;
}

GCU_Socket_Result gcu_socket_address_format(
  const GCU_Socket_Address * address, char * buffer, size_t size) {
  if (!address || !buffer || size == 0
      || address->size < sizeof(GCU_Socket_Address)) {
    return GCU_SOCKET_ERR_INVALID;
  }
  gcu_socket_startup_();
  char text[GCU_SOCKET_ADDRESS_STRING_MAX];
  char host[INET6_ADDRSTRLEN + 1];
  if (address->family == GCU_SOCKET_IPV4) {
    if (!inet_ntop(AF_INET, address->bytes, host, sizeof(host))) {
      return GCU_SOCKET_ERR_INVALID;
    }
    snprintf(text, sizeof(text), "%s:%u", host, (unsigned)address->port);
  }
  else if (address->family == GCU_SOCKET_IPV6) {
    if (!inet_ntop(AF_INET6, address->bytes, host, sizeof(host))) {
      return GCU_SOCKET_ERR_INVALID;
    }
    if (address->scope_id) {
      snprintf(text, sizeof(text), "[%s%%%lu]:%u", host,
        (unsigned long)address->scope_id, (unsigned)address->port);
    }
    else {
      snprintf(text, sizeof(text), "[%s]:%u", host, (unsigned)address->port);
    }
  }
  else {
    return GCU_SOCKET_ERR_INVALID;
  }
  if (strlen(text) >= size) {
    buffer[0] = '\0';
    return GCU_SOCKET_ERR_INVALID;
  }
  memcpy(buffer, text, strlen(text) + 1);
  return GCU_SOCKET_OK;
}

bool gcu_socket_address_to_native_(
  const GCU_Socket_Address * address, void * storage, int * length) {
  if (!address || address->size < sizeof(GCU_Socket_Address)) {
    return false;
  }
  if (address->family == GCU_SOCKET_IPV4) {
    struct sockaddr_in * in = (struct sockaddr_in *)storage;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons(address->port);
    memcpy(&in->sin_addr, address->bytes, 4);
    *length = (int)sizeof(*in);
    return true;
  }
  if (address->family == GCU_SOCKET_IPV6) {
    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *)storage;
    memset(in6, 0, sizeof(*in6));
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(address->port);
    in6->sin6_scope_id = address->scope_id;
    memcpy(&in6->sin6_addr, address->bytes, 16);
    *length = (int)sizeof(*in6);
    return true;
  }
  return false;
}

bool gcu_socket_address_from_native_(
  const void * native, int length, GCU_Socket_Address * out) {
  const struct sockaddr * sa = (const struct sockaddr *)native;
  if (length >= (int)sizeof(struct sockaddr_in) && sa->sa_family == AF_INET) {
    const struct sockaddr_in * in = (const struct sockaddr_in *)native;
    gcu_socket_address_clear(out);
    out->family = GCU_SOCKET_IPV4;
    out->port = ntohs(in->sin_port);
    memcpy(out->bytes, &in->sin_addr, 4);
    return true;
  }
  if (length >= (int)sizeof(struct sockaddr_in6)
      && sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 * in6 = (const struct sockaddr_in6 *)native;
    gcu_socket_address_clear(out);
    out->family = GCU_SOCKET_IPV6;
    out->port = ntohs(in6->sin6_port);
    out->scope_id = in6->sin6_scope_id;
    memcpy(out->bytes, &in6->sin6_addr, 16);
    return true;
  }
  return false;
}

//
// Sockets.
//

#ifdef _WIN32
#define GCU_NATIVE(s) ((SOCKET)(s)->handle)
#define GCU_NATIVE_INVALID INVALID_SOCKET
#define GCU_SOCKET_FAILED(r) ((r) == SOCKET_ERROR)
#else
#define GCU_NATIVE(s) ((s)->fd)
#define GCU_NATIVE_INVALID (-1)
#define GCU_SOCKET_FAILED(r) ((r) < 0)
#endif

void gcu_socket_native_close_(GCU_Socket * socket) {
#ifdef _WIN32
  closesocket(GCU_NATIVE(socket));
#else
  close(GCU_NATIVE(socket));
#endif
}

void gcu_socket_free_(GCU_Socket * socket) {
  gcu_allocator_free(socket->allocator, socket);
}

GCU_Socket_Result gcu_socket_allocate_(GCU_Socket ** out,
  const GCU_Allocator * allocator, GCU_Socket_Family family,
  GCU_Socket_Type type) {
  GCU_Socket * socket = gcu_allocator_calloc(allocator, 1, sizeof(*socket));
  if (!socket) {
    return GCU_SOCKET_ERR_OOM;
  }
  socket->allocator = allocator ? allocator : gcu_allocator_default();
  socket->family = family;
  socket->type = type;
#ifdef _WIN32
  socket->handle = (uintptr_t)INVALID_SOCKET;
#else
  socket->fd = -1;
#endif
  socket->nonblocking = true;
  *out = socket;
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_adopt_(GCU_Socket ** out,
  const GCU_Allocator * allocator, GCU_Socket_Family family,
  GCU_Socket_Type type, uintptr_t native) {
  GCU_Socket * socket = NULL;
  if (gcu_socket_allocate_(&socket, allocator, family, type)
      != GCU_SOCKET_OK) {
    // The caller's handle is ours to release on failure, as documented.
#ifdef _WIN32
    closesocket((SOCKET)native);
#else
    close((int)native);
#endif
    return GCU_SOCKET_ERR_OOM;
  }
#ifdef _WIN32
  socket->handle = native;
#else
  socket->fd = (int)native;
#endif
  *out = socket;
  return GCU_SOCKET_OK;
}

static void gcu_socket_close_raw(uintptr_t native) {
#ifdef _WIN32
  closesocket((SOCKET)native);
#else
  close((int)native);
#endif
}

GCU_Socket_Result gcu_socket_create(GCU_Socket ** out_socket,
  GCU_Socket_Family family, GCU_Socket_Type type,
  const GCU_Allocator * allocator) {
  if (!out_socket || (family != GCU_SOCKET_IPV4 && family != GCU_SOCKET_IPV6)
      || (type != GCU_SOCKET_STREAM && type != GCU_SOCKET_DATAGRAM)) {
    return GCU_SOCKET_ERR_INVALID;
  }
  gcu_socket_startup_();
  int af = family == GCU_SOCKET_IPV4 ? AF_INET : AF_INET6;
  int kind = type == GCU_SOCKET_STREAM ? SOCK_STREAM : SOCK_DGRAM;

#ifdef _WIN32
  SOCKET native = WSASocketW(af, kind, 0, NULL, 0,
    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (native == INVALID_SOCKET) {
    return GCU_SOCKET_ERR_OS;
  }
  u_long on = 1;
  if (ioctlsocket(native, FIONBIO, &on) == SOCKET_ERROR) {
    int saved = gcu_socket_save_error();
    closesocket(native);
    gcu_socket_restore_error(saved);
    return GCU_SOCKET_ERR_OS;
  }
  uintptr_t handle = (uintptr_t)native;
#else
#ifdef __linux__
  int native = socket(af, kind | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (native < 0) {
    return GCU_SOCKET_ERR_OS;
  }
#else
  int native = socket(af, kind, 0);
  if (native < 0) {
    return GCU_SOCKET_ERR_OS;
  }
  if (fcntl(native, F_SETFD, FD_CLOEXEC) < 0
      || fcntl(native, F_SETFL, fcntl(native, F_GETFL, 0) | O_NONBLOCK) < 0) {
    int saved = gcu_socket_save_error();
    close(native);
    gcu_socket_restore_error(saved);
    return GCU_SOCKET_ERR_OS;
  }
#endif
  uintptr_t handle = (uintptr_t)native;
#endif

  // One behaviour everywhere: Linux defaults an IPv6 socket to dual-stack
  // and Windows to IPv6-only.  The caller can turn it off.
  if (family == GCU_SOCKET_IPV6) {
    int on6 = 1;
    if (setsockopt(native, IPPROTO_IPV6, IPV6_V6ONLY, GCU_SOCKOPT_PTR(&on6),
          sizeof(on6)) != 0) {
      int saved = gcu_socket_save_error();
      gcu_socket_close_raw((uintptr_t)native);
      gcu_socket_restore_error(saved);
      return GCU_SOCKET_ERR_OS;
    }
  }

  GCU_Socket * made = NULL;
  GCU_Socket_Result result =
    gcu_socket_adopt_(&made, allocator, family, type, handle);
  if (result != GCU_SOCKET_OK) {
    return result;
  }
  *out_socket = made;
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_bind(
  GCU_Socket * socket, const GCU_Socket_Address * address) {
  if (!socket || !address) {
    return GCU_SOCKET_ERR_INVALID;
  }
  if (address->family != socket->family) {
    return GCU_SOCKET_ERR_INVALID;
  }
  struct sockaddr_storage storage;
  int length = 0;
  if (!gcu_socket_address_to_native_(address, &storage, &length)) {
    return GCU_SOCKET_ERR_INVALID;
  }
  if (GCU_SOCKET_FAILED(bind(GCU_NATIVE(socket),
        (const struct sockaddr *)&storage, (gcu_socklen_t)length))) {
    return GCU_SOCKET_ERR_OS;
  }
  socket->bound = true;
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_listen(GCU_Socket * socket, int backlog) {
  if (!socket || socket->type != GCU_SOCKET_STREAM) {
    return GCU_SOCKET_ERR_INVALID;
  }
  if (GCU_SOCKET_FAILED(
        listen(GCU_NATIVE(socket), backlog > 0 ? backlog : SOMAXCONN))) {
    return GCU_SOCKET_ERR_OS;
  }
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_local_address(
  const GCU_Socket * socket, GCU_Socket_Address * out) {
  if (!socket || !out) {
    return GCU_SOCKET_ERR_INVALID;
  }
  struct sockaddr_storage storage;
  gcu_socklen_t length = sizeof(storage);
  if (GCU_SOCKET_FAILED(getsockname(
        GCU_NATIVE(socket), (struct sockaddr *)&storage, &length))) {
    return GCU_SOCKET_ERR_OS;
  }
  return gcu_socket_address_from_native_(&storage, (int)length, out)
    ? GCU_SOCKET_OK : GCU_SOCKET_ERR_INVALID;
}

GCU_Socket_Result gcu_socket_peer_address(
  const GCU_Socket * socket, GCU_Socket_Address * out) {
  if (!socket || !out) {
    return GCU_SOCKET_ERR_INVALID;
  }
  struct sockaddr_storage storage;
  gcu_socklen_t length = sizeof(storage);
  if (GCU_SOCKET_FAILED(getpeername(
        GCU_NATIVE(socket), (struct sockaddr *)&storage, &length))) {
    return GCU_SOCKET_ERR_OS;
  }
  return gcu_socket_address_from_native_(&storage, (int)length, out)
    ? GCU_SOCKET_OK : GCU_SOCKET_ERR_INVALID;
}

/**
 * Where an option lives, for both directions.
 */
static bool gcu_socket_option_level(
  GCU_Socket_Option option, int * level, int * name) {
  switch (option) {
    case GCU_SOCKET_OPT_REUSE_ADDRESS:
      *level = SOL_SOCKET; *name = SO_REUSEADDR; return true;
    case GCU_SOCKET_OPT_NO_DELAY:
      *level = IPPROTO_TCP; *name = TCP_NODELAY; return true;
    case GCU_SOCKET_OPT_KEEP_ALIVE:
      *level = SOL_SOCKET; *name = SO_KEEPALIVE; return true;
    case GCU_SOCKET_OPT_IPV6_ONLY:
      *level = IPPROTO_IPV6; *name = IPV6_V6ONLY; return true;
    case GCU_SOCKET_OPT_SEND_BUFFER:
      *level = SOL_SOCKET; *name = SO_SNDBUF; return true;
    case GCU_SOCKET_OPT_RECV_BUFFER:
      *level = SOL_SOCKET; *name = SO_RCVBUF; return true;
    case GCU_SOCKET_OPT_LINGER:
      *level = SOL_SOCKET; *name = SO_LINGER; return true;
  }
  return false;
}

GCU_Socket_Result gcu_socket_set_option(
  GCU_Socket * socket, GCU_Socket_Option option, int value) {
  int level = 0;
  int name = 0;
  if (!socket || !gcu_socket_option_level(option, &level, &name)) {
    return GCU_SOCKET_ERR_INVALID;
  }
  int status;
  if (option == GCU_SOCKET_OPT_LINGER) {
    if (value > 65535) {
      return GCU_SOCKET_ERR_INVALID;   // l_linger is an unsigned short
    }
    struct linger linger_value;
    linger_value.l_onoff = value >= 0;
    linger_value.l_linger = value >= 0 ? (unsigned short)value : 0;
    status = setsockopt(GCU_NATIVE(socket), level, name,
      GCU_SOCKOPT_PTR(&linger_value), sizeof(linger_value));
  }
  else {
    status = setsockopt(GCU_NATIVE(socket), level, name,
      GCU_SOCKOPT_PTR(&value), sizeof(value));
  }
  return GCU_SOCKET_FAILED(status) ? GCU_SOCKET_ERR_OS : GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_get_option(
  const GCU_Socket * socket, GCU_Socket_Option option, int * out_value) {
  int level = 0;
  int name = 0;
  if (!socket || !out_value || !gcu_socket_option_level(option, &level, &name)) {
    return GCU_SOCKET_ERR_INVALID;
  }
  int status;
  int value = 0;
  if (option == GCU_SOCKET_OPT_LINGER) {
    struct linger linger_value;
    gcu_socklen_t length = sizeof(linger_value);
    status = getsockopt(GCU_NATIVE(socket), level, name,
      GCU_SOCKOPT_OUT(&linger_value), &length);
    value = linger_value.l_onoff ? (int)linger_value.l_linger : -1;
  }
  else {
    gcu_socklen_t length = sizeof(value);
    status = getsockopt(GCU_NATIVE(socket), level, name,
      GCU_SOCKOPT_OUT(&value), &length);
  }
  if (GCU_SOCKET_FAILED(status)) {
    return GCU_SOCKET_ERR_OS;
  }
  *out_value = value;
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_set_nonblocking(
  GCU_Socket * socket, bool nonblocking) {
  if (!socket) {
    return GCU_SOCKET_ERR_INVALID;
  }
  if (!nonblocking && (socket->loop || socket->pending)) {
    // The loop's thread would block in the call it makes for this socket.
    return GCU_SOCKET_ERR_STATE;
  }
#ifdef _WIN32
  u_long mode = nonblocking ? 1 : 0;
  if (ioctlsocket(GCU_NATIVE(socket), FIONBIO, &mode) == SOCKET_ERROR) {
    return GCU_SOCKET_ERR_OS;
  }
#else
  int flags = fcntl(socket->fd, F_GETFL, 0);
  if (flags < 0
      || fcntl(socket->fd, F_SETFL,
           nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0) {
    return GCU_SOCKET_ERR_OS;
  }
#endif
  socket->nonblocking = nonblocking;
  return GCU_SOCKET_OK;
}

GCU_Socket_Result gcu_socket_shutdown(
  GCU_Socket * socket, GCU_Socket_Shutdown how) {
  if (!socket) {
    return GCU_SOCKET_ERR_INVALID;
  }
  int native_how;
  switch (how) {
    case GCU_SOCKET_SHUTDOWN_READ:
      native_how = 0; break;       // SHUT_RD, SD_RECEIVE
    case GCU_SOCKET_SHUTDOWN_WRITE:
      native_how = 1; break;       // SHUT_WR, SD_SEND
    case GCU_SOCKET_SHUTDOWN_BOTH:
      native_how = 2; break;       // SHUT_RDWR, SD_BOTH
    default:
      return GCU_SOCKET_ERR_INVALID;
  }
  return GCU_SOCKET_FAILED(shutdown(GCU_NATIVE(socket), native_how))
    ? GCU_SOCKET_ERR_OS : GCU_SOCKET_OK;
}

GCU_Socket_Family gcu_socket_family(const GCU_Socket * socket) {
  return socket ? socket->family : (GCU_Socket_Family)0;
}

GCU_Socket_Type gcu_socket_type(const GCU_Socket * socket) {
  return socket ? socket->type : (GCU_Socket_Type)0;
}

GCU_Socket_Result gcu_socket_close(GCU_Socket * socket) {
  if (!socket) {
    return GCU_SOCKET_OK;
  }
  if (socket->pending) {
    return GCU_SOCKET_ERR_STATE;
  }
  if (socket->loop) {
    // The loop takes the socket out of its set and decides when the memory
    // goes: an event for it may already be in the batch being delivered.
    gcu_loop_socket_closed_(socket->loop, socket);
    return GCU_SOCKET_OK;
  }
  gcu_socket_native_close_(socket);
  gcu_socket_free_(socket);
  return GCU_SOCKET_OK;
}

GCU_Socket_Error_Kind gcu_socket_error_kind(int os_error) {
#ifdef _WIN32
  switch (os_error) {
    case WSAECONNREFUSED: return GCU_SOCKET_ERROR_REFUSED;
    case WSAECONNRESET: return GCU_SOCKET_ERROR_RESET;
    case WSAECONNABORTED: return GCU_SOCKET_ERROR_ABORTED;
    case WSAETIMEDOUT: return GCU_SOCKET_ERROR_TIMED_OUT;
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAENETDOWN:
    case WSAEHOSTDOWN: return GCU_SOCKET_ERROR_UNREACHABLE;
    case WSAEADDRINUSE: return GCU_SOCKET_ERROR_ADDRESS_IN_USE;
    case WSAENOTCONN: return GCU_SOCKET_ERROR_NOT_CONNECTED;
    case WSAESHUTDOWN: return GCU_SOCKET_ERROR_BROKEN_PIPE;
    // WSA_OPERATION_ABORTED is ERROR_OPERATION_ABORTED, 995: the code an
    // overlapped call reports when CancelIoEx ended it.
    case WSA_OPERATION_ABORTED:
    case WSAECANCELLED: return GCU_SOCKET_ERROR_CANCELLED;
    default: return GCU_SOCKET_ERROR_OTHER;
  }
#else
  switch (os_error) {
    case ECONNREFUSED: return GCU_SOCKET_ERROR_REFUSED;
    case ECONNRESET: return GCU_SOCKET_ERROR_RESET;
    case ECONNABORTED: return GCU_SOCKET_ERROR_ABORTED;
    case ETIMEDOUT: return GCU_SOCKET_ERROR_TIMED_OUT;
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
    case EHOSTDOWN: return GCU_SOCKET_ERROR_UNREACHABLE;
    case EADDRINUSE: return GCU_SOCKET_ERROR_ADDRESS_IN_USE;
    case ENOTCONN: return GCU_SOCKET_ERROR_NOT_CONNECTED;
    case EPIPE: return GCU_SOCKET_ERROR_BROKEN_PIPE;
    case ECANCELED: return GCU_SOCKET_ERROR_CANCELLED;
    default: return GCU_SOCKET_ERROR_OTHER;
  }
#endif
}
