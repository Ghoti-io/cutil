/**
 * @file
 *
 * Tests for sockets: addresses, creation, options and the error mapping.
 * Everything that moves bytes goes through the loop and is in test-loop.cpp.
 *
 * Loopback only, and port 0 so that nothing here can collide with another
 * process.  IPv6 is reported as an explicit skip where `::1` is not usable,
 * never as a pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gtest/gtest.h>
#include <ghoti.io/cutil/error.h>
#include <ghoti.io/cutil/socket.h>

#include "hang-guard.h"

#include <cerrno>
#include <cstring>
#include <string>

using namespace std;

namespace {

class SocketTest : public ghoti_test::HangGuarded<60> {};

/**
 * Owns a socket and closes it.
 */
struct Sock {
  GCU_Socket * s = nullptr;
  ~Sock() { gcu_socket_close(s); }
};

bool ipv6Usable() {
  GCU_Socket * s = nullptr;
  if (gcu_socket_create(&s, GCU_SOCKET_IPV6, GCU_SOCKET_STREAM, nullptr)
      != GCU_SOCKET_OK) {
    return false;
  }
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV6, 0);
  bool ok = gcu_socket_bind(s, &a) == GCU_SOCKET_OK;
  gcu_socket_close(s);
  return ok;
}

#define REQUIRE_IPV6() \
  do { \
    if (!ipv6Usable()) { \
      GTEST_SKIP() << "::1 is not available on this host"; \
    } \
  } while (0)

string text(const GCU_Socket_Address & a) {
  char buffer[GCU_SOCKET_ADDRESS_STRING_MAX];
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_address_format(&a, buffer, sizeof(buffer)));
  return buffer;
}

TEST_F(SocketTest, Ipv4AddressFormatsAsHostAndPort) {
  const uint8_t bytes[4] = {192, 168, 1, 20};
  GCU_Socket_Address a;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_ipv4(&a, bytes, 8080));
  EXPECT_EQ(sizeof(GCU_Socket_Address), a.size);
  EXPECT_EQ(GCU_SOCKET_IPV4, a.family);
  EXPECT_EQ("192.168.1.20:8080", text(a));
}

TEST_F(SocketTest, Ipv6AddressFormatsInBracketsAndKeepsItsZone) {
  GCU_Socket_Address a;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_loopback(&a, GCU_SOCKET_IPV6, 443));
  EXPECT_EQ("[::1]:443", text(a));
  const uint8_t bytes[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_ipv6(&a, bytes, 3, 9));
  EXPECT_EQ("[fe80::1%3]:9", text(a));
}

TEST_F(SocketTest, LoopbackAndWildcardAddressesAreTheRightNumbers) {
  GCU_Socket_Address a;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 1));
  EXPECT_EQ("127.0.0.1:1", text(a));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_any(&a, GCU_SOCKET_IPV4, 2));
  EXPECT_EQ("0.0.0.0:2", text(a));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_any(&a, GCU_SOCKET_IPV6, 3));
  EXPECT_EQ("[::]:3", text(a));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_address_loopback(&a, (GCU_Socket_Family)9, 1));
}

TEST_F(SocketTest, ParseTakesNumbersAndRefusesNames) {
  GCU_Socket_Address a;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_parse(&a, "10.0.0.7", 53));
  EXPECT_EQ("10.0.0.7:53", text(a));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_parse(&a, "2001:db8::2", 53));
  EXPECT_EQ("[2001:db8::2]:53", text(a));

  // A name is never resolved here, and a number that is not a whole address is
  // not guessed at.
  GCU_Socket_Address untouched;
  memset(&untouched, 0x5a, sizeof(untouched));
  a = untouched;
  for (const char * bad : {"localhost", "1.2.3", "256.1.1.1", "", "::g", "1.2.3.4:80"}) {
    EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_parse(&a, bad, 1))
      << bad;
  }
  EXPECT_EQ(0, memcmp(&a, &untouched, sizeof(a))) << "written on failure";
}

