/*
 * IPv6 for ENet (Brawl Online). See enet_ipv6.h.
 *
 * Upstream's platform file (enet/win32.c or enet/unix.c) is compiled with its socket and address
 * functions renamed to enet_v4_* (CMakeLists.txt); the functions of the same names here are what
 * ENet and its users call. Each first asks whether the socket is one of ours (IPv6); if not, it
 * calls upstream's function with the same arguments and returns what it returns.
 */
#define ENET_BUILDING_LIB 1
#include "enet_ipv6.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

/* Upstream's functions, renamed (CMakeLists.txt). */
ENetSocket enet_v4_socket_create (ENetSocketType);
int enet_v4_socket_bind (ENetSocket, const ENetAddress *);
int enet_v4_socket_get_address (ENetSocket, ENetAddress *);
int enet_v4_socket_connect (ENetSocket, const ENetAddress *);
int enet_v4_socket_send (ENetSocket, const ENetAddress *, const ENetBuffer *, size_t);
int enet_v4_socket_receive (ENetSocket, ENetAddress *, ENetBuffer *, size_t);
void enet_v4_socket_destroy (ENetSocket);
int enet_v4_address_set_host_ip (ENetAddress *, const char *);
int enet_v4_address_set_host (ENetAddress *, const char *);
int enet_v4_address_get_host_ip (const ENetAddress *, char *, size_t);
int enet_v4_address_get_host (const ENetAddress *, char *, size_t);

/* ---- A lock (a static one: no init call needed) ---- */

#ifdef _WIN32
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive (& lock)
#define UNLOCK() ReleaseSRWLockExclusive (& lock)
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock (& lock)
#define UNLOCK() pthread_mutex_unlock (& lock)
#endif

/* ---- Our sockets ---- */

#define MAX_V6_SOCKETS 64
static ENetSocket v6Sockets [MAX_V6_SOCKETS];
static int v6SocketCount = 0;
/* Read without the lock first: a process with no IPv6 socket pays one load per call. */
static volatile int anyV6Socket = 0;

int
enet_socket_is_ipv6 (ENetSocket socket)
{
    int i, found = 0;
    if (! anyV6Socket || socket == ENET_SOCKET_NULL)
      return 0;
    LOCK ();
    for (i = 0; i < v6SocketCount; ++ i)
      if (v6Sockets [i] == socket)
      {
          found = 1;
          break;
      }
    UNLOCK ();
    return found;
}

static int
add_v6_socket (ENetSocket socket)
{
    int ok = 0;
    LOCK ();
    if (v6SocketCount < MAX_V6_SOCKETS)
    {
        v6Sockets [v6SocketCount ++] = socket;
        anyV6Socket = 1;
        ok = 1;
    }
    UNLOCK ();
    return ok;
}

static void
remove_v6_socket (ENetSocket socket)
{
    int i;
    LOCK ();
    for (i = 0; i < v6SocketCount; ++ i)
      if (v6Sockets [i] == socket)
      {
          v6Sockets [i] = v6Sockets [-- v6SocketCount];
          break;
      }
    anyV6Socket = v6SocketCount > 0;
    UNLOCK ();
}

/* ---- Handles ----
   Entry i (1-based) is the address of handle 240.(i>>16).(i>>8).i; `slots` is an open-addressing
   table of entry numbers by the address's hash. Entries are never removed: a process talks to few
   IPv6 addresses (the server's, its peers'), and a full table only refuses new ones. */

#define MAX_HANDLES 65535
#define SLOT_COUNT 131072 /* a power of two, at least twice MAX_HANDLES */
#define HANDLE_BYTE 240

static enet_uint8 (* entries) [16] = NULL; /* entries [i - 1] */
static enet_uint32 * slots = NULL;
static enet_uint32 entryCount = 0;

static enet_uint32
hash16 (const enet_uint8 * b)
{
    enet_uint32 h = 2166136261u;
    int i;
    for (i = 0; i < 16; ++ i)
    {
        h ^= b [i];
        h *= 16777619u;
    }
    return h;
}

static enet_uint32
handle_from_index (enet_uint32 i)
{
    enet_uint8 b [4];
    enet_uint32 host;
    b [0] = HANDLE_BYTE;
    b [1] = (enet_uint8) (i >> 16);
    b [2] = (enet_uint8) (i >> 8);
    b [3] = (enet_uint8) i;
    memcpy (& host, b, 4);
    return host;
}

