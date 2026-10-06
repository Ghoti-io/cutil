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
 * Non-blocking TCP and UDP sockets over BSD sockets and Winsock, behind one
 * interface.  Names are not resolved here: an address is a number.
 *
 * A socket is only a handle on the operating system's object.  Reading,
 * writing, accepting and connecting are done through the event loop
 * (loop.h), which reports each operation when it has completed; nothing in
 * this file blocks, and nothing here exposes readiness.
 *
 * The design and the reasoning behind each decision are recorded in
 * `documentation/loop.md`.
 *
 * ## Non-blocking from creation
 *
 * Every socket is created in non-blocking mode, and the descriptor is not
 * inherited by child processes.  On Windows the socket is created
 * overlapped so that a completion port can drive it, and this file owns
 * `WSAStartup`: it runs once, the first time a socket or address is created,
 * and is never undone.
 *
 * ## Errors
 *
 * A call that fails because the operating system refused returns
 * ::GCU_SOCKET_ERR_OS and leaves the reason in `gcu_error_last()`, to be read
 * on the next line like any other OS error (error.h).  An operation started
 * through the loop carries the same code in its completion.
 * gcu_socket_error_kind() sorts such a code into the handful of cases a
 * caller acts on, the same way on every platform.  `EAGAIN` and
 * `WSAEWOULDBLOCK` are never reported: waiting is what the loop is for.
 *
 * ## Threads
 *
 * A socket is used from one thread at a time.  The loop it is used with
 * adds a stricter rule: see loop.h.
 */

#ifndef GHOTI_IO_GCU_SOCKET_H
#define GHOTI_IO_GCU_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The longest text gcu_socket_address_format() writes, terminator included.
 */
#define GCU_SOCKET_ADDRESS_STRING_MAX 80

/**
 * The outcome of a socket call.
 */
typedef enum GCU_Socket_Result {
  GCU_SOCKET_OK = 0,          ///< Succeeded.
  GCU_SOCKET_ERR_INVALID,     ///< A bad argument: NULL, an unparsable address.
  GCU_SOCKET_ERR_OOM,         ///< No memory.
  GCU_SOCKET_ERR_UNSUPPORTED, ///< Not available on this platform or here.
  GCU_SOCKET_ERR_STATE,       ///< Not valid now: operations are in flight.
  GCU_SOCKET_ERR_OS,          ///< The OS refused; see gcu_error_last().
  GCU_SOCKET_RESULT_COUNT,    ///< The number of results; not itself a result.
} GCU_Socket_Result;

/**
 * Address families.
 */
typedef enum GCU_Socket_Family {
  GCU_SOCKET_IPV4 = 1, ///< IPv4.
  GCU_SOCKET_IPV6 = 2, ///< IPv6.
} GCU_Socket_Family;

/**
 * Socket types.
 */
typedef enum GCU_Socket_Type {
  GCU_SOCKET_STREAM = 1,   ///< TCP: a reliable byte stream.
  GCU_SOCKET_DATAGRAM = 2, ///< UDP: unreliable datagrams.
} GCU_Socket_Type;

/**
 * Options for gcu_socket_set_option() and gcu_socket_get_option().  Every
 * value is an `int`; a boolean option takes 0 or 1.
 */
typedef enum GCU_Socket_Option {
  GCU_SOCKET_OPT_REUSE_ADDRESS = 1, ///< Boolean: `SO_REUSEADDR`.
  GCU_SOCKET_OPT_NO_DELAY,          ///< Boolean: `TCP_NODELAY`.  Stream only.
  GCU_SOCKET_OPT_KEEP_ALIVE,        ///< Boolean: `SO_KEEPALIVE`.
  GCU_SOCKET_OPT_IPV6_ONLY,         ///< Boolean: `IPV6_V6ONLY`.  IPv6 only.  A
                                    ///<   new IPv6 socket has it on, on every platform.
  GCU_SOCKET_OPT_SEND_BUFFER,       ///< Bytes: `SO_SNDBUF`.  The OS may adjust.
  GCU_SOCKET_OPT_RECV_BUFFER,       ///< Bytes: `SO_RCVBUF`.  The OS may adjust.
  /**
   * `SO_LINGER`: a negative value turns lingering off, otherwise it is the
   * number of seconds close may take to flush, at most 65535 (more is
   * ::GCU_SOCKET_ERR_INVALID, not a wrap).  Zero makes a close abortive:
   * the peer sees a reset instead of an orderly end.  Stream only.
   */
  GCU_SOCKET_OPT_LINGER,
} GCU_Socket_Option;

