//
//  File: %dev-net.c
//  Summary: "Device: TCP/IP network access"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Supports TCP and UDP (but not raw socket modes.)
//
//=////////////////////////////////////////////////////////////////////////=//
//

#include <stdlib.h>
#include <string.h>

#include "sys-net.h"

#ifdef IS_ERROR
#undef IS_ERROR //winerror.h defines this, so undef it to avoid the warning
#endif
#include "sys-core.h"

#include "reb-net.h"
#include "reb-evtypes.h"

#if (0)
    #define WATCH1(s,a) printf(s, a)
    #define WATCH2(s,a,b) printf(s, a, b)
    #define WATCH4(s,a,b,c,d) printf(s, a, b, c, d)
#else
    #define WATCH1(s,a)
    #define WATCH2(s,a,b)
    #define WATCH4(s,a,b,c,d)
#endif

void Signal_Device(REBREQ *req, REBINT type);
DEVICE_CMD Listen_Socket(REBREQ *sock);

#ifdef TO_WINDOWS
    extern HWND Event_Handle; // For WSAAsync API
#endif

// Prevent sendmsg/write raising SIGPIPE the TCP socket is closed:
// https://stackoverflow.com/questions/108183/how-to-prevent-sigpipes-or-handle-them-properly
// Linux does not support SO_NOSIGPIPE
//
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/***********************************************************************
**
**  Local Functions
**
***********************************************************************/

static void Set_Addr(struct sockaddr_in *sa, long ip, int port)
{
    // Set the IP address and port number in a socket_addr struct.
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = ip;  //htonl(ip); NOTE: REBOL stays in network byte order
    sa->sin_port = htons((unsigned short)port);
}

static void Get_Local_IP(struct devreq_net *sock)
{
    // Get the local IP address and port number.
    // This code should be fast and never fail.
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);

    getsockname(AS_REBREQ(sock)->requestee.socket, cast(struct sockaddr *, &sa), &len);
    sock->local_ip = sa.sin_addr.s_addr; //htonl(ip); NOTE: REBOL stays in network byte order
    sock->local_port = ntohs(sa.sin_port);
}

static REBOOL Set_Sock_Options(SOCKET sock)
{
    // Prevent sendmsg/write raising SIGPIPE the TCP socket is closed:
    // https://stackoverflow.com/questions/108183/how-to-prevent-sigpipes-or-handle-them-properly
#if defined(SO_NOSIGPIPE)
    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0) {
        return FALSE;
    }
#endif

    // Set non-blocking mode. Return TRUE if no error.
#ifdef FIONBIO
    unsigned long mode = 1;
    return NOT(IOCTL(sock, FIONBIO, &mode));
#else
    int flags;
    flags = fcntl(sock, F_GETFL, 0);
    flags |= O_NONBLOCK;
    //else flags &= ~O_NONBLOCK;
    return LOGICAL(fcntl(sock, F_SETFL, flags) >= 0);
#endif
}


//
//  Init_Net: C
//
// Intialize networking libraries and related interfaces.
// This function will be called prior to any socket functions.
//
DEVICE_CMD Init_Net(REBREQ *dr)
{
    REBDEV *dev = (REBDEV*)dr; // just to keep compiler happy
#ifdef TO_WINDOWS
    WSADATA wsaData;
    // Initialize Windows Socket API with given VERSION.
    // It is ok to call twice, as long as WSACleanup twice.
    if (WSAStartup(0x0101, &wsaData)) return DR_ERROR;
#endif
    dev->flags |= RDF_INIT;
    return DR_DONE;
}


//
//  Quit_Net: C
//
// Close and cleanup networking libraries and related interfaces.
//
DEVICE_CMD Quit_Net(REBREQ *dr)
{
    REBDEV *dev = (REBDEV*)dr; // just to keep compiler happy
#ifdef TO_WINDOWS
    if (dev->flags & RDF_INIT)
        WSACleanup();
#endif
    dev->flags &= ~RDF_INIT;
    return DR_DONE;
}