static enet_uint32
index_from_handle (enet_uint32 host)
{
    enet_uint8 b [4];
    memcpy (b, & host, 4);
    if (b [0] != HANDLE_BYTE)
      return 0;
    return ((enet_uint32) b [1] << 16) | ((enet_uint32) b [2] << 8) | b [3];
}

int
enet_address_is_ipv6 (const ENetAddress * address)
{
    return address != NULL && index_from_handle (address -> host) != 0;
}

enet_uint32
enet_ipv6_handle_of (const enet_uint8 * bytes16)
{
    enet_uint32 slot, index = 0;
    LOCK ();
    if (entries == NULL)
    {
        entries = (enet_uint8 (*) [16]) enet_malloc (MAX_HANDLES * 16);
        slots = (enet_uint32 *) enet_malloc (SLOT_COUNT * sizeof (enet_uint32));
        if (entries == NULL || slots == NULL)
        {
            if (entries != NULL)
              enet_free (entries);
            if (slots != NULL)
              enet_free (slots);
            entries = NULL;
            slots = NULL;
            UNLOCK ();
            return 0;
        }
        memset (slots, 0, SLOT_COUNT * sizeof (enet_uint32));
    }
    slot = hash16 (bytes16) & (SLOT_COUNT - 1);
    for (;;)
    {
        enet_uint32 e = slots [slot];
        if (e == 0)
        {
            if (entryCount < MAX_HANDLES)
            {
                memcpy (entries [entryCount], bytes16, 16);
                index = ++ entryCount;
                slots [slot] = index;
            }
            break;
        }
        if (memcmp (entries [e - 1], bytes16, 16) == 0)
        {
            index = e;
            break;
        }
        slot = (slot + 1) & (SLOT_COUNT - 1);
    }
    UNLOCK ();
    return index != 0 ? handle_from_index (index) : 0;
}

int
enet_ipv6_handle_bytes (enet_uint32 host, enet_uint8 * bytes16)
{
    enet_uint32 index = index_from_handle (host);
    int ok = 0;
    if (index == 0)
      return -1;
    LOCK ();
    if (entries != NULL && index <= entryCount)
    {
        memcpy (bytes16, entries [index - 1], 16);
        ok = 1;
    }
    UNLOCK ();
    return ok ? 0 : -1;
}

static int
is_unspecified (const enet_uint8 * b)
{
    static const enet_uint8 zero [16] = { 0 };
    return memcmp (b, zero, 16) == 0;
}

/* sockaddr_in6 <-> ENetAddress. The unspecified address is ENET_HOST_ANY. */
static int
to_sockaddr (const ENetAddress * address, struct sockaddr_in6 * sin6)
{
    memset (sin6, 0, sizeof (struct sockaddr_in6));
    sin6 -> sin6_family = AF_INET6;
    if (address == NULL)
      return 0;
    sin6 -> sin6_port = ENET_HOST_TO_NET_16 (address -> port);
    if (address -> host == ENET_HOST_ANY)
      return 0;
    return enet_ipv6_handle_bytes (address -> host, (enet_uint8 *) & sin6 -> sin6_addr);
}

static int
from_sockaddr (const struct sockaddr_in6 * sin6, ENetAddress * address)
{
    const enet_uint8 * b = (const enet_uint8 *) & sin6 -> sin6_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin6 -> sin6_port);
    if (is_unspecified (b))
    {
        address -> host = ENET_HOST_ANY;
        return 0;
    }
    address -> host = enet_ipv6_handle_of (b);
    return address -> host != 0 ? 0 : -1;
}

/* ---- Addresses in text ---- */

