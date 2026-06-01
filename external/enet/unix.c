/** 
 @file  unix.c
 @brief ENet Unix system specific functions
*/
#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>      /* R12: IPv6 socket structs (sockaddr_in6, in6_addr) */
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>       /* R12: inet_pton / inet_ntop for IPv6 literals */
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"

#ifdef __APPLE__
#ifdef HAS_POLL
#undef HAS_POLL
#endif
#ifndef HAS_FCNTL
#define HAS_FCNTL 1
#endif
#ifndef HAS_INET_PTON
#define HAS_INET_PTON 1
#endif
#ifndef HAS_INET_NTOP
#define HAS_INET_NTOP 1
#endif
#ifndef HAS_MSGHDR_FLAGS
#define HAS_MSGHDR_FLAGS 1
#endif
#ifndef HAS_SOCKLEN_T
#define HAS_SOCKLEN_T 1
#endif
#ifndef HAS_GETADDRINFO
#define HAS_GETADDRINFO 1
#endif
#ifndef HAS_GETNAMEINFO
#define HAS_GETNAMEINFO 1
#endif
#endif

#ifdef HAS_FCNTL
#include <fcntl.h>
#endif

#ifdef HAS_POLL
#include <poll.h>
#endif

#if !defined(HAS_SOCKLEN_T) && !defined(__socklen_t_defined)
typedef int socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static enet_uint32 timeBase = 0;

/* ----- R12: dual-stack helpers ---------------------------------------------
 * The patched ENet always opens AF_INET6 sockets with IPV6_V6ONLY=0 so a
 * single socket carries both IPv4 (as v4-mapped ::ffff:a.b.c.d) and IPv6
 * traffic. These two helpers translate between the public ENetAddress
 * (which carries either a u32 v4 host OR a u8[16] v6 host depending on the
 * family field) and the kernel's sockaddr_in6. They keep the per-call site
 * conversion noise to one line at each entry point.
 * -------------------------------------------------------------------------- */

/* Pack an ENetAddress into a sockaddr_in6. v4 addresses are converted to
 * the IPv4-mapped IPv6 form (::ffff:a.b.c.d) so the dual-stack socket
 * delivers them to v4 peers. */
static void
enet_addr_to_sin6 (const ENetAddress * address, struct sockaddr_in6 * sin6)
{
    memset (sin6, 0, sizeof (struct sockaddr_in6));
    sin6 -> sin6_family = AF_INET6;
    sin6 -> sin6_port   = ENET_HOST_TO_NET_16 (address -> port);

    if (address -> family == AF_INET6)
    {
        memcpy (& sin6 -> sin6_addr, address -> host6, 16);
    }
    else if (address -> host == ENET_HOST_ANY)
    {
        /* Wildcard: bind/connect to dual-stack any (::). */
        sin6 -> sin6_addr = in6addr_any;
    }
    else
    {
        /* v4-mapped IPv6: ::ffff:a.b.c.d. The 4 IPv4 octets sit at offset 12
         * of the 16-byte v6 address in network byte order. address->host is
         * already in network byte order. */
        enet_uint8 * out = (enet_uint8 *) & sin6 -> sin6_addr;
        memset (out, 0, 10);
        out [10] = 0xff;
        out [11] = 0xff;
        memcpy (out + 12, & address -> host, 4);
    }
}

/* Unpack a sockaddr_in6 into an ENetAddress. v4-mapped addresses decode to
 * family=AF_INET so legacy receivers (the engine peer table, NAT-style
 * key hashing) keep seeing a u32 v4 host as before R12. Native v6 sets
 * family=AF_INET6 with the full 16-byte host6. */
static void
enet_sin6_to_addr (const struct sockaddr_in6 * sin6, ENetAddress * address)
{
    const enet_uint8 * src = (const enet_uint8 *) & sin6 -> sin6_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin6 -> sin6_port);

    /* IPv4-mapped IPv6 (::ffff:a.b.c.d) — first 10 bytes 0, bytes 10-11 0xff. */
    if (src [0] == 0 && src [1] == 0 && src [2] == 0 && src [3] == 0 &&
        src [4] == 0 && src [5] == 0 && src [6] == 0 && src [7] == 0 &&
        src [8] == 0 && src [9] == 0 && src [10] == 0xff && src [11] == 0xff)
    {
        address -> family = AF_INET;
        memcpy (& address -> host, src + 12, 4);
        memset (address -> host6, 0, 16);  /* keep host6 zeroed for hash determinism */
    }
    else
    {
        address -> family = AF_INET6;
        address -> host   = 0;             /* unused under v6; zero for determinism */
        memcpy (address -> host6, src, 16);
    }
}

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) time (NULL);
}

enet_uint32
enet_time_get (void)
{
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);

    return timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);
    
    timeBase = timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    /* R12: detect IPv6 by the presence of ':' — inet_pton(AF_INET, ...) accepts
     * dotted-quads and decimal forms but rejects v6 literals, and the v6 form
     * is the only one that contains a colon. Route accordingly. */
    if (strchr (name, ':') != NULL)
    {
        if (inet_pton (AF_INET6, name, address -> host6) != 1)
          return -1;
        address -> family = AF_INET6;
        address -> host   = 0;
        return 0;
    }