//
//  Open_Socket: C
//
// Setup a socket with the specified protocol and bind it to
// the related transport service.
//
// Returns 0 on success.
// On failure, error code is OS local.
//
// Note: This is an intialization procedure and no actual
// connection is made at this time. The IP address and port
// number are not needed, only the type of service required.
//
// After usage:
//     Close_Socket() - to free OS allocations
//
DEVICE_CMD Open_Socket(REBREQ *sock)
{
    int type;
    int protocol;
    long result;

    sock->error = 0;
    sock->state = 0;  // clear all flags

    // Setup for correct type and protocol:
    if (sock->modes & RST_UDP) {
        type = SOCK_DGRAM;
        protocol = IPPROTO_UDP;
    }
    else {  // TCP is default
        type = SOCK_STREAM;
        protocol = IPPROTO_TCP;
    }

    // Bind to the transport service, return socket handle or error:
    result = (int)socket(AF_INET, type, protocol);

    // Failed, get error code (os local):
    if (result == BAD_SOCKET) {
        sock->error = GET_ERROR;
        return DR_ERROR;
    }

    sock->requestee.socket = result;
    sock->state |= RSM_OPEN;

    // Set socket to non-blocking async mode:
    if (!Set_Sock_Options(sock->requestee.socket)) {
        sock->error = GET_ERROR;
        return DR_ERROR;
    }

    return DR_DONE;
}


//
//  Close_Socket: C
//
// Close a socket.
//
// Returns 0 on success.
// On failure, error code is OS local.
//
DEVICE_CMD Close_Socket(REBREQ *req)
{
    struct devreq_net *sock = DEVREQ_NET(req);
    req->error = 0;

    if (req->state & RSM_OPEN) {

        req->state = 0;  // clear: RSM_OPEN, RSM_CONNECT

        // If DNS pending, abort it:
        if (sock->host_info) {  // indicates DNS phase active
            OS_FREE(sock->host_info);
            req->requestee.socket = req->length; // Restore TCP socket (see Lookup)
        }

        if (CLOSE_SOCKET(req->requestee.socket)) {
            req->error = GET_ERROR;
            return DR_ERROR;
        }
    }

    return DR_DONE;
}


//
//  Lookup_Socket: C
//
// Initiate the GetHost request and return immediately.
// This is very similar to the DNS device.
// The request will pend until the main event handler gets WM_DNS.
// Note the temporary results buffer (must be freed later).
// Note we use the sock->requestee.handle for the DNS handle. During use,
// we store the TCP socket in the length field.
//
DEVICE_CMD Lookup_Socket(REBREQ *req)
{
    struct devreq_net *sock = DEVREQ_NET(req);
    sock->host_info = NULL; // no allocated data

    // !!! R3-Alpha would use asynchronous DNS API on Windows, but that API
    // was not supported by IPv6, and developers are encouraged to use normal
    // socket APIs with their own threads.

    HOSTENT *host = gethostbyname(s_cast(req->common.data));
    if (host != NULL) {
        memcpy(&sock->remote_ip, *host->h_addr_list, 4); //he->h_length);
        req->flags &= ~RRF_DONE;
        Signal_Device(req, EVT_LOOKUP);
        return DR_DONE;
    }

    req->error = GET_ERROR;
    //Signal_Device(req, EVT_ERROR);
    return DR_ERROR; // Remove it from pending list
}