int
enet_address_set_host_ipv6 (ENetAddress * address, const char * text)
{
    struct in6_addr a;
    enet_uint32 host;
    if (text == NULL || strchr (text, ':') == NULL || inet_pton (AF_INET6, text, & a) != 1)
      return -1;
    if (is_unspecified ((const enet_uint8 *) & a))
    {
        address -> host = ENET_HOST_ANY;
        return 0;
    }
    host = enet_ipv6_handle_of ((const enet_uint8 *) & a);
    if (host == 0)
      return -1;
    address -> host = host;
    return 0;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    if (name != NULL && strchr (name, ':') != NULL)
      return enet_address_set_host_ipv6 (address, name);
    return enet_v4_address_set_host_ip (address, name);
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    if (name != NULL && strchr (name, ':') != NULL)
      return enet_address_set_host_ipv6 (address, name);
    return enet_v4_address_set_host (address, name);
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    struct in6_addr a;
    if (! enet_address_is_ipv6 (address))
      return enet_v4_address_get_host_ip (address, name, nameLength);
    if (enet_ipv6_handle_bytes (address -> host, (enet_uint8 *) & a) != 0 ||
        inet_ntop (AF_INET6, & a, name, nameLength) == NULL)
      return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    if (enet_address_is_ipv6 (address))
      return enet_address_get_host_ip (address, name, nameLength);
    return enet_v4_address_get_host (address, name, nameLength);
}

/* ---- Sockets ---- */

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return enet_v4_socket_create (type);
}

ENetSocket
enet_socket_create_ipv6 (ENetSocketType type)
{
    int on = 1;
#ifdef _WIN32
    SOCKET s = socket (AF_INET6, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
      return ENET_SOCKET_NULL;
#else
    int s = socket (AF_INET6, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s == -1)
      return ENET_SOCKET_NULL;
#endif
    /* IPv6 only: IPv4 stays on the IPv4 sockets, whatever the system's default. */
    if (setsockopt (s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) & on, sizeof (on)) != 0 ||
        ! add_v6_socket (s))
    {
        enet_v4_socket_destroy (s);
        return ENET_SOCKET_NULL;
    }
    return s;
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (enet_socket_is_ipv6 (socket))
      remove_v6_socket (socket);
    enet_v4_socket_destroy (socket);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in6 sin6;
    if (! enet_socket_is_ipv6 (socket))
      return enet_v4_socket_bind (socket, address);
    if (to_sockaddr (address, & sin6) != 0)
      return -1;
    return bind (socket, (struct sockaddr *) & sin6, sizeof (sin6)) == 0 ? 0 : -1;
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in6 sin6;
#ifdef _WIN32
    int len = sizeof (sin6);
#else
    socklen_t len = sizeof (sin6);
#endif
    if (! enet_socket_is_ipv6 (socket))
      return enet_v4_socket_get_address (socket, address);
    if (getsockname (socket, (struct sockaddr *) & sin6, & len) != 0 || sin6.sin6_family != AF_INET6)
      return -1;
    return from_sockaddr (& sin6, address);
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in6 sin6;
    int result;
    if (! enet_socket_is_ipv6 (socket))
      return enet_v4_socket_connect (socket, address);
    if (to_sockaddr (address, & sin6) != 0)
      return -1;
    result = connect (socket, (struct sockaddr *) & sin6, sizeof (sin6));
#ifdef _WIN32
    if (result == SOCKET_ERROR && WSAGetLastError () != WSAEWOULDBLOCK)
      return -1;
    return 0;
#else
    if (result == -1 && errno == EINPROGRESS)
      return 0;
    return result;
#endif
}

int
enet_socket_send (ENetSocket socket, const ENetAddress * address, const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct sockaddr_in6 sin6;
    if (! enet_socket_is_ipv6 (socket))
      return enet_v4_socket_send (socket, address, buffers, bufferCount);
    /* An address that is not ours (an IPv4 one) cannot be reached from here: dropped as lost. */
    if (address != NULL && (! enet_address_is_ipv6 (address) || to_sockaddr (address, & sin6) != 0))
    {
        size_t i, total = 0;
        for (i = 0; i < bufferCount; ++ i)
          total += buffers [i].dataLength;
        return (int) total;
    }
#ifdef _WIN32
    {
        DWORD sentLength = 0;
        if (WSASendTo (socket, (LPWSABUF) buffers, (DWORD) bufferCount, & sentLength, 0,
                       address != NULL ? (struct sockaddr *) & sin6 : NULL,
                       address != NULL ? sizeof (sin6) : 0, NULL, NULL) == SOCKET_ERROR)
        {
            if (WSAGetLastError () == WSAEWOULDBLOCK)
              return 0;
            return -1;
        }
        return (int) sentLength;
    }
#else
    {
        struct msghdr msgHdr;
        int sentLength;
        memset (& msgHdr, 0, sizeof (msgHdr));
        if (address != NULL)
        {
            msgHdr.msg_name = & sin6;
            msgHdr.msg_namelen = sizeof (sin6);
        }
        msgHdr.msg_iov = (struct iovec *) buffers;
        msgHdr.msg_iovlen = bufferCount;
        sentLength = sendmsg (socket, & msgHdr, MSG_NOSIGNAL);
        if (sentLength == -1)
        {
            if (errno == EWOULDBLOCK)
              return 0;
            return -1;
        }
        return sentLength;
    }
#endif
}

