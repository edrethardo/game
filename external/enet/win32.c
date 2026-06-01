/** 
 @file  win32.c
 @brief ENet Win32 system specific functions
*/
#ifdef _WIN32

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"
#include <windows.h>
#include <mmsystem.h>
#include <ws2tcpip.h>

static enet_uint32 timeBase = 0;

int
enet_initialize (void)
{
    /* R12: bump from Winsock 1.1 to 2.2 — required for AF_INET6 sockets, inet_pton,
     * getaddrinfo and the IPV6_V6ONLY socket option. 2.2 has shipped with every
     * Windows since 98SE, so this isn't a meaningful compatibility regression. */
    WORD versionRequested = MAKEWORD (2, 2);
    WSADATA wsaData;

    if (WSAStartup (versionRequested, & wsaData))
       return -1;

    if (LOBYTE (wsaData.wVersion) != 2 ||
        HIBYTE (wsaData.wVersion) != 2)
    {
       WSACleanup ();

       return -1;
    }

    timeBeginPeriod (1);

    return 0;
}

/* ----- R12: dual-stack helpers (mirror unix.c) ----------------------------
 * AF_INET6 socket with IPV6_V6ONLY=0 carries v4 traffic too via IPv4-mapped
 * IPv6 (::ffff:a.b.c.d). These helpers translate between ENetAddress and the
 * kernel's sockaddr_in6, and decode v4-mapped back to family=AF_INET so peer
 * table hashing on the engine side keeps using u32 hosts for v4 sessions.
 * -------------------------------------------------------------------------- */

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
        sin6 -> sin6_addr = in6addr_any;
    }
    else
    {
        enet_uint8 * out = (enet_uint8 *) & sin6 -> sin6_addr;
        memset (out, 0, 10);
        out [10] = 0xff;
        out [11] = 0xff;
        memcpy (out + 12, & address -> host, 4);
    }
}

static void
enet_sin6_to_addr (const struct sockaddr_in6 * sin6, ENetAddress * address)
{
    const enet_uint8 * src = (const enet_uint8 *) & sin6 -> sin6_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin6 -> sin6_port);

    if (src [0] == 0 && src [1] == 0 && src [2] == 0 && src [3] == 0 &&
        src [4] == 0 && src [5] == 0 && src [6] == 0 && src [7] == 0 &&
        src [8] == 0 && src [9] == 0 && src [10] == 0xff && src [11] == 0xff)
    {
        address -> family = AF_INET;
        memcpy (& address -> host, src + 12, 4);
        memset (address -> host6, 0, 16);
    }
    else
    {
        address -> family = AF_INET6;
        address -> host   = 0;
        memcpy (address -> host6, src, 16);
    }
}

void
enet_deinitialize (void)
{
    timeEndPeriod (1);

    WSACleanup ();
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) timeGetTime ();
}