//
//  Connect_Socket: C
//
// Connect a socket to a service.
// Only required for connection-based protocols (e.g. not UDP).
// The IP address must already be resolved before calling.
//
// This function is asynchronous. It will return immediately.
// You can call this function again to check the pending connection.
//
// The function will return:
//     =0: connection succeeded (or already is connected)
//     >0: in-progress, still trying
//     <0: error occurred, no longer trying
//
// Before usage:
//     Open_Socket() -- to allocate the socket
//
DEVICE_CMD Connect_Socket(REBREQ *req)
{
    int result;
    struct sockaddr_in sa;
    struct devreq_net *sock = DEVREQ_NET(req);

    if (req->modes & RST_LISTEN)
        return Listen_Socket(req);

    if (req->state & RSM_CONNECT)
        return DR_DONE; // already connected

    if (req->modes & RST_UDP) {
        req->state &= ~RSM_ATTEMPT;
        req->state |= RSM_CONNECT;
        Get_Local_IP(sock);
        Signal_Device(req, EVT_CONNECT);
        return DR_DONE; // done
    }

    Set_Addr(&sa, sock->remote_ip, sock->remote_port);
    result = connect(
        req->requestee.socket, cast(struct sockaddr *, &sa), sizeof(sa)
    );

    if (result != 0) result = GET_ERROR;

    WATCH2("connect() error: %d - %s\n", result, strerror(result));

    switch (result) {

    case 0: // no error
    case NE_ISCONN:
        // Connected, set state:
        req->state &= ~RSM_ATTEMPT;
        req->state |= RSM_CONNECT;
        Get_Local_IP(sock);
        Signal_Device(req, EVT_CONNECT);
        return DR_DONE; // done

#ifdef TO_WINDOWS
    case NE_INVALID:    // Corrects for Microsoft bug
#endif
    case NE_WOULDBLOCK:
    case NE_INPROGRESS:
    case NE_ALREADY:
        // Still trying:
        req->state |= RSM_ATTEMPT;
        return DR_PEND;

    default:
        // An error happened:
        req->state &= ~RSM_ATTEMPT;
        req->error = result;
        //Signal_Device(req, EVT_ERROR);
        return DR_ERROR;
    }
}


//
//  Transfer_Socket: C
//
// Write or read a socket (for connection-based protocols).
//
// This function is asynchronous. It will return immediately.
// You can call this function again to check the pending connection.
//
// The mode is RSM_RECEIVE or RSM_SEND.
//
// The function will return:
//     =0: succeeded
//     >0: in-progress, still trying
//     <0: error occurred, no longer trying
//
// Before usage:
//     Open_Socket()
//     Connect_Socket()
//     Verify that RSM_CONNECT is true
//     Setup the sock->common.data and sock->length
//
// Note that the mode flag is cleared by the caller, not here.
//
DEVICE_CMD Transfer_Socket(REBREQ *req)
{
    int result;
    long len;
    struct sockaddr_in remote_addr;
    socklen_t addr_len = sizeof(remote_addr);
    struct devreq_net *sock = DEVREQ_NET(req);
    int mode = (req->command == RDC_READ ? RSM_RECEIVE : RSM_SEND);

    if (NOT(req->state & RSM_CONNECT) && NOT(req->modes & RST_UDP)) {
        req->error = -18;
        return DR_ERROR;
    }

    req->state |= mode;

    // Limit size of transfer:
    len = MIN(req->length - req->actual, MAX_TRANSFER);

    if (mode == RSM_SEND) {
        // If host is no longer connected:
        Set_Addr(&remote_addr, sock->remote_ip, sock->remote_port);
        result = sendto(
            req->requestee.socket,
            s_cast(req->common.data), len,
            MSG_NOSIGNAL, // Flags
            cast(struct sockaddr*, &remote_addr), addr_len
        );
        WATCH2("send() len: %d actual: %d\n", len, result);

        if (result >= 0) {
            req->common.data += result;
            req->actual += result;
            if (req->actual >= req->length) {
                Signal_Device(req, EVT_WROTE);
                return DR_DONE;
            }
            req->flags |= RRF_ACTIVE; // notify OS_WAIT of activity
            return DR_PEND;
        }
        // if (result < 0) ...
    }
    else {
        result = recvfrom(
            req->requestee.socket,
            s_cast(req->common.data), len,
            0, // Flags
            cast(struct sockaddr*, &remote_addr), &addr_len
        );
        WATCH2("recv() len: %d result: %d\n", len, result);

        if (result > 0) {
            if (req->modes & RST_UDP) {
                sock->remote_ip = remote_addr.sin_addr.s_addr;
                sock->remote_port = ntohs(remote_addr.sin_port);
            }
            req->actual = result;
            Signal_Device(req, EVT_READ);
            return DR_DONE;
        }
        if (result == 0) {      // The socket gracefully closed.
            req->actual = 0;
            req->state &= ~RSM_CONNECT; // But, keep RRF_OPEN true
            Signal_Device(req, EVT_CLOSE);
            return DR_DONE;
        }
        // if (result < 0) ...
    }

    // Check error code:
    result = GET_ERROR;
    WATCH2("get error: %d %s\n", result, strerror(result));
    if (result == NE_WOULDBLOCK) return DR_PEND; // still waiting

    WATCH4("ERROR: recv(%d %x) len: %d error: %d\n", req->requestee.socket, req->common.data, len, result);
    // A nasty error happened:
    req->error = result;
    //Signal_Device(req, EVT_ERROR);
    return DR_ERROR;
}