TEST_F(SocketTest, FormatRefusesABufferShorterThanTheTextAndLeavesItEmpty) {
  GCU_Socket_Address a;
  gcu_socket_address_parse(&a, "255.255.255.255", 65535);
  char small[8];
  memset(small, 'x', sizeof(small));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_address_format(&a, small, sizeof(small)))
    << "truncated text that parses as another address is not an answer";
  EXPECT_EQ('\0', small[0]);
  // The text is 21 characters: 22 bytes is exactly enough, 21 is not.
  char exact[22];
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_format(&a, exact, sizeof(exact)));
  EXPECT_STREQ("255.255.255.255:65535", exact);
  char shortByOne[21];
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_address_format(&a, shortByOne, sizeof(shortByOne)));

  char one[1] = {'x'};
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_format(&a, one, 1));
  EXPECT_EQ('\0', one[0]);
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_format(&a, one, 0));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_format(nullptr, one, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_format(&a, nullptr, 1));

  // The longest address there is fits the constant that promises it.
  GCU_Socket_Address longest;
  const uint8_t all[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  gcu_socket_address_ipv6(&longest, all, 0xffffffffu, 65535);
  char buffer[GCU_SOCKET_ADDRESS_STRING_MAX];
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_address_format(&longest, buffer, sizeof(buffer)));
  EXPECT_EQ("[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295]:65535",
    string(buffer));
}

TEST_F(SocketTest, ConstructorsRefuseNull) {
  const uint8_t bytes[16] = {0};
  GCU_Socket_Address a;
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_ipv4(nullptr, bytes, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_ipv4(&a, nullptr, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_ipv6(nullptr, bytes, 0, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_ipv6(&a, nullptr, 0, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_parse(nullptr, "1.1.1.1", 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_address_parse(&a, nullptr, 1));
}

TEST_F(SocketTest, AnAddressSmallerThanTheFirstGenerationIsRefused) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 0);
  a.size = sizeof(a) - 1;
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_bind(s.s, &a));
  char buffer[GCU_SOCKET_ADDRESS_STRING_MAX];
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_address_format(&a, buffer, sizeof(buffer)));
  a.size = 0;
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_bind(s.s, &a));
}

TEST_F(SocketTest, CreateRefusesBadArgumentsAndWritesNothing) {
  GCU_Socket * s = reinterpret_cast<GCU_Socket *>(0x1);
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_create(nullptr, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_create(&s, (GCU_Socket_Family)0, GCU_SOCKET_STREAM, nullptr));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_create(&s, GCU_SOCKET_IPV4, (GCU_Socket_Type)7, nullptr));
  EXPECT_EQ(reinterpret_cast<GCU_Socket *>(0x1), s);
}

TEST_F(SocketTest, ACreatedSocketReportsItsFamilyAndType) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, nullptr));
  EXPECT_EQ(GCU_SOCKET_IPV4, gcu_socket_family(s.s));
  EXPECT_EQ(GCU_SOCKET_DATAGRAM, gcu_socket_type(s.s));
  EXPECT_EQ(0, (int)gcu_socket_family(nullptr));
  EXPECT_EQ(0, (int)gcu_socket_type(nullptr));
}

TEST_F(SocketTest, BindingPortZeroReportsThePortTheOsChose) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(s.s, &a));
  GCU_Socket_Address local;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(s.s, &local));
  EXPECT_NE(0, local.port);
  EXPECT_EQ(0, memcmp(local.bytes, a.bytes, 4));
  EXPECT_EQ(sizeof(GCU_Socket_Address), local.size);
}

TEST_F(SocketTest, BindRefusesAnAddressOfTheWrongFamily) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV6, 0);
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_bind(s.s, &a));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_bind(s.s, nullptr));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_bind(nullptr, &a));
}

TEST_F(SocketTest, BindingAnAddressInUseIsAnOsErrorOfThatKind) {
  Sock first;
  Sock second;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&first.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&second.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(first.s, &a));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(first.s, 1));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(first.s, &a));
  ASSERT_EQ(GCU_SOCKET_ERR_OS, gcu_socket_bind(second.s, &a));
  int error = gcu_error_last();
  EXPECT_EQ(GCU_SOCKET_ERROR_ADDRESS_IN_USE, gcu_socket_error_kind(error));
#ifndef _WIN32
  // The code goes straight to the error API.  Not asserted on Windows, where
  // the text of a Winsock code comes from the system's message table, which
  // wine does not carry for every code; the kind above is the property.
  char message[GCU_ERROR_STRING_MAX];
  EXPECT_EQ(0, gcu_error_string(error, message, sizeof(message)));
  EXPECT_GT(strlen(message), 0u);
#endif
}

TEST_F(SocketTest, ListenIsForBoundStreamSocketsOnly) {
  Sock datagram;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&datagram.s, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, nullptr));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_listen(datagram.s, 4));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_listen(nullptr, 4));
  Sock stream;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&stream.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(stream.s, &a));
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_listen(stream.s, 0))
    << "a backlog of 0 or less picks a default";
}

