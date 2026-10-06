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

/**
 * @file
 *
 * What socket.c and loop.c share about a socket.  Never installed.
 */

#ifndef GHOTI_IO_GCU_SRC_SOCKET_INTERNAL_H
#define GHOTI_IO_GCU_SRC_SOCKET_INTERNAL_H

#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/loop.h>
#include <ghoti.io/cutil/socket.h>

/**
 * The socket, as socket.c builds it and loop.c drives it.
 *
 * Everything from `loop` down belongs to the loop the socket is used with.
 */
struct GCU_Socket {
  const GCU_Allocator * allocator;
  GCU_Socket_Family family;
  GCU_Socket_Type type;
#ifdef _WIN32
  uintptr_t handle;               ///< The SOCKET.
#else
  int fd;                         ///< The descriptor.
#endif
  bool nonblocking;
  bool bound;                     ///< Bound, explicitly or by the loop.
  bool detached;                  ///< Its loop was destroyed.

  struct GCU_Loop * loop;         ///< The loop it is used with, or NULL.
  size_t pending;                 ///< Operations in flight on it.
  GCU_Loop_Op * rq_head;          ///< Reads, accepts, receives, in order.
  GCU_Loop_Op * rq_tail;
  GCU_Loop_Op * wq_head;          ///< Writes, connects, sends, in order.
  GCU_Loop_Op * wq_tail;
  uint32_t interest;              ///< What the epoll registration asks for.
  bool registered;                ///< Present in the epoll set.
  bool closed;                    ///< Closed; memory freed at the iteration's end.
  GCU_Socket * loop_next;         ///< The loop's list of sockets.
  GCU_Socket * loop_prev;
  GCU_Socket * zombie_next;       ///< The loop's list of closed sockets.
};

/**
 * Allocate a socket structure with no native handle yet (`fd` is -1 on POSIX,
 * the handle is INVALID_SOCKET on Windows), for a caller that must have the
 * memory before it takes a connection off the OS.
 *
 * @param out Receives the socket.
 * @param allocator The allocator, or NULL.
 * @param family The family.
 * @param type The type.
 * @return ::GCU_SOCKET_OK or ::GCU_SOCKET_ERR_OOM.
 */
GCU_Socket_Result gcu_socket_allocate_(GCU_Socket ** out,
  const GCU_Allocator * allocator, GCU_Socket_Family family,
  GCU_Socket_Type type);

/**
 * Wrap a native handle in a socket the caller will own.  The handle is
 * adopted: freed with the socket, and closed on failure.
 *
 * @param out Receives the socket.
 * @param allocator The allocator, or NULL.
 * @param family The family.
 * @param type The type.
 * @param native The descriptor or SOCKET, already non-blocking.
 * @return ::GCU_SOCKET_OK or ::GCU_SOCKET_ERR_OOM.
 */
GCU_Socket_Result gcu_socket_adopt_(GCU_Socket ** out,
  const GCU_Allocator * allocator, GCU_Socket_Family family,
  GCU_Socket_Type type, uintptr_t native);

/** Close the native handle; leaves the structure. */
void gcu_socket_native_close_(GCU_Socket * socket);

/** Free the structure after the native handle is closed. */
void gcu_socket_free_(GCU_Socket * socket);

/** The loop's half of closing a socket that was used with it. */
void gcu_loop_socket_closed_(struct GCU_Loop * loop, GCU_Socket * socket);

/**
 * Convert to the OS's address.
 *
 * @param address The address.
 * @param storage At least 128 bytes, suitably aligned.
 * @param length Receives the length.
 * @return `false` for a bad address.
 */
bool gcu_socket_address_to_native_(
  const GCU_Socket_Address * address, void * storage, int * length);

/**
 * Convert from the OS's address.
 *
 * @param native A `struct sockaddr`.
 * @param length Its length.
 * @param out Receives the address.
 * @return `false` for a family that is neither IPv4 nor IPv6.
 */
bool gcu_socket_address_from_native_(
  const void * native, int length, GCU_Socket_Address * out);

/** Run Winsock's startup once; a no-op elsewhere. */
void gcu_socket_startup_(void);

#endif // GHOTI_IO_GCU_SRC_SOCKET_INTERNAL_H
