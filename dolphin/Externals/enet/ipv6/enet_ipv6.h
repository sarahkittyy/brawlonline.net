/*
 * IPv6 for ENet (Brawl Online). Not part of upstream ENet.
 *
 * Upstream ENet 1.3 speaks IPv4 only: ENetAddress::host is an IPv4 address. Here an IPv6 socket
 * names each IPv6 address it talks to by a 32-bit handle in ENetAddress::host, an address in
 * 240.0.0.0/8 (reserved "class E" space: nothing on the internet sends from it). ENet's protocol
 * code and anything that compares, stores or copies ENetAddress values work unchanged, and IPv4
 * sockets never see a handle.
 *
 * The socket functions below wrap upstream's (compiled from enet/win32.c and enet/unix.c under
 * other names, see CMakeLists.txt): for an IPv4 socket they call upstream's function and nothing
 * else, so IPv4 behaves exactly as before; only sockets made by enet_socket_create_ipv6 (or a
 * host made by enet_host_create_ipv6) take the IPv6 code. Those are IPV6_V6ONLY, so they never
 * carry IPv4 traffic.
 */
#ifndef ENET_BRAWL_IPV6_H
#define ENET_BRAWL_IPV6_H

#include <enet/enet.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENET_IPV6_TEXT_MAX 64 /* room for any IPv6 address in text, with a NUL */

/* A handle is ENetAddress::host whose first byte (network order) is 240. */
ENET_API int enet_address_is_ipv6 (const ENetAddress * address);

/* An IPv6 address in text ("2001:db8::1", no brackets, no zone) to its handle. -1 if the text is
   not an IPv6 address (an IPv4 address is not), or the table of addresses is full. */
ENET_API int enet_address_set_host_ipv6 (ENetAddress * address, const char * text);

/* The 16 bytes of a handle's address (network order). -1 if `host` is no handle. */
ENET_API int enet_ipv6_handle_bytes (enet_uint32 host, enet_uint8 * bytes16);
/* The handle of 16 address bytes (made on first use). 0 if the table is full. */
ENET_API enet_uint32 enet_ipv6_handle_of (const enet_uint8 * bytes16);

/* An IPv6 UDP socket (IPV6_V6ONLY). ENET_SOCKET_NULL if the system has no IPv6. */
ENET_API ENetSocket enet_socket_create_ipv6 (ENetSocketType type);
/* Whether `socket` came from enet_socket_create_ipv6 (and is not destroyed yet). */
ENET_API int enet_socket_is_ipv6 (ENetSocket socket);

/* enet_host_create on an IPv6 socket. `address`: its host a handle (bind to that address) or
   ENET_HOST_ANY (any IPv6 address of this machine), and the port. NULL if no IPv6. */
ENET_API ENetHost * enet_host_create_ipv6 (const ENetAddress * address, size_t peerCount,
                                           size_t channelLimit, enet_uint32 incomingBandwidth,
                                           enet_uint32 outgoingBandwidth);

#ifdef __cplusplus
}
#endif

#endif /* ENET_BRAWL_IPV6_H */