TEST_F(SocketTest, OptionsReadBackWhatWasSet) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  int v = -7;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_REUSE_ADDRESS, 1));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_REUSE_ADDRESS, &v));
  EXPECT_NE(0, v);
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_NO_DELAY, 1));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_NO_DELAY, &v));
  EXPECT_NE(0, v);
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_KEEP_ALIVE, 1));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_KEEP_ALIVE, &v));
  EXPECT_NE(0, v);
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_NO_DELAY, 0));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_NO_DELAY, &v));
  EXPECT_EQ(0, v);

  // The OS may round a buffer, and Linux doubles it, so only a floor holds.
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_SEND_BUFFER, 8192));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_SEND_BUFFER, &v));
  EXPECT_GE(v, 4096);
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_RECV_BUFFER, 8192));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_RECV_BUFFER, &v));
  EXPECT_GE(v, 2048);

  // Linger: negative is off, otherwise seconds, and zero is an abortive close.
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_set_option(s.s, GCU_SOCKET_OPT_LINGER, 0));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_get_option(s.s, GCU_SOCKET_OPT_LINGER, &v));
  EXPECT_EQ(0, v);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_set_option(s.s, GCU_SOCKET_OPT_LINGER, 5));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_get_option(s.s, GCU_SOCKET_OPT_LINGER, &v));
  EXPECT_EQ(5, v);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_set_option(s.s, GCU_SOCKET_OPT_LINGER, -1));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_get_option(s.s, GCU_SOCKET_OPT_LINGER, &v));
  EXPECT_EQ(-1, v);
}

TEST_F(SocketTest, OptionCallsRefuseNullAndUnknownOptions) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  int v = 0;
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_set_option(nullptr, GCU_SOCKET_OPT_NO_DELAY, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_set_option(s.s, (GCU_Socket_Option)99, 1));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_NO_DELAY, nullptr));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_get_option(s.s, (GCU_Socket_Option)99, &v));
  // The linger time is an unsigned short in the OS; a longer one would wrap.
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_LINGER, 65536));
  EXPECT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_LINGER, 65535));
}

TEST_F(SocketTest, AnIpv6SocketIsIpv6OnlyUntilToldOtherwise) {
  REQUIRE_IPV6();
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV6, GCU_SOCKET_STREAM, nullptr));
  int v = 0;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_IPV6_ONLY, &v));
  EXPECT_NE(0, v) << "Linux defaults to dual-stack; the library says one thing";
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(s.s, GCU_SOCKET_OPT_IPV6_ONLY, 0));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_get_option(s.s, GCU_SOCKET_OPT_IPV6_ONLY, &v));
  EXPECT_EQ(0, v);
}

TEST_F(SocketTest, TheAddressOfAnUnconnectedSocketIsAnOsErrorOfThatKind) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  ASSERT_EQ(GCU_SOCKET_ERR_OS, gcu_socket_peer_address(s.s, &a));
  EXPECT_EQ(GCU_SOCKET_ERROR_NOT_CONNECTED,
    gcu_socket_error_kind(gcu_error_last()));
  ASSERT_EQ(GCU_SOCKET_ERR_OS,
    gcu_socket_shutdown(s.s, GCU_SOCKET_SHUTDOWN_WRITE));
  EXPECT_EQ(GCU_SOCKET_ERROR_NOT_CONNECTED,
    gcu_socket_error_kind(gcu_error_last()));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID,
    gcu_socket_shutdown(s.s, (GCU_Socket_Shutdown)0));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_peer_address(nullptr, &a));
}

TEST_F(SocketTest, NonBlockingModeCanBeSwitchedAndSwitchedBack) {
  Sock s;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s.s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_set_nonblocking(s.s, false));
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_set_nonblocking(s.s, true));
  EXPECT_EQ(GCU_SOCKET_ERR_INVALID, gcu_socket_set_nonblocking(nullptr, true));
}