//
//  Listen_Socket: C
//
// Setup a server (listening) socket (TCP or UDP).
//
// Before usage:
//     Open_Socket();
//     Set local_port to desired port number.
//
// Use this instead of Connect_Socket().
//
DEVICE_CMD Listen_Socket(REBREQ *req)
{
    int result;
    int len = 1;
    struct sockaddr_in sa;
    struct devreq_net *sock = DEVREQ_NET(req);

    // make sure ACCEPT queue is empty
    // initialized in p-net.c
    assert(req->common.sock == NULL);

    // Setup socket address range and port:
    Set_Addr(&sa, INADDR_ANY, sock->local_port);

    // Allow listen socket reuse:
    result = setsockopt(
        req->requestee.socket, SOL_SOCKET, SO_REUSEADDR,
        cast(char*, &len), sizeof(len)
    );

    if (result) {
lserr:
        req->error = GET_ERROR;
        return DR_ERROR;
    }

    // Bind the socket to our local address:
    result = bind(
        req->requestee.socket, cast(struct sockaddr *, &sa), sizeof(sa)
    );
    if (result)
        goto lserr;

    req->state |= RSM_BIND;

    // For TCP connections, setup listen queue:
    if (NOT(req->modes & RST_UDP)) {
        result = listen(req->requestee.socket, SOMAXCONN);
        if (result)
            goto lserr;
        req->state |= RSM_LISTEN;
    }

    Get_Local_IP(sock);
    req->command = RDC_CREATE; // the command done on wakeup

    return DR_PEND;
}



//
//  Modify_Socket: C
//
// !!! R3-Alpha had no RDC_MODIFY commands.  Some way was needed to get
// multicast setting through to the platform-specific port code, and this
// method was chosen.  Eventually, the ports *themselves* should be extension
// modules instead of in core, and then there won't be concern about the
// mixture of port dispatch code with platform code.
//
DEVICE_CMD Modify_Socket(REBREQ *sock)
{
    assert(sock->command == RDC_MODIFY);

    REBFRM *frame_ = cast(REBFRM*, sock->common.data);
    int result = 0;

    switch (sock->flags) {
    case 3171: {
        INCLUDE_PARAMS_OF_SET_UDP_MULTICAST;

        UNUSED(ARG(port)); // implicit from sock, which caller extracted

        if (NOT(sock->modes & RST_UDP)) { // !!! other checks?
            sock->error = -18;
            return DR_ERROR;
        }

        struct ip_mreq mreq;
        memcpy(&mreq.imr_multiaddr.s_addr, VAL_TUPLE(ARG(group)), 4);
        memcpy(&mreq.imr_interface.s_addr, VAL_TUPLE(ARG(member)), 4);

        result = setsockopt(
            sock->requestee.socket,
            IPPROTO_IP,
            REF(drop) ? IP_DROP_MEMBERSHIP : IP_ADD_MEMBERSHIP,
            cast(char*, &mreq),
            sizeof(mreq)
        );

        break; }

    case 2365: {
        INCLUDE_PARAMS_OF_SET_UDP_TTL;

        UNUSED(ARG(port)); // implicit from sock, which caller extracted

        if (NOT(sock->modes & RST_UDP)) { // !!! other checks?
            sock->error = -18;
            return DR_ERROR;
        }

        int ttl = VAL_INT32(ARG(ttl));
        result = setsockopt(
            sock->requestee.socket,
            IPPROTO_IP,
            IP_TTL,
            cast(char*, &ttl),
            sizeof(ttl)
        );

        break; }

    default:
        // not return DR_ERROR?  Is failing here ok?
        //
        fail ("Unknown socket MODIFY operation");
    }

    if (result < 0) {
        sock->error = result;
        return DR_ERROR;
    }

    return DR_DONE;
}