enet_uint32
enet_time_get (void)
{
    return (enet_uint32) timeGetTime () - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    timeBase = (enet_uint32) timeGetTime () - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    /* R12: route IPv6 literals (any string containing ':') through inet_pton.
     * For v4 literals we keep the hand-rolled parser the original Win32 build
     * used — it produces network-byte-order output the rest of the file expects
     * and avoids depending on Winsock's inet_pton on older Windows. */
    if (strchr (name, ':') != NULL)
    {
        if (inet_pton (AF_INET6, name, address -> host6) != 1)
          return -1;
        address -> family = AF_INET6;
        address -> host   = 0;
        return 0;
    }
    {
        enet_uint8 vals [4] = { 0, 0, 0, 0 };
        int i;

        for (i = 0; i < 4; ++ i)
        {
            const char * next = name + 1;
            if (* name != '0')
            {
                long val = strtol (name, (char **) & next, 10);
                if (val < 0 || val > 255 || next == name || next - name > 3)
                  return -1;
                vals [i] = (enet_uint8) val;
            }

            if (* next != (i < 3 ? '.' : '\0'))
              return -1;
            name = next + 1;
        }

        memcpy (& address -> host, vals, sizeof (enet_uint32));
        address -> family = AF_INET;
        memset (address -> host6, 0, 16);
    }
    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    /* R12: getaddrinfo with AF_UNSPEC returns both A (v4) and AAAA (v6) records;
     * we prefer v6 when both are offered since the dual-stack socket handles
     * v4 peers transparently anyway. Falls through to the literal parser on
     * failure. */
    struct addrinfo hints, * resultList = NULL, * result = NULL;
    struct addrinfo * pickV4 = NULL, * pickV6 = NULL;

    memset (& hints, 0, sizeof (hints));
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo (name, NULL, & hints, & resultList) == 0)
    {
        for (result = resultList; result != NULL; result = result -> ai_next)
        {
            if (result -> ai_family == AF_INET6 && pickV6 == NULL &&
                result -> ai_addr != NULL && result -> ai_addrlen >= (int) sizeof (struct sockaddr_in6))
              pickV6 = result;
            else if (result -> ai_family == AF_INET && pickV4 == NULL &&
                     result -> ai_addr != NULL && result -> ai_addrlen >= (int) sizeof (struct sockaddr_in))
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
        freeaddrinfo (resultList);
    }

    return enet_address_set_host_ip (address, name);
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    /* R12: format v6 hosts via inet_ntop(AF_INET6, ...); fall through to the
     * original inet_ntoa path for v4 so non-Vista+ targets still work. */
    if (address -> family == AF_INET6)
    {
        if (inet_ntop (AF_INET6, (void *) address -> host6, name, nameLength) == NULL)
          return -1;
        return 0;
    }
    {
        char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
        if (addr == NULL)
            return -1;
        else
        {
            size_t addrLen = strlen(addr);
            if (addrLen >= nameLength)
              return -1;
            memcpy (name, addr, addrLen + 1);
        }
    }
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    /* R12: route reverse-DNS through getnameinfo on a sockaddr_in6 so both v4
     * (v4-mapped) and v6 names resolve. The original gethostbyaddr-only path
     * couldn't reach AAAA records. */
    struct sockaddr_in6 sin6;
    int err;

    enet_addr_to_sin6 (address, & sin6);

    err = getnameinfo ((struct sockaddr *) & sin6, sizeof (sin6), name, (DWORD) nameLength, NULL, 0, NI_NAMEREQD);
    if (err == 0)
      return 0;

    return enet_address_get_host_ip (address, name, nameLength);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
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
                 sizeof (struct sockaddr_in6)) == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in6 sin6;
    int sinLength = sizeof (struct sockaddr_in6);

    if (getsockname (socket, (struct sockaddr *) & sin6, & sinLength) == -1)
      return -1;

    enet_sin6_to_addr (& sin6, address);
    return 0;
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog) == SOCKET_ERROR ? -1 : 0;
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    /* R12: AF_INET6 + IPV6_V6ONLY=0 — dual-stack socket (mirror unix.c). v4
     * peers reach this socket as v4-mapped IPv6 and are decoded back to
     * family=AF_INET in enet_sin6_to_addr. */
    ENetSocket s = socket (PF_INET6, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s != INVALID_SOCKET)
    {
        DWORD zero = 0;
        (void) setsockopt (s, IPPROTO_IPV6, IPV6_V6ONLY, (char *) & zero, sizeof (zero));
    }
    return s;
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = SOCKET_ERROR;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
        {
            u_long nonBlocking = (u_long) value;
            result = ioctlsocket (socket, FIONBIO, & nonBlocking);
            break;
        }

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
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDTIMEO:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_TTL:
            result = setsockopt (socket, IPPROTO_IP, IP_TTL, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = SOCKET_ERROR, len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof(int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, (char *) value, & len);
            break;

        case ENET_SOCKOPT_TTL:
            len = sizeof(int);
            result = getsockopt (socket, IPPROTO_IP, IP_TTL, (char *) value, & len);
            break;

        default:
            break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in6 sin6;
    int result;

    enet_addr_to_sin6 (address, & sin6);

    result = connect (socket, (struct sockaddr *) & sin6, sizeof (struct sockaddr_in6));
    if (result == SOCKET_ERROR && WSAGetLastError () != WSAEWOULDBLOCK)
      return -1;

    return 0;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    SOCKET result;
    struct sockaddr_in6 sin6;
    int sinLength = sizeof (struct sockaddr_in6);

    result = accept (socket,
                     address != NULL ? (struct sockaddr *) & sin6 : NULL,
                     address != NULL ? & sinLength : NULL);

    if (result == INVALID_SOCKET)
      return ENET_SOCKET_NULL;

    if (address != NULL)
      enet_sin6_to_addr (& sin6, address);

    return result;
}

int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how) == SOCKET_ERROR ? -1 : 0;
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != INVALID_SOCKET)
      closesocket (socket);
}

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct sockaddr_in6 sin6;
    DWORD sentLength = 0;

    if (address != NULL)
      enet_addr_to_sin6 (address, & sin6);

    if (WSASendTo (socket,
                   (LPWSABUF) buffers,
                   (DWORD) bufferCount,
                   & sentLength,
                   0,
                   address != NULL ? (struct sockaddr *) & sin6 : NULL,
                   address != NULL ? sizeof (struct sockaddr_in6) : 0,
                   NULL,
                   NULL) == SOCKET_ERROR)
    {
       if (WSAGetLastError () == WSAEWOULDBLOCK)
         return 0;

       return -1;
    }

    return (int) sentLength;
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    INT sinLength = sizeof (struct sockaddr_in6);
    DWORD flags = 0,
          recvLength = 0;
    struct sockaddr_in6 sin6;

    if (WSARecvFrom (socket,
                     (LPWSABUF) buffers,
                     (DWORD) bufferCount,
                     & recvLength,
                     & flags,
                     address != NULL ? (struct sockaddr *) & sin6 : NULL,
                     address != NULL ? & sinLength : NULL,
                     NULL,
                     NULL) == SOCKET_ERROR)
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

    if (address != NULL)
      enet_sin6_to_addr (& sin6, address);

    return (int) recvLength;
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
      return -1;

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;
    
    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
} 

#endif