TEST_F(SocketTest, ErrorKindsAreTheSameOnEveryPlatform) {
#ifdef _WIN32
  EXPECT_EQ(GCU_SOCKET_ERROR_REFUSED, gcu_socket_error_kind(10061));
  EXPECT_EQ(GCU_SOCKET_ERROR_RESET, gcu_socket_error_kind(10054));
  EXPECT_EQ(GCU_SOCKET_ERROR_ABORTED, gcu_socket_error_kind(10053));
  EXPECT_EQ(GCU_SOCKET_ERROR_TIMED_OUT, gcu_socket_error_kind(10060));
  EXPECT_EQ(GCU_SOCKET_ERROR_UNREACHABLE, gcu_socket_error_kind(10051));
  EXPECT_EQ(GCU_SOCKET_ERROR_UNREACHABLE, gcu_socket_error_kind(10065));
  EXPECT_EQ(GCU_SOCKET_ERROR_ADDRESS_IN_USE, gcu_socket_error_kind(10048));
  EXPECT_EQ(GCU_SOCKET_ERROR_NOT_CONNECTED, gcu_socket_error_kind(10057));
  EXPECT_EQ(GCU_SOCKET_ERROR_CANCELLED, gcu_socket_error_kind(995));
#else
  EXPECT_EQ(GCU_SOCKET_ERROR_REFUSED, gcu_socket_error_kind(ECONNREFUSED));
  EXPECT_EQ(GCU_SOCKET_ERROR_RESET, gcu_socket_error_kind(ECONNRESET));
  EXPECT_EQ(GCU_SOCKET_ERROR_ABORTED, gcu_socket_error_kind(ECONNABORTED));
  EXPECT_EQ(GCU_SOCKET_ERROR_TIMED_OUT, gcu_socket_error_kind(ETIMEDOUT));
  EXPECT_EQ(GCU_SOCKET_ERROR_UNREACHABLE, gcu_socket_error_kind(ENETUNREACH));
  EXPECT_EQ(GCU_SOCKET_ERROR_UNREACHABLE, gcu_socket_error_kind(EHOSTUNREACH));
  EXPECT_EQ(GCU_SOCKET_ERROR_ADDRESS_IN_USE, gcu_socket_error_kind(EADDRINUSE));
  EXPECT_EQ(GCU_SOCKET_ERROR_NOT_CONNECTED, gcu_socket_error_kind(ENOTCONN));
  EXPECT_EQ(GCU_SOCKET_ERROR_BROKEN_PIPE, gcu_socket_error_kind(EPIPE));
  EXPECT_EQ(GCU_SOCKET_ERROR_CANCELLED, gcu_socket_error_kind(ECANCELED));
#endif
  EXPECT_EQ(GCU_SOCKET_ERROR_OTHER, gcu_socket_error_kind(0));
  EXPECT_EQ(GCU_SOCKET_ERROR_OTHER, gcu_socket_error_kind(-12345));
}

TEST_F(SocketTest, EveryResultHasADistinctName) {
  for (int r = 0; r < (int)GCU_SOCKET_RESULT_COUNT; ++r) {
    const char * name = gcu_socket_result_string((GCU_Socket_Result)r);
    ASSERT_NE(nullptr, name);
    EXPECT_GT(strlen(name), 0u) << r;
    for (int q = 0; q < r; ++q) {
      EXPECT_STRNE(name, gcu_socket_result_string((GCU_Socket_Result)q));
    }
  }
  EXPECT_STREQ("unknown socket result",
    gcu_socket_result_string((GCU_Socket_Result)99));
  EXPECT_STREQ("unknown socket result",
    gcu_socket_result_string((GCU_Socket_Result)-1));
}

TEST_F(SocketTest, ClosingNullIsIgnored) {
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_close(nullptr));
}

//
// Allocation failure.
//

struct NthFails {
  int remaining;  // the allocation that fails; counts down, 0 fails now
  int allocs = 0;
  int frees = 0;
};
void * nthCalloc(void * ctx, size_t n, size_t size) {
  NthFails * c = static_cast<NthFails *>(ctx);
  if (c->remaining-- == 0) return nullptr;
  ++c->allocs;
  return calloc(n, size);
}
void * nthMalloc(void * ctx, size_t size) {
  NthFails * c = static_cast<NthFails *>(ctx);
  if (c->remaining-- == 0) return nullptr;
  ++c->allocs;
  return malloc(size);
}
void * nthRealloc(void * ctx, void * p, size_t size) {
  NthFails * c = static_cast<NthFails *>(ctx);
  if (c->remaining-- == 0) return nullptr;
  if (!p) ++c->allocs;
  return realloc(p, size);
}
void nthFree(void * ctx, void * p) {
  NthFails * c = static_cast<NthFails *>(ctx);
  if (p) ++c->frees;
  free(p);
}

TEST_F(SocketTest, AnAllocationFailureIsAnErrorThatLeaksNothingAndWritesNothing) {
  GCU_Allocator alloc = {nullptr, nthMalloc, nthCalloc, nthRealloc, nthFree};
  NthFails counts{0};
  alloc.ctx = &counts;
  GCU_Socket * s = reinterpret_cast<GCU_Socket *>(0x1);
  EXPECT_EQ(GCU_SOCKET_ERR_OOM,
    gcu_socket_create(&s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, &alloc));
  EXPECT_EQ(reinterpret_cast<GCU_Socket *>(0x1), s);
  EXPECT_EQ(counts.allocs, counts.frees);

  counts.remaining = 1;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, &alloc));
  EXPECT_EQ(1, counts.allocs);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_close(s));
  EXPECT_EQ(counts.allocs, counts.frees);
}

} // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