#ifdef HAS_INET_PTON
    if (! inet_pton (AF_INET, name, & address -> host))
#else
    if (! inet_aton (name, (struct in_addr *) & address -> host))
#endif
        return -1;
    address -> family = AF_INET;
    memset (address -> host6, 0, 16);
    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    /* R12: hostname-or-literal resolver. AF_UNSPEC asks getaddrinfo to return
     * both A (v4) and AAAA (v6) records; we prefer v6 when both are offered
     * since the host's dual-stack socket can reach v4 peers transparently. */
#ifdef HAS_GETADDRINFO
    struct addrinfo hints, * resultList = NULL, * result = NULL;
    struct addrinfo * pickV4 = NULL, * pickV6 = NULL;

    memset (& hints, 0, sizeof (hints));
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo (name, NULL, & hints, & resultList) != 0)
      return -1;

    for (result = resultList; result != NULL; result = result -> ai_next)
    {
        if (result -> ai_family == AF_INET6 && pickV6 == NULL &&
            result -> ai_addr != NULL && result -> ai_addrlen >= sizeof (struct sockaddr_in6))
          pickV6 = result;
        else if (result -> ai_family == AF_INET && pickV4 == NULL &&
                 result -> ai_addr != NULL && result -> ai_addrlen >= sizeof (struct sockaddr_in))
          pickV4 = result;
    }

    if (pickV6 != NULL)
    {
        struct sockaddr_in6 * sin6 = (struct sockaddr_in6 *) pickV6 -> ai_addr;
        memcpy (address -> host6, & sin6 -> sin6_addr, 16);
        address -> family = AF_INET6;
        address -> host   = 0;
        freeaddrinfo (resultList);
        return 0;
    }
    if (pickV4 != NULL)
    {
        struct sockaddr_in * sin = (struct sockaddr_in *) pickV4 -> ai_addr;
        address -> host   = sin -> sin_addr.s_addr;
        address -> family = AF_INET;
        memset (address -> host6, 0, 16);
        freeaddrinfo (resultList);
        return 0;
    }

    if (resultList != NULL)
      freeaddrinfo (resultList);
#else
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYNAME_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__GNU__)
    gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    hostEntry = gethostbyname (name);
#endif

    if (hostEntry != NULL && hostEntry -> h_addrtype == AF_INET)
    {
        address -> host = * (enet_uint32 *) hostEntry -> h_addr_list [0];
        address -> family = AF_INET;
        memset (address -> host6, 0, 16);
        return 0;
    }
#endif

    return enet_address_set_host_ip (address, name);
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    /* R12: format IPv6 with inet_ntop(AF_INET6, ...) when the family flag says so. */
    if (address -> family == AF_INET6)
    {
        if (inet_ntop (AF_INET6, address -> host6, name, nameLength) == NULL)
          return -1;
        return 0;
    }
#ifdef HAS_INET_NTOP
    if (inet_ntop (AF_INET, & address -> host, name, nameLength) == NULL)
#else
    char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
    if (addr != NULL)
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
          return -1;
        memcpy (name, addr, addrLen + 1);
    }
    else
#endif
        return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef HAS_GETNAMEINFO
    /* R12: route reverse-DNS through sockaddr_in6 so v6 addresses resolve too.
     * For v4 we pack into a v4-mapped sockaddr_in6 so the call site stays single. */
    struct sockaddr_in6 sin6;
    int err;

    enet_addr_to_sin6 (address, & sin6);

    err = getnameinfo ((struct sockaddr *) & sin6, sizeof (sin6), name, nameLength, NULL, 0, NI_NAMEREQD);
    if (! err)
    {
        if (name != NULL && nameLength > 0 && ! memchr (name, '\0', nameLength))
          return -1;
        return 0;
    }
    if (err != EAI_NONAME)
      return -1;
#else
    struct in_addr in;
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYADDR_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

    in.s_addr = address -> host;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__GNU__)
    gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    in.s_addr = address -> host;

    hostEntry = gethostbyaddr ((char *) & in, sizeof (struct in_addr), AF_INET);
#endif

    if (hostEntry != NULL)
    {
       size_t hostLen = strlen (hostEntry -> h_name);
       if (hostLen >= nameLength)
         return -1;
       memcpy (name, hostEntry -> h_name, hostLen + 1);
       return 0;
    }