/**
 * Which direction gcu_socket_shutdown() ends.
 */
typedef enum GCU_Socket_Shutdown {
  GCU_SOCKET_SHUTDOWN_READ = 1,  ///< No more receiving.
  GCU_SOCKET_SHUTDOWN_WRITE = 2, ///< No more sending; the peer reads the end.
  GCU_SOCKET_SHUTDOWN_BOTH = 3,  ///< Both.
} GCU_Socket_Shutdown;

/**
 * What an OS error code means, the same on every platform.
 */
typedef enum GCU_Socket_Error_Kind {
  GCU_SOCKET_ERROR_OTHER = 0,       ///< Anything not listed.
  GCU_SOCKET_ERROR_REFUSED,         ///< Nothing is listening there.
  GCU_SOCKET_ERROR_RESET,           ///< The peer reset the connection.
  GCU_SOCKET_ERROR_ABORTED,         ///< The connection was aborted locally.
  GCU_SOCKET_ERROR_TIMED_OUT,       ///< The OS gave up waiting.
  GCU_SOCKET_ERROR_UNREACHABLE,     ///< No route to the network or host.
  GCU_SOCKET_ERROR_ADDRESS_IN_USE,  ///< The address is already bound.
  GCU_SOCKET_ERROR_NOT_CONNECTED,   ///< The socket is not connected.
  GCU_SOCKET_ERROR_BROKEN_PIPE,     ///< Wrote to a connection already closed.
  GCU_SOCKET_ERROR_CANCELLED,       ///< The OS reports the operation aborted.
} GCU_Socket_Error_Kind;

/**
 * An IPv4 or IPv6 address and a port.  Plain data, built by the calls below
 * or by filling the fields after setting `size`.
 *
 * Begins with `size`, so that fields may be added at the end: a receiver
 * reads only what fits in the size the caller states, and a struct smaller
 * than the first generation is refused with ::GCU_SOCKET_ERR_INVALID.  Always
 * set `size = sizeof(GCU_Socket_Address)`; the constructors do.
 */
typedef struct GCU_Socket_Address {
  size_t size;               ///< `sizeof(GCU_Socket_Address)`.
  GCU_Socket_Family family;  ///< The family; says how many `bytes` are used.
  uint16_t port;             ///< The port, in host byte order.
  uint8_t bytes[16];         ///< The address in network order; IPv4 uses 4.
  uint32_t scope_id;         ///< IPv6 zone (interface index); otherwise 0.
} GCU_Socket_Address;

/**
 * A socket.  Opaque.  Created by gcu_socket_create() or delivered by an
 * accept completion, and released by gcu_socket_close().
 */
typedef struct GCU_Socket GCU_Socket;

/**
 * Build an IPv4 address.
 *
 * @param out Receives the address.
 * @param bytes The four address bytes in network order, `{127, 0, 0, 1}`.
 * @param port The port, host order; 0 asks the OS to choose when binding.
 * @return ::GCU_SOCKET_OK or ::GCU_SOCKET_ERR_INVALID for NULL.
 */
GCU_API GCU_Socket_Result gcu_socket_address_ipv4(
  GCU_Socket_Address * out, const uint8_t bytes[4], uint16_t port);

/**
 * Build an IPv6 address.
 *
 * @param out Receives the address.
 * @param bytes The sixteen address bytes in network order.
 * @param scope_id The zone index for a link-local address, else 0.
 * @param port The port, host order.
 * @return ::GCU_SOCKET_OK or ::GCU_SOCKET_ERR_INVALID for NULL.
 */
GCU_API GCU_Socket_Result gcu_socket_address_ipv6(GCU_Socket_Address * out,
  const uint8_t bytes[16], uint32_t scope_id, uint16_t port);

/**
 * The loopback address, `127.0.0.1` or `::1`.
 *
 * @param out Receives the address.
 * @param family The family.
 * @param port The port.
 * @return ::GCU_SOCKET_OK or ::GCU_SOCKET_ERR_INVALID.
 */