extern void Attach_Request(REBREQ **prior, REBREQ *req);

//
//  Accept_Socket: C
//
// Accept an inbound connection on a TCP listen socket.
//
// The function will return:
//     =0: succeeded
//     >0: in-progress, still trying
//     <0: error occurred, no longer trying
//
// Before usage:
//     Open_Socket();
//     Set local_port to desired port number.
//     Listen_Socket();
//
DEVICE_CMD Accept_Socket(REBREQ *req)
{
    struct sockaddr_in sa;
    struct devreq_net *news;
    socklen_t len = sizeof(sa);
    int result;
    struct devreq_net *sock = DEVREQ_NET(req);

    // Accept a new socket, if there is one:
    result = accept(req->requestee.socket, cast(struct sockaddr *, &sa), &len);

    if (result == BAD_SOCKET) {
        result = GET_ERROR;
        if (result == NE_WOULDBLOCK) return DR_PEND;
        req->error = result;
        //Signal_Device(sock, EVT_ERROR);
        return DR_ERROR;
    }

    if (!Set_Sock_Options(result)) {
        req->error = GET_ERROR;
        //Signal_Device(sock, EVT_ERROR);
        return DR_ERROR;
    }

    // To report the new socket, the code here creates a temporary
    // request and copies the listen request to it. Then, it stores
    // the new values for IP and ports and links this request to the
    // original via the sock->common.data.
    news = OS_ALLOC_ZEROFILL(struct devreq_net);
//  *news = *sock;
    news->devreq.device = req->device;

    cast(REBREQ*, news)->flags |= RRF_OPEN;
    news->devreq.state |= (RSM_OPEN | RSM_CONNECT);

    news->devreq.requestee.socket = result;
    news->remote_ip   = sa.sin_addr.s_addr; //htonl(ip); NOTE: REBOL stays in network byte order
    news->remote_port = ntohs(sa.sin_port);
    Get_Local_IP(news);

    // There could be mulitple connections to be accepted.
    // Queue them at common.sock
    Attach_Request(cast(REBREQ**, &AS_REBREQ(sock)->common.sock), AS_REBREQ(news));

    Signal_Device(req, EVT_ACCEPT);

    // Even though we signalled, we keep the listen pending to
    // accept additional connections.
    return DR_PEND;
}


/***********************************************************************
**
**  Command Dispatch Table (RDC_ enum order)
**
***********************************************************************/

static DEVICE_CMD_FUNC Dev_Cmds[RDC_MAX] = {
    Init_Net,
    Quit_Net,
    Open_Socket,
    Close_Socket,
    Transfer_Socket,        // Read
    Transfer_Socket,        // Write
    0,  // poll
    Connect_Socket,
    0,  // query
    Modify_Socket,          // modify
    Accept_Socket,          // Create
    0,  // delete
    0,  // rename
    Lookup_Socket
};

DEFINE_DEV(
    Dev_Net,
    "TCP/IP Network", 1, Dev_Cmds, RDC_MAX, sizeof(struct devreq_net)
);