#endif

    return enet_address_get_host_ip (address, name, nameLength);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    /* R12: bind to a dual-stack sockaddr_in6 so the single socket carries
     * both v4 (via v4-mapped) and v6 traffic. ENET_HOST_ANY → ::, matching
     * IPV6_V6ONLY=0 socket option set in enet_socket_create. */
    struct sockaddr_in6 sin6;

    if (address != NULL)
    {
        enet_addr_to_sin6 (address, & sin6);
    }
    else
    {
        memset (& sin6, 0, sizeof (sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_addr   = in6addr_any;
        sin6.sin6_port   = 0;
    }

    return bind (socket,
                 (struct sockaddr *) & sin6,
                 sizeof (struct sockaddr_in6));
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in6 sin6;
    socklen_t sinLength = sizeof (struct sockaddr_in6);

    if (getsockname (socket, (struct sockaddr *) & sin6, & sinLength) == -1)
      return -1;

    enet_sin6_to_addr (& sin6, address);
    return 0;
}

int 
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    /* R12: AF_INET6 + IPV6_V6ONLY=0 → dual-stack socket. v4 peers are surfaced
     * as IPv4-mapped IPv6 addresses by the kernel and decoded back to family=
     * AF_INET in enet_sin6_to_addr, so the engine's existing per-peer hashing
     * (which keys on ENetAddress.host as a u32) keeps working unchanged for
     * v4-only sessions. */
    ENetSocket s = socket (PF_INET6, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s != -1)
    {
        int zero = 0;
        /* Best-effort: if the platform doesn't expose IPV6_V6ONLY (rare on
         * modern Linux/BSD/macOS), the default behavior on Linux/BSD is
         * v6-only anyway and v4 peers won't reach us — log-only failure path
         * isn't worth the noise here since we want this socket created either
         * way and the caller falls back to the v4-mapped path. */
        (void) setsockopt (s, IPPROTO_IPV6, IPV6_V6ONLY, (char *) & zero, sizeof (zero));
    }
    return s;
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = -1;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
#ifdef HAS_FCNTL
            result = fcntl (socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl (socket, F_GETFL) & ~O_NONBLOCK));
#else
            result = ioctl (socket, FIONBIO, & value);
#endif
            break;

        case ENET_SOCKOPT_BROADCAST:
            result = setsockopt (socket, SOL_SOCKET, SO_BROADCAST, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_RCVBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVTIMEO:
        {
            struct timeval timeVal;
            timeVal.tv_sec = value / 1000;
            timeVal.tv_usec = (value % 1000) * 1000;
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & timeVal, sizeof (struct timeval));
            break;
        }

        case ENET_SOCKOPT_SNDTIMEO:
        {
            struct timeval timeVal;
            timeVal.tv_sec = value / 1000;
            timeVal.tv_usec = (value % 1000) * 1000;
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & timeVal, sizeof (struct timeval));
            break;
        }

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_TTL:
            result = setsockopt (socket, IPPROTO_IP, IP_TTL, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = -1;
    socklen_t len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof (int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, value, & len);
            break;

        case ENET_SOCKOPT_TTL:
            len = sizeof (int);
            result = getsockopt (socket, IPPROTO_IP, IP_TTL, (char *) value, & len);
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in6 sin6;
    int result;

    enet_addr_to_sin6 (address, & sin6);

    result = connect (socket, (struct sockaddr *) & sin6, sizeof (struct sockaddr_in6));
    if (result == -1 && errno == EINPROGRESS)
      return 0;

    return result;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    int result;
    struct sockaddr_in6 sin6;
    socklen_t sinLength = sizeof (struct sockaddr_in6);

    result = accept (socket,
                     address != NULL ? (struct sockaddr *) & sin6 : NULL,
                     address != NULL ? & sinLength : NULL);

    if (result == -1)
      return ENET_SOCKET_NULL;

    if (address != NULL)
      enet_sin6_to_addr (& sin6, address);

    return result;
}
    
int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how);
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != -1)
      close (socket);
}

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in6 sin6;
    int sentLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        enet_addr_to_sin6 (address, & sin6);
        msgHdr.msg_name    = & sin6;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in6);
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

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in6 sin6;
    int recvLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        msgHdr.msg_name    = & sin6;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in6);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    recvLength = recvmsg (socket, & msgHdr, MSG_NOSIGNAL);

    if (recvLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

#ifdef HAS_MSGHDR_FLAGS
    if (msgHdr.msg_flags & MSG_TRUNC)
      return -2;
#endif

    if (address != NULL)
      enet_sin6_to_addr (& sin6, address);

    return recvLength;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    return select (maxSocket + 1, readSet, writeSet, NULL, & timeVal);
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
#ifdef HAS_POLL
    struct pollfd pollSocket;
    int pollCount;
    
    pollSocket.fd = socket;
    pollSocket.events = 0;

    if (* condition & ENET_SOCKET_WAIT_SEND)
      pollSocket.events |= POLLOUT;

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      pollSocket.events |= POLLIN;

    pollCount = poll (& pollSocket, 1, timeout);

    if (pollCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (pollCount == 0)
      return 0;

    if (pollSocket.revents & POLLOUT)
      * condition |= ENET_SOCKET_WAIT_SEND;
    
    if (pollSocket.revents & POLLIN)
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#else
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO (& readSet);
    FD_ZERO (& writeSet);

    if (* condition & ENET_SOCKET_WAIT_SEND)
      FD_SET (socket, & writeSet);

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      FD_SET (socket, & readSet);

    selectCount = select (socket + 1, & readSet, & writeSet, NULL, & timeVal);

    if (selectCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }
      
        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#endif
}

#endif