GCU_API GCU_Socket_Result gcu_socket_address_loopback(
  GCU_Socket_Address * out, GCU_Socket_Family family, uint16_t port);

/**
 * The wildcard address, `0.0.0.0` or `::`.
 *
 * @param out Receives the address.
 * @param family The family.
 * @param port The port.
 * @return ::GCU_SOCKET_OK or ::GCU_SOCKET_ERR_INVALID.
 */
GCU_API GCU_Socket_Result gcu_socket_address_any(
  GCU_Socket_Address * out, GCU_Socket_Family family, uint16_t port);

/**
 * Parse a numeric address: dotted-quad IPv4 or colon-hex IPv6.  A name is
 * not resolved, and a string that is not a number is refused.
 *
 * @param out Receives the address; not written on failure.
 * @param text The address, without a port or brackets.
 * @param port The port.
 * @return ::GCU_SOCKET_OK, or ::GCU_SOCKET_ERR_INVALID.
 */
GCU_API GCU_Socket_Result gcu_socket_address_parse(
  GCU_Socket_Address * out, const char * text, uint16_t port);

/**
 * Write an address as text: `127.0.0.1:80`, `[::1]:80`.  The text is for
 * people and logs and is **not** the input of gcu_socket_address_parse(),
 * which takes a bare address: this has a port, brackets and (for a zone) a
 * `%scope` that parse refuses.
 *
 * @param address The address.
 * @param buffer Destination.
 * @param size Bytes in @p buffer; ::GCU_SOCKET_ADDRESS_STRING_MAX is always
 *   enough.
 * @return ::GCU_SOCKET_OK, or ::GCU_SOCKET_ERR_INVALID for a NULL argument,
 *   a zero @p size, an address with an unknown family, or a buffer too small
 *   for the whole text (it is then set to an empty string: truncated text
 *   could name a different address).
 */
GCU_API GCU_Socket_Result gcu_socket_address_format(
  const GCU_Socket_Address * address, char * buffer, size_t size);

/**
 * Create a socket.  It is non-blocking, not inherited by child processes,
 * and not bound.
 *
 * @param out_socket Receives the socket; written only on success.  Release
 *   it with gcu_socket_close().
 * @param family The address family.
 * @param type Stream or datagram.
 * @param allocator Allocator for the descriptor, or `NULL` for the default.
 *   It must outlive the socket.
 * @return ::GCU_SOCKET_OK; ::GCU_SOCKET_ERR_INVALID for NULL or an unknown
 *   family or type; ::GCU_SOCKET_ERR_OOM; or ::GCU_SOCKET_ERR_OS.
 */
GCU_API GCU_Socket_Result gcu_socket_create(GCU_Socket ** out_socket,
  GCU_Socket_Family family, GCU_Socket_Type type,
  const GCU_Allocator * allocator);

/**
 * Bind a socket to a local address.  Port 0 lets the OS choose; read the
 * choice back with gcu_socket_local_address().
 *
 * @param socket The socket.
 * @param address A local address of the socket's family.
 * @return ::GCU_SOCKET_OK; ::GCU_SOCKET_ERR_INVALID for NULL, a bad address
 *   or one of the wrong family; or ::GCU_SOCKET_ERR_OS (`EADDRINUSE`
 *   included).
 */
GCU_API GCU_Socket_Result gcu_socket_bind(
  GCU_Socket * socket, const GCU_Socket_Address * address);

/**
 * Start listening for connections.  Stream sockets only.
 *
 * @param socket A bound stream socket.
 * @param backlog How many connections the OS may queue; 0 or less picks a
 *   default.
 * @return ::GCU_SOCKET_OK; ::GCU_SOCKET_ERR_INVALID for NULL or a datagram
 *   socket; or ::GCU_SOCKET_ERR_OS.
 */
GCU_API GCU_Socket_Result gcu_socket_listen(GCU_Socket * socket, int backlog);

/**
 * The address the socket is bound to.
 *
 * @param socket The socket.
 * @param out Receives the address.
 * @return ::GCU_SOCKET_OK, ::GCU_SOCKET_ERR_INVALID or ::GCU_SOCKET_ERR_OS.
 */
GCU_API GCU_Socket_Result gcu_socket_local_address(
  const GCU_Socket * socket, GCU_Socket_Address * out);