int
enet_socket_receive (ENetSocket socket, ENetAddress * address, ENetBuffer * buffers,
                     size_t bufferCount)
{
    struct sockaddr_in6 sin6;
    int recvLength;
    if (! enet_socket_is_ipv6 (socket))
      return enet_v4_socket_receive (socket, address, buffers, bufferCount);
    memset (& sin6, 0, sizeof (sin6));
#ifdef _WIN32
    {
        INT sinLength = sizeof (sin6);
        DWORD flags = 0, length = 0;
        if (WSARecvFrom (socket, (LPWSABUF) buffers, (DWORD) bufferCount, & length, & flags,
                         (struct sockaddr *) & sin6, & sinLength, NULL, NULL) == SOCKET_ERROR)
        {
            switch (WSAGetLastError ())
            {
            case WSAEWOULDBLOCK:
            case WSAECONNRESET:
                return 0;
            case WSAEMSGSIZE:
                return -2;
            }
            return -1;
        }
        if (flags & MSG_PARTIAL)
          return -2;
        recvLength = (int) length;
    }
#else
    {
        struct msghdr msgHdr;
        memset (& msgHdr, 0, sizeof (msgHdr));
        msgHdr.msg_name = & sin6;
        msgHdr.msg_namelen = sizeof (sin6);
        msgHdr.msg_iov = (struct iovec *) buffers;
        msgHdr.msg_iovlen = bufferCount;
        recvLength = recvmsg (socket, & msgHdr, MSG_NOSIGNAL);
        if (recvLength == -1)
        {
            if (errno == EWOULDBLOCK)
              return 0;
            return -1;
        }
        if (msgHdr.msg_flags & MSG_TRUNC)
          return -2;
    }
#endif
    if (sin6.sin6_family != AF_INET6)
      return 0;
    if (address != NULL && from_sockaddr (& sin6, address) != 0)
      return 0; /* the table of addresses is full: a sender we cannot name is not heard */
    return recvLength;
}

/* ---- Hosts ---- */

ENetHost *
enet_host_create_ipv6 (const ENetAddress * address, size_t peerCount, size_t channelLimit,
                       enet_uint32 incomingBandwidth, enet_uint32 outgoingBandwidth)
{
    ENetSocket socket;
    ENetHost * host;
    ENetAddress bound;

    if (address == NULL)
      return NULL;
    socket = enet_socket_create_ipv6 (ENET_SOCKET_TYPE_DATAGRAM);
    if (socket == ENET_SOCKET_NULL)
      return NULL;
    if (enet_socket_bind (socket, address) < 0)
    {
        enet_socket_destroy (socket);
        return NULL;
    }
    /* A host made without an address has an unbound IPv4 socket and nothing else of it: that
       socket is swapped for ours, set up as enet_host_create sets up its own. */
    host = enet_host_create (NULL, peerCount, channelLimit, incomingBandwidth, outgoingBandwidth);
    if (host == NULL)
    {
        enet_socket_destroy (socket);
        return NULL;
    }
    enet_socket_destroy (host -> socket);
    host -> socket = socket;
    enet_socket_set_option (socket, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option (socket, ENET_SOCKOPT_RCVBUF, ENET_HOST_RECEIVE_BUFFER_SIZE);
    enet_socket_set_option (socket, ENET_SOCKOPT_SNDBUF, ENET_HOST_SEND_BUFFER_SIZE);
    if (enet_socket_get_address (socket, & bound) == 0)
      host -> address = bound;
    else
      host -> address = * address;
    return host;
}