/**
 * The address of the peer a socket is connected to.
 *
 * @param socket A connected socket.
 * @param out Receives the address.
 * @return ::GCU_SOCKET_OK, ::GCU_SOCKET_ERR_INVALID or ::GCU_SOCKET_ERR_OS
 *   (`ENOTCONN` when not connected).
 */
GCU_API GCU_Socket_Result gcu_socket_peer_address(
  const GCU_Socket * socket, GCU_Socket_Address * out);

/**
 * Set an option.
 *
 * @param socket The socket.
 * @param option Which option.
 * @param value Its value; see ::GCU_Socket_Option.
 * @return ::GCU_SOCKET_OK; ::GCU_SOCKET_ERR_INVALID for NULL or an unknown
 *   option; or ::GCU_SOCKET_ERR_OS.
 */
GCU_API GCU_Socket_Result gcu_socket_set_option(
  GCU_Socket * socket, GCU_Socket_Option option, int value);

/**
 * Read an option back.  What the OS reports may differ from what was set
 * (Linux doubles a buffer size).
 *
 * @param socket The socket.
 * @param option Which option.
 * @param out_value Receives the value.
 * @return As gcu_socket_set_option().
 */
GCU_API GCU_Socket_Result gcu_socket_get_option(
  const GCU_Socket * socket, GCU_Socket_Option option, int * out_value);

/**
 * Switch non-blocking mode.  Sockets start non-blocking, and the loop
 * refuses one that is not (`GCU_LOOP_ERR_STATE`), so this exists for a
 * caller that hands the socket to code of its own.  Making a socket blocking
 * is refused with ::GCU_SOCKET_ERR_STATE once a loop has used it or while an
 * operation is in flight, because the loop's thread would then block in the
 * call it makes for it.
 *
 * @param socket The socket.
 * @param nonblocking `true` for non-blocking.
 * @return ::GCU_SOCKET_OK, ::GCU_SOCKET_ERR_INVALID, ::GCU_SOCKET_ERR_STATE
 *   (see above) or ::GCU_SOCKET_ERR_OS.
 */
GCU_API GCU_Socket_Result gcu_socket_set_nonblocking(
  GCU_Socket * socket, bool nonblocking);

/**
 * End one or both directions of a connection without closing the socket.
 *
 * @param socket A connected stream socket.
 * @param how Which direction.
 * @return ::GCU_SOCKET_OK, ::GCU_SOCKET_ERR_INVALID or ::GCU_SOCKET_ERR_OS.
 */
GCU_API GCU_Socket_Result gcu_socket_shutdown(
  GCU_Socket * socket, GCU_Socket_Shutdown how);

/**
 * The socket's family.
 *
 * @param socket The socket.
 * @return The family, or 0 for NULL.
 */
GCU_API GCU_Socket_Family gcu_socket_family(const GCU_Socket * socket);

/**
 * The socket's type.
 *
 * @param socket The socket.
 * @return The type, or 0 for NULL.
 */
GCU_API GCU_Socket_Type gcu_socket_type(const GCU_Socket * socket);

/**
 * Close a socket and free it.
 *
 * Refused while an operation on the socket is in flight: cancel it and wait
 * for its completion first, since the operation's buffer is the loop's until
 * then.  A close from inside a completion callback is fine, including of the
 * socket the callback was told about.  The socket's memory is released at
 * the end of the loop iteration that closed it, so an event already
 * collected for it is not a use after free.
 *
 * @param socket The socket, or `NULL`, which is ignored.
 * @return ::GCU_SOCKET_OK; or ::GCU_SOCKET_ERR_STATE when operations are in
 *   flight, with the socket untouched.
 */
GCU_API GCU_Socket_Result gcu_socket_close(GCU_Socket * socket);

/**
 * Sort an OS error code into a kind.
 *
 * @param os_error A code from gcu_error_last() or a completion's `os_error`.
 * @return The kind, or ::GCU_SOCKET_ERROR_OTHER.
 */
GCU_API GCU_Socket_Error_Kind gcu_socket_error_kind(int os_error);

/**
 * A short name for a result, for messages.
 *
 * @param result Any value; one out of range gets a placeholder.
 * @return A string with static storage.
 */
GCU_API const char * gcu_socket_result_string(GCU_Socket_Result result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_SOCKET_H
