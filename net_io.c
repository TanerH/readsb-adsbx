// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// net_io.c: network handling.
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// This file incorporates work covered by the following copyright and
// license:
//
// Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//  *  Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//  *  Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "readsb.h"

/* for PRIX64 */
#include <inttypes.h>

#include <assert.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//
// ============================= Networking =============================
//
// Note: here we disregard any kind of good coding practice in favor of
// extreme simplicity, that is:
//
// 1) We only rely on the kernel buffers for our I/O without any kind of
//    user space buffering.
// 2) We don't register any kind of event handler, from time to time a
//    function gets called and we accept new connections. All the rest is
//    handled via non-blocking I/O and manually polling clients to see if
//    they have something new to share with us when reading is needed.

static int handleBeastCommand(struct client *c, char *p, int remote);
static int decodeBinMessage(struct client *c, char *p, int remote);
static int decodeHexMessage(struct client *c, char *hex, int remote);

static void send_raw_heartbeat(struct net_service *service);
static void send_beast_heartbeat(struct net_service *service);
static void send_sbs_heartbeat(struct net_service *service);

static void writeFATSVEvent(struct modesMessage *mm, struct aircraft *a);
static void writeFATSVPositionUpdate(float lat, float lon, float alt);

static void autoset_modeac();

static timer_t reconnect_timer;

//
//=========================================================================
//
// Networking "stack" initialization
//

// Init a service with the given read/write characteristics, return the new service.
// Doesn't arrange for the service to listen or connect
struct net_service *serviceInit(const char *descr, struct net_writer *writer, heartbeat_fn hb, read_mode_t mode, const char *sep, read_fn handler) {
    struct net_service *service;

    if (!(service = calloc(sizeof (*service), 1))) {
        fprintf(stderr, "Out of memory allocating service %s\n", descr);
        exit(1);
    }

    service->next = Modes.services;
    Modes.services = service;

    service->descr = descr;
    service->listener_count = 0;
    service->pusher_count = 0;
    service->connections = 0;
    service->writer = writer;
    service->read_sep = sep;
    service->read_mode = mode;
    service->read_handler = handler;

    if (service->writer) {
        if (!service->writer->data) {
            if (!(service->writer->data = malloc(MODES_OUT_BUF_SIZE))) {
                fprintf(stderr, "Out of memory allocating output buffer for service %s\n", descr);
                exit(1);
            }
        }

        service->writer->service = service;
        service->writer->dataUsed = 0;
        service->writer->lastWrite = mstime();
        service->writer->send_heartbeat = hb;
    }

    return service;
}

// Create a client attached to the given service using the provided socket FD
struct client *createSocketClient(struct net_service *service, int fd) {
    anetSetSendBuffer(Modes.aneterr, fd, (MODES_NET_SNDBUF_SIZE << Modes.net_sndbuf_size));
    return createGenericClient(service, fd);
}

// Create a client attached to the given service using the provided FD (might not be a socket!)

struct client *createGenericClient(struct net_service *service, int fd) {
    struct client *c;

    anetNonBlock(Modes.aneterr, fd);

    if (!(c = (struct client *) malloc(sizeof (*c)))) {
        fprintf(stderr, "Out of memory allocating a new %s network client\n", service->descr);
        exit(1);
    }

    c->service = service;
    c->next = Modes.clients;
    c->fd = fd;
    c->buflen = 0;
    c->modeac_requested = 0;
    c->last_receive = mstime();
    c->last_flush = mstime();
    c->last_send = mstime();
    c->sendq_len = 0;
    c->sendq_max = 0;
    c->sendq = NULL;

    // Sockaddr info is filled in later (hopefully) - so zero it out for now
    memset(&c->ss, 0, sizeof(c->ss));

    if (service->writer) {
        if (!(c->sendq = malloc(MODES_NET_SNDBUF_SIZE << Modes.net_sndbuf_size))) {
            fprintf(stderr, "Out of memory allocating client SendQ\n");
            exit(1);
        }
        // Have to keep track of this manually
        c->sendq_max = MODES_NET_SNDBUF_SIZE << Modes.net_sndbuf_size;
    }
    Modes.clients = c;

    ++service->connections;
    if (service->writer && service->connections == 1) {
        service->writer->lastWrite = mstime(); // suppress heartbeat initially
    }

    return c;
}

// Make sure output is at least 22 chars for xxx.xxx.xxx.xxx:nnnnn + null
int get_host_port(struct client *c, char *output, int size) {
    if (size < 22) {
        return 0;
    }
    if (c) {
        char ip[20];
        uint16_t port;

        if (!inet_ntop(AF_INET, &(((struct sockaddr_in *)&c->ss)->sin_addr), ip, sizeof(ip))) {
            fprintf(stderr, "inet_ntop failed: %d (%s)\n", errno, strerror(errno));
        }
        port = ntohs( ((struct sockaddr_in *)&c->ss)->sin_port);

        snprintf(output, size, "%s:%d", ip, port);
        return 1;
    } else {
        snprintf(output, 6, "error");
        return 0;
    }
}

// Timer callback checking periodically whether the push service lost its server
// connection and requires a re-connect.
static void serviceReconnectCallback(int sig) {
    MODES_NOTUSED(sig);
    struct net_service *s;

    for (s = Modes.services; s; s = s->next) {
        /* Try reconnecting to push server on connection loss */
        if ((s->pusher_count > 0) && (s->connections == 0)) {
            fprintf(stderr, "Push service re-connect.\n");
            serviceConnect(s, Modes.net_push_server_address, Modes.net_push_server_port);
        }
    }
}

// Initiate an outgoing connection which will use the given service.
// Return the new client or NULL if the connection failed
struct client *serviceConnect(struct net_service *service, char *push_addr, char *push_port) {
    if (!push_port || !strcmp(push_port, "") || !strcmp(push_port, "0"))
        return NULL;

    if (!push_addr || !strcmp(push_addr, ""))
        return NULL;

    struct sockaddr_storage ss;
    int s;
    s = anetTcpConnect(Modes.aneterr, push_addr, push_port, &ss);

    /* Setup timer to check whether push service requires a re-connection to server. */
    if((Modes.net_push_delay <= 0) || (Modes.net_push_delay > 86400)) {
        Modes.net_push_delay = 30;
    }

    // If this is already set, we already have a timer, so only create timer if it's 0
    if (service->pusher_count == 0) {
        (void) signal(SIGALRM, serviceReconnectCallback);
        struct itimerspec time;
        time.it_value.tv_sec = Modes.net_push_delay;
        time.it_value.tv_nsec = 0;
        time.it_interval.tv_sec = Modes.net_push_delay;
        time.it_interval.tv_nsec = 0;
        timer_create (CLOCK_REALTIME, NULL, &reconnect_timer);
        timer_settime (reconnect_timer, 0, &time, NULL);
    }

    service->pusher_count = 1;

    if (s == ANET_ERR) {
        fprintf(stderr, "Service connection to %s:%s failed: %s\n", push_addr, push_port, Modes.aneterr);
        return NULL;
    }


    // If we're able to create this "client", save the sockaddr info and print a msg
    struct client *c;
    c = createSocketClient(service, s);
    if (c) {
        // We got a client, copy 'ss' into it
        memcpy(&c->ss, &ss, sizeof(ss));
        if (Modes.debug & MODES_DEBUG_NET) {
            char h_p[31];
            get_host_port(c, h_p, 30);
            fprintf(stderr, "Service connection to %s (fd %d) established\n", h_p, s);
        }
    } else {
        fprintf(stderr, "Service connection to %s:%s failed\n", push_addr, push_port);
    }

    return c;
}

// Set up the given service to listen on an address/port.
// _exits_ on failure!
void serviceListen(struct net_service *service, char *bind_addr, char *bind_ports) {
    int *fds = NULL;
    int n = 0;
    char *p, *end;
    char buf[128];

    if (service->listener_count > 0) {
        fprintf(stderr, "Tried to set up the service %s twice!\n", service->descr);
        exit(1);
    }

    if (!bind_ports || !strcmp(bind_ports, "") || !strcmp(bind_ports, "0"))
        return;

    p = bind_ports;
    while (p && *p) {
        int newfds[16];
        int nfds, i;

        end = strpbrk(p, ", ");
        if (!end) {
            strncpy(buf, p, sizeof (buf));
            buf[sizeof (buf) - 1] = 0;
            p = NULL;
        } else {
            size_t len = end - p;
            if (len >= sizeof (buf))
                len = sizeof (buf) - 1;
            memcpy(buf, p, len);
            buf[len] = 0;
            p = end + 1;
        }

        nfds = anetTcpServer(Modes.aneterr, buf, bind_addr, newfds, sizeof (newfds));
        if (nfds == ANET_ERR) {
            fprintf(stderr, "Error opening the listening port %s (%s): %s\n",
                    buf, service->descr, Modes.aneterr);
            exit(1);
        }

        fds = realloc(fds, (n + nfds) * sizeof (int));
        if (!fds) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }

        for (i = 0; i < nfds; ++i) {
            anetNonBlock(Modes.aneterr, newfds[i]);
            fds[n++] = newfds[i];
        }
    }

    service->listener_count = n;
    service->listener_fds = fds;
}

struct net_service *makeBeastInputService(void) {
    return serviceInit("Beast TCP input", NULL, NULL, READ_MODE_BEAST, NULL, decodeBinMessage);
}

struct net_service *makeFatsvOutputService(void) {
    return serviceInit("FATSV TCP output", &Modes.fatsv_out, NULL, READ_MODE_IGNORE, NULL, NULL);
}

void modesInitNet(void) {
    struct net_service *s;
    struct net_service *beast_out;
    struct net_service *beast_in;
    struct net_service *raw_out;
    struct net_service *raw_in;
    struct net_service *vrs_out;
    struct net_service *sbs_out;

    signal(SIGPIPE, SIG_IGN);
    Modes.clients = NULL;
    Modes.services = NULL;

    // set up listeners
    raw_out = serviceInit("Raw TCP output", &Modes.raw_out, send_raw_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(raw_out, Modes.net_bind_address, Modes.net_output_raw_ports);

    beast_out = serviceInit("Beast TCP output", &Modes.beast_out, send_beast_heartbeat, READ_MODE_BEAST_COMMAND, NULL, handleBeastCommand);
    serviceListen(beast_out, Modes.net_bind_address, Modes.net_output_beast_ports);

    vrs_out = serviceInit("VRS json output", &Modes.vrs_out, NULL, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(vrs_out, Modes.net_bind_address, Modes.net_output_vrs_ports);

    sbs_out = serviceInit("Basestation TCP output", &Modes.sbs_out, send_sbs_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(sbs_out, Modes.net_bind_address, Modes.net_output_sbs_ports);

    raw_in = serviceInit("Raw TCP input", NULL, NULL, READ_MODE_ASCII, "\n", decodeHexMessage);
    serviceListen(raw_in, Modes.net_bind_address, Modes.net_input_raw_ports);

    /* Beast input via network */
    beast_in = makeBeastInputService();
    serviceListen(beast_in, Modes.net_bind_address, Modes.net_input_beast_ports);

    /* Beast input from local Modes-S Beast via USB */
    if (Modes.sdr_type == SDR_MODESBEAST) {
        s = makeBeastInputService();
        createGenericClient(s, Modes.beast_fd);
    }
    else if (Modes.sdr_type == SDR_GNS) {
        /* Hex input from local GNS5894 via USART0 */
        s = serviceInit("Hex GNSHAT input", NULL, NULL, READ_MODE_ASCII, "\n", decodeHexMessage);
        s->serial_service = 1;
        createGenericClient(s, Modes.beast_fd);
    }

    if ((Modes.net_push_server_address != NULL) && (Modes.net_push_server_port != NULL)) {
        switch (Modes.net_push_server_mode) {
            default:
            case PUSH_MODE_RAW:
                s = raw_out;
                break;
            case PUSH_MODE_BEAST:
                s = beast_out;
                break;
            case PUSH_MODE_SBS:
                s = sbs_out;
                break;
        }
        serviceConnect(s, Modes.net_push_server_address, Modes.net_push_server_port);
    }
}


uint64_t timeout_start = 0;

//
//=========================================================================
//
// This function gets called from time to time when the decoding thread is
// awakened by new data arriving. This usually happens a few times every second
//
static struct client * modesAcceptClients(void) {
    int fd;
    struct net_service *s;
    struct client *c;

    for (s = Modes.services; s; s = s->next) {
        int i;
        for (i = 0; i < s->listener_count; ++i) {
            struct sockaddr_storage saddr;
            while ((fd = anetTcpAccept(Modes.aneterr, s->listener_fds[i], &saddr)) >= 0) {
                c = createSocketClient(s, fd);
                if (c) {
                    // We created the client, save the sockaddr info
                    memcpy(&c->ss, &saddr, sizeof(saddr));
                    if (Modes.debug & MODES_DEBUG_NET) {
                        char h_p[31];
                        get_host_port(c, h_p, 30);
                        fprintf(stderr, "New client connection from %s (fd %d)\n", h_p, fd);
                    }
                } else {
                    fprintf(stderr, "New client accept failed\n");
                }
            }
        }
    }
    return Modes.clients;
}

static int get_socket_error(int fd) {
    int err = 1;
    socklen_t len = sizeof err;
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &err, &len)) {
        fprintf(stderr, "Get client socket error failed.\n");
    }
    if (err) {
        errno = err; // Set errno to the socket SO_ERROR
    }
    return err;
}

static void close_socket(int fd) {
    if (fd >= 0) {
        get_socket_error(fd); // First clear any errors, which can cause close to fail
        if (shutdown(fd, SHUT_RDWR) < 0) { // Secondly, terminate the reliable delivery
            if (errno != ENOTCONN && errno != EINVAL) { // SGI causes EINVAL
                fprintf(stderr, "Shutdown client socket failed.\n");
            }
        }
        close(fd); // Finally call close() socket
    }
}

//
//=========================================================================
//
// On error free the client, collect the structure, adjust maxfd if needed.
//
static void modesCloseClient(struct client *c) {
    if (!c->service) {
        fprintf(stderr, "warning: double close of net client\n");
        return;
    }

    // Clean up, but defer removing from the list until modesNetCleanup().
    // This is because there may be stackframes still pointing at this
    // client (unpredictably: reading from client A may cause client B to
    // be freed)

    //char ip[20];
    //uint16_t port;
    //inet_ntop(AF_INET,&(((struct sockaddr_in *)&c->ss)->sin_addr), ip, sizeof(ip));
    //port = ntohs( ((struct sockaddr_in *)&c->ss)->sin_port);

    if (Modes.debug & MODES_DEBUG_NET) {
        char h_p[31];
        get_host_port(c, h_p, 30);
        fprintf(stderr, "Closing client connection: %s (fd %d, SendQ %d, RecvQ %d) [%s]\n",
                h_p, c->fd, c->sendq_len, c->buflen, c->service->descr);
    }

    close_socket(c->fd);
    c->service->connections--;

    // mark it as inactive and ready to be freed
    c->fd = -1;
    c->service = NULL;
    c->modeac_requested = 0;
    c->sendq_len = 0;
    if (c->sendq) {
        free(c->sendq);
        c->sendq = NULL;
    }

    autoset_modeac();
}

//
// Send data to clients, if we can...
//
static void flushClients() {
    struct client *c;

    // Iterate across clients, if there is a sendq for one, try to flush it
    for (c = Modes.clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->sendq_len > 0) {
            int towrite = c->sendq_len;
            char *psendq = c->sendq;
            int loops = 0;
            int max_loops = 10;
            int total_nwritten = 0;
            int done = 0;

            do {
#ifndef _WIN32
                int nwritten = write(c->fd, psendq, towrite);
                int err = errno;
#else
                int nwritten = send(c->fd, psendq, towrite, 0);
                int err = WSAGetLastError();
#endif
                loops++;
                // If we get -1, it's only fatal if it's not EAGAIN/EWOULDBLOCK
                if (nwritten < 0) {
                    if (err != EAGAIN && err != EWOULDBLOCK) {
                        char h_p[31];
                        get_host_port(c, h_p, 30);
                        fprintf(stderr, "flushClients: write error to %s: %d (%s) [%s]\n", h_p,
                                err, strerror(err), c->service->descr);
                        modesCloseClient(c);
                    }
                    done = 1;	// Blocking, just bail, try later.
                } else {
                    if (nwritten > 0) {
                        // We've written something, add it to the total
                        total_nwritten += nwritten;
                        // Advance buffer
                        psendq += nwritten;
                        towrite -= nwritten;
                    }
                    if (total_nwritten == c->sendq_len) {
                        done = 1;
                    }
                }
            } while (!done && (loops < max_loops));

            if (total_nwritten > 0) {
                c->last_send = mstime();	// If we wrote anything, update this.
                if (total_nwritten == c->sendq_len) {
                    c->sendq_len = 0;
                } else {
                    c->sendq_len -= total_nwritten;
                    memmove((void*)c->sendq, c->sendq + total_nwritten, towrite);
                }
            }
            c->last_flush = mstime();

            // If we have a queue, but haven't been able to write anything for MODES_NET_HEARTBEAT_INTERVAL, it's dead.
            if (c->sendq_len && ((c->last_flush - c->last_send) > MODES_NET_HEARTBEAT_INTERVAL)) {
                char h_p[31];
                get_host_port(c, h_p, 30);
                fprintf(stderr, "flushClients: Connection to %s (fd %d, SendQ %d) seems dead, closing...\n",
                        h_p, c->fd, c->sendq_len);
                modesCloseClient(c);
            }
        }
    }
    return;
}

//
//=========================================================================
//
// Send the write buffer for the specified writer to all connected clients
//
static void flushWrites(struct net_writer *writer) {
    struct client *c;

    for (c = Modes.clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->service->writer == writer->service->writer) {
            uintptr_t psendq_end = (uintptr_t)c->sendq + c->sendq_len; // Pointer to end of sendq

            // Add the buffer to the client's SendQ
            if ((c->sendq_len + writer->dataUsed) >= c->sendq_max) {
                // Too much data in client SendQ.  Drop client - SendQ exceeded.
                char h_p[31];
                get_host_port(c, h_p, 30);
                fprintf(stderr, "flushWrites: SendQ exceeded for client %s - fd %d (%d > %d), dropping. [%s]\n",
                        h_p, c->fd, c->sendq_len + writer->dataUsed, c->sendq_max, c->service->descr);
                modesCloseClient(c);
                continue;	// Go to the next client
            }
            // Append the data to the end of the queue, increment len
            memcpy((void*)psendq_end, writer->data, writer->dataUsed);
            c->sendq_len += writer->dataUsed;
        }
    }
    writer->dataUsed = 0;
    writer->lastWrite = mstime();
    // Try flushing...
    flushClients();
    return;
}

// Prepare to write up to 'len' bytes to the given net_writer.
// Returns a pointer to write to, or NULL to skip this write.
static void *prepareWrite(struct net_writer *writer, int len) {
    if (!writer ||
            !writer->service ||
            !writer->service->connections ||
            !writer->data)
        return NULL;

    if (len > MODES_OUT_BUF_SIZE)
        return NULL;

    if (writer->dataUsed + len >= MODES_OUT_BUF_SIZE) {
        // Flush now to free some space
        flushWrites(writer);
    }

    return writer->data + writer->dataUsed;
}

// Complete a write previously begun by prepareWrite.
// endptr should point one byte past the last byte written
// to the buffer returned from prepareWrite.
static void completeWrite(struct net_writer *writer, void *endptr) {
    writer->dataUsed = endptr - writer->data;

    if (writer->dataUsed >= Modes.net_output_flush_size) {
        flushWrites(writer);
    }
}

//
//=========================================================================
//
// Write raw output in Beast Binary format with Timestamp to TCP clients
//
static void modesSendBeastOutput(struct modesMessage *mm) {
    int msgLen = mm->msgbits / 8;
    char *p = prepareWrite(&Modes.beast_out, 2 + 2 * (7 + msgLen));
    char ch;
    int j;
    int sig;
    unsigned char *msg = (Modes.net_verbatim ? mm->verbatim : mm->msg);

    if (!p)
        return;

    *p++ = 0x1a;
    if (msgLen == MODES_SHORT_MSG_BYTES) {
        *p++ = '2';
    } else if (msgLen == MODES_LONG_MSG_BYTES) {
        *p++ = '3';
    } else if (msgLen == MODEAC_MSG_BYTES) {
        *p++ = '1';
    } else {
        return;
    }

    /* timestamp, big-endian */
    *p++ = (ch = (mm->timestampMsg >> 40));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 32));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 24));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 16));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 8));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg));
    if (0x1A == ch) {
        *p++ = ch;
    }

    sig = round(sqrt(mm->signalLevel) * 255);
    if (mm->signalLevel > 0 && sig < 1)
        sig = 1;
    if (sig > 255)
        sig = 255;
    *p++ = ch = (char) sig;
    if (0x1A == ch) {
        *p++ = ch;
    }

    for (j = 0; j < msgLen; j++) {
        *p++ = (ch = msg[j]);
        if (0x1A == ch) {
            *p++ = ch;
        }
    }

    completeWrite(&Modes.beast_out, p);
}

static void send_beast_heartbeat(struct net_service *service) {
    static char heartbeat_message[] = {0x1a, '1', 0, 0, 0, 0, 0, 0, 0, 0, 0};
    char *data;

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, sizeof (heartbeat_message));
    if (!data)
        return;

    memcpy(data, heartbeat_message, sizeof (heartbeat_message));
    completeWrite(service->writer, data + sizeof (heartbeat_message));
}

//
//=========================================================================
//
// Print the two hex digits to a string for a single byte.
//
static void printHexDigit(char *p, unsigned char c) {
    const char hex_lookup[] = "0123456789ABCDEF";
    p[0] = hex_lookup[(c >> 4) & 0x0F];
    p[1] = hex_lookup[c & 0x0F];
}

//
//=========================================================================
//
// Write raw output to TCP clients
//
static void modesSendRawOutput(struct modesMessage *mm) {
    int msgLen = mm->msgbits / 8;
    char *p = prepareWrite(&Modes.raw_out, msgLen * 2 + 15);
    int j;
    unsigned char *msg = (Modes.net_verbatim ? mm->verbatim : mm->msg);

    if (!p)
        return;

    if (Modes.mlat && mm->timestampMsg) {
        /* timestamp, big-endian */
        sprintf(p, "@%012" PRIX64,
                mm->timestampMsg);
        p += 13;
    } else
        *p++ = '*';

    for (j = 0; j < msgLen; j++) {
        printHexDigit(p, msg[j]);
        p += 2;
    }

    *p++ = ';';
    *p++ = '\n';

    completeWrite(&Modes.raw_out, p);
}

static void send_raw_heartbeat(struct net_service *service) {
    static char *heartbeat_message = "*0000;\n";
    char *data;
    int len = strlen(heartbeat_message);

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, len);
    if (!data)
        return;

    memcpy(data, heartbeat_message, len);
    completeWrite(service->writer, data + len);
}

//
//=========================================================================
//
// Write SBS output to TCP clients
//
static void modesSendSBSOutput(struct modesMessage *mm, struct aircraft *a) {
    char *p;
    struct timespec now;
    struct tm stTime_receive, stTime_now;
    int msgType;

    // For now, suppress non-ICAO addresses
    if (mm->addr & MODES_NON_ICAO_ADDRESS)
        return;

    p = prepareWrite(&Modes.sbs_out, 200);
    if (!p)
        return;

    //
    // SBS BS style output checked against the following reference
    // http://www.homepages.mcb.net/bones/SBS/Article/Barebones42_Socket_Data.htm - seems comprehensive
    //

    // Decide on the basic SBS Message Type
    switch (mm->msgtype) {
        case 4:
        case 20:
            msgType = 5;
            break;
            break;

        case 5:
        case 21:
            msgType = 6;
            break;

        case 0:
        case 16:
            msgType = 7;
            break;

        case 11:
            msgType = 8;
            break;

        case 17:
        case 18:
            if (mm->metype >= 1 && mm->metype <= 4) {
                msgType = 1;
            } else if (mm->metype >= 5 && mm->metype <= 8) {
                msgType = 2;
            } else if (mm->metype >= 9 && mm->metype <= 18) {
                msgType = 3;
            } else if (mm->metype == 19) {
                msgType = 4;
            } else {
                return;
            }
            break;

        default:
            return;
    }

    // Fields 1 to 6 : SBS message type and ICAO address of the aircraft and some other stuff
    p += sprintf(p, "MSG,%d,1,1,%06X,1,", msgType, mm->addr);

    // Find current system time
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &stTime_now);

    // Find message reception time
    time_t received = (time_t) (mm->sysTimestampMsg / 1000);
    localtime_r(&received, &stTime_receive);

    // Fields 7 & 8 are the message reception time and date
    p += sprintf(p, "%04d/%02d/%02d,", (stTime_receive.tm_year + 1900), (stTime_receive.tm_mon + 1), stTime_receive.tm_mday);
    p += sprintf(p, "%02d:%02d:%02d.%03u,", stTime_receive.tm_hour, stTime_receive.tm_min, stTime_receive.tm_sec, (unsigned) (mm->sysTimestampMsg % 1000));

    // Fields 9 & 10 are the current time and date
    p += sprintf(p, "%04d/%02d/%02d,", (stTime_now.tm_year + 1900), (stTime_now.tm_mon + 1), stTime_now.tm_mday);
    p += sprintf(p, "%02d:%02d:%02d.%03u", stTime_now.tm_hour, stTime_now.tm_min, stTime_now.tm_sec, (unsigned) (now.tv_nsec / 1000000U));

    // Field 11 is the callsign (if we have it)
    if (mm->callsign_valid) {
        p += sprintf(p, ",%s", mm->callsign);
    } else {
        p += sprintf(p, ",");
    }

    // Field 12 is the altitude (if we have it)
    if (Modes.use_gnss) {
        if (mm->altitude_geom_valid) {
            p += sprintf(p, ",%dH", mm->altitude_geom);
        } else if (mm->altitude_baro_valid && trackDataValid(&a->geom_delta_valid)) {
            p += sprintf(p, ",%dH", mm->altitude_baro + a->geom_delta);
        } else if (mm->altitude_baro_valid) {
            p += sprintf(p, ",%d", mm->altitude_baro);
        } else {
            p += sprintf(p, ",");
        }
    } else {
        if (mm->altitude_baro_valid) {
            p += sprintf(p, ",%d", mm->altitude_baro);
        } else if (mm->altitude_geom_valid && trackDataValid(&a->geom_delta_valid)) {
            p += sprintf(p, ",%d", mm->altitude_geom - a->geom_delta);
        } else {
            p += sprintf(p, ",");
        }
    }

    // Field 13 is the ground Speed (if we have it)
    if (mm->gs_valid) {
        p += sprintf(p, ",%.0f", mm->gs.selected);
    } else {
        p += sprintf(p, ",");
    }

    // Field 14 is the ground Heading (if we have it)
    if (mm->heading_valid && mm->heading_type == HEADING_GROUND_TRACK) {
        p += sprintf(p, ",%.0f", mm->heading);
    } else {
        p += sprintf(p, ",");
    }

    // Fields 15 and 16 are the Lat/Lon (if we have it)
    if (mm->cpr_decoded) {
        p += sprintf(p, ",%1.5f,%1.5f", mm->decoded_lat, mm->decoded_lon);
    } else {
        p += sprintf(p, ",,");
    }

    // Field 17 is the VerticalRate (if we have it)
    if (Modes.use_gnss) {
        if (mm->geom_rate_valid) {
            p += sprintf(p, ",%dH", mm->geom_rate);
        } else if (mm->baro_rate_valid) {
            p += sprintf(p, ",%d", mm->baro_rate);
        } else {
            p += sprintf(p, ",");
        }
    } else {
        if (mm->baro_rate_valid) {
            p += sprintf(p, ",%d", mm->baro_rate);
        } else if (mm->geom_rate_valid) {
            p += sprintf(p, ",%d", mm->geom_rate);
        } else {
            p += sprintf(p, ",");
        }
    }

    // Field 18 is  the Squawk (if we have it)
    if (mm->squawk_valid) {
        p += sprintf(p, ",%04x", mm->squawk);
    } else {
        p += sprintf(p, ",");
    }

    // Field 19 is the Squawk Changing Alert flag (if we have it)
    if (mm->alert_valid) {
        if (mm->alert) {
            p += sprintf(p, ",-1");
        } else {
            p += sprintf(p, ",0");
        }
    } else {
        p += sprintf(p, ",");
    }

    // Field 20 is the Squawk Emergency flag (if we have it)
    if (mm->squawk_valid) {
        if ((mm->squawk == 0x7500) || (mm->squawk == 0x7600) || (mm->squawk == 0x7700)) {
            p += sprintf(p, ",-1");
        } else {
            p += sprintf(p, ",0");
        }
    } else {
        p += sprintf(p, ",");
    }

    // Field 21 is the Squawk Ident flag (if we have it)
    if (mm->spi_valid) {
        if (mm->spi) {
            p += sprintf(p, ",-1");
        } else {
            p += sprintf(p, ",0");
        }
    } else {
        p += sprintf(p, ",");
    }

    // Field 22 is the OnTheGround flag (if we have it)
    switch (mm->airground) {
        case AG_GROUND:
            p += sprintf(p, ",-1");
            break;
        case AG_AIRBORNE:
            p += sprintf(p, ",0");
            break;
        default:
            p += sprintf(p, ",");
            break;
    }

    p += sprintf(p, "\r\n");

    completeWrite(&Modes.sbs_out, p);
}

static void send_sbs_heartbeat(struct net_service *service) {
    static char *heartbeat_message = "\r\n"; // is there a better one?
    char *data;
    int len = strlen(heartbeat_message);

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, len);
    if (!data)
        return;

    memcpy(data, heartbeat_message, len);
    completeWrite(service->writer, data + len);
}

//
//=========================================================================
//
void modesQueueOutput(struct modesMessage *mm, struct aircraft *a) {
    int is_mlat = (mm->source == SOURCE_MLAT);

    if (a && !is_mlat && mm->correctedbits < 2) {
        // Don't ever forward 2-bit-corrected messages via SBS output.
        // Don't ever forward mlat messages via SBS output.
        modesSendSBSOutput(mm, a);
    }

    if (!is_mlat && (Modes.net_verbatim || mm->correctedbits < 2)) {
        // Forward 2-bit-corrected messages via raw output only if --net-verbatim is set
        // Don't ever forward mlat messages via raw output.
        modesSendRawOutput(mm);
    }

    if ((!is_mlat || Modes.forward_mlat) && (Modes.net_verbatim || mm->correctedbits < 2)) {
        // Forward 2-bit-corrected messages via beast output only if --net-verbatim is set
        // Forward mlat messages via beast output only if --forward-mlat is set
        modesSendBeastOutput(mm);
    }

    if (a && !is_mlat) {
        writeFATSVEvent(mm, a);
    }
}

// Decode a little-endian IEEE754 float (binary32)
float ieee754_binary32_le_to_float(uint8_t *data) {
    double sign = (data[3] & 0x80) ? -1.0 : 1.0;
    int16_t raw_exponent = ((data[3] & 0x7f) << 1) | ((data[2] & 0x80) >> 7);
    uint32_t raw_significand = ((data[2] & 0x7f) << 16) | (data[1] << 8) | data[0];

    if (raw_exponent == 0) {
        if (raw_significand == 0) {
            /* -0 is treated like +0 */
            return 0;
        } else {
            /* denormal */
            return ldexp(sign * raw_significand, -126 - 23);
        }
    }

    if (raw_exponent == 255) {
        if (raw_significand == 0) {
            /* +/-infinity */
            return sign < 0 ? -INFINITY : INFINITY;
        } else {
            /* NaN */
#ifdef NAN
            return NAN;
#else
            return 0.0f;
#endif
        }
    }

    /* normalized value */
    return ldexp(sign * ((1 << 23) | raw_significand), raw_exponent - 127 - 23);
}

static void handle_radarcape_position(float lat, float lon, float alt) {
	// disable this
	return;
    if (!isfinite(lat) || lat < -90 || lat > 90 || !isfinite(lon) || lon < -180 || lon > 180 || !isfinite(alt))
        return;

    writeFATSVPositionUpdate(lat, lon, alt);

    if (!(Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        Modes.fUserLat = lat;
        Modes.fUserLon = lon;
        Modes.bUserFlags |= MODES_USER_LATLON_VALID;
        receiverPositionChanged(lat, lon, alt);
    }
}

// recompute global Mode A/C setting
static void autoset_modeac() {
    struct client *c;

    if (!Modes.mode_ac_auto)
        return;

    Modes.mode_ac = 0;
    for (c = Modes.clients; c; c = c->next) {
        if (c->modeac_requested) {
            Modes.mode_ac = 1;
            break;
        }
    }
}

// Send some Beast settings commands to a client
void sendBeastSettings(struct client *c, const char *settings) {
    int len;
    char *buf, *p;

    len = strlen(settings) * 3;
    buf = p = alloca(len);

    while (*settings) {
        *p++ = 0x1a;
        *p++ = '1';
        *p++ = *settings++;
    }

    anetWrite(c->fd, buf, len);
}

//
// Handle a Beast command message.
// Currently, we just look for the Mode A/C command message
// and ignore everything else.
//
static int handleBeastCommand(struct client *c, char *p, int remote) {
    MODES_NOTUSED(remote);
    if (p[0] != '1') {
        // huh?
        return 0;
    }

    switch (p[1]) {
        case 'j':
            c->modeac_requested = 0;
            break;
        case 'J':
            c->modeac_requested = 1;
            break;
    }

    autoset_modeac();
    return 0;
}

//
//=========================================================================
//
// This function decodes a Beast binary format message
//
// The message is passed to the higher level layers, so it feeds
// the selected screen output, the network output and so forth.
//
// If the message looks invalid it is silently discarded.
//
// The function always returns 0 (success) to the caller as there is no
// case where we want broken messages here to close the client connection.
//
static int decodeBinMessage(struct client *c, char *p, int remote) {
    int msgLen = 0;
    int j;
    char ch;
    unsigned char msg[MODES_LONG_MSG_BYTES + 7];
    static struct modesMessage zeroMessage;
    struct modesMessage mm;
    MODES_NOTUSED(c);
    memset(&mm, 0, sizeof (mm));

    ch = *p++; /// Get the message type

    if (ch == '1') {
        if (!Modes.mode_ac) {
            if (remote) {
                Modes.stats_current.remote_received_modeac++;
            } else {
                Modes.stats_current.demod_modeac++;
            }
            return 0;
        }
        msgLen = MODEAC_MSG_BYTES;
    } else if (ch == '2') {
        msgLen = MODES_SHORT_MSG_BYTES;
    } else if (ch == '3') {
        msgLen = MODES_LONG_MSG_BYTES;
    } else if (ch == '5') {
        // Special case for Radarcape position messages.
        float lat, lon, alt;

        for (j = 0; j < 21; j++) { // and the data
            msg[j] = ch = *p++;
            if (0x1A == ch) {
                p++;
            }
        }

        lat = ieee754_binary32_le_to_float(msg + 4);
        lon = ieee754_binary32_le_to_float(msg + 8);
        alt = ieee754_binary32_le_to_float(msg + 12);

        handle_radarcape_position(lat, lon, alt);
    } else {
        // Ignore this.
        return 0;
    }

    if (msgLen) {
        mm = zeroMessage;

        /* Beast messages are marked depending on their source. From internet they are marked
         * remote so that we don't try to pass them off as being received by this instance
         * when forwarding them.
         */
        mm.remote = remote;

        // Grab the timestamp (big endian format)
        mm.timestampMsg = 0;
        for (j = 0; j < 6; j++) {
            ch = *p++;
            mm.timestampMsg = mm.timestampMsg << 8 | (ch & 255);
            if (0x1A == ch) {
                p++;
            }
        }

        // record reception time as the time we read it.
        mm.sysTimestampMsg = mstime();

        ch = *p++; // Grab the signal level
        mm.signalLevel = ((unsigned char) ch / 255.0);
        mm.signalLevel = mm.signalLevel * mm.signalLevel;

        /* In case of Mode-S Beast use the signal level per message for statistics */
        if (Modes.sdr_type == SDR_MODESBEAST) {
            Modes.stats_current.signal_power_sum += mm.signalLevel;
            Modes.stats_current.signal_power_count += 1;

            if (mm.signalLevel > Modes.stats_current.peak_signal_power)
                Modes.stats_current.peak_signal_power = mm.signalLevel;
            if (mm.signalLevel > 0.50119)
                Modes.stats_current.strong_signal_count++; // signal power above -3dBFS
        }

        if (0x1A == ch) {
            p++;
        }

        for (j = 0; j < msgLen; j++) { // and the data
            msg[j] = ch = *p++;
            if (0x1A == ch) {
                p++;
            }
        }

        if (msgLen == MODEAC_MSG_BYTES) { // ModeA or ModeC
            if (remote) {
                Modes.stats_current.remote_received_modeac++;
            } else {
                Modes.stats_current.demod_modeac++;
            }
            decodeModeAMessage(&mm, ((msg[0] << 8) | msg[1]));
        } else {
            int result;
            if (remote) {
                Modes.stats_current.remote_received_modes++;
            } else {
                Modes.stats_current.demod_preambles++;
            }
            result = decodeModesMessage(&mm, msg);
            if (result < 0) {
                if (result == -1) {
                    if (remote) {
                        Modes.stats_current.remote_rejected_unknown_icao++;
                    } else {
                        Modes.stats_current.demod_rejected_unknown_icao++;
                    }
                } else {
                    if (remote) {
                        Modes.stats_current.remote_rejected_bad++;
                    } else {
                        Modes.stats_current.demod_rejected_bad++;
                    }
                }
                return 0;
            } else {
                if (remote) {
                    Modes.stats_current.remote_accepted[mm.correctedbits]++;
                } else {
                    Modes.stats_current.demod_accepted[mm.correctedbits]++;
                }
            }
        }

        useModesMessage(&mm);
    }
    return (0);
}
//
//=========================================================================
//
// Turn an hex digit into its 4 bit decimal value.
// Returns -1 if the digit is not in the 0-F range.
//
static int hexDigitVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    else if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    else return -1;
}
//
//=========================================================================
//
// This function decodes a string representing message in raw hex format
// like: *8D4B969699155600E87406F5B69F; The string is null-terminated.
//
// The message is passed to the higher level layers, so it feeds
// the selected screen output, the network output and so forth.
//
// If the message looks invalid it is silently discarded.
//
// The function always returns 0 (success) to the caller as there is no
// case where we want broken messages here to close the client connection.
//
static int decodeHexMessage(struct client *c, char *hex, int remote) {
    int l = strlen(hex), j;
    unsigned char msg[MODES_LONG_MSG_BYTES];
    struct modesMessage mm;
    static struct modesMessage zeroMessage;

    MODES_NOTUSED(remote);
    MODES_NOTUSED(c);
    mm = zeroMessage;

    // Mark messages received over the internet as remote so that we don't try to
    // pass them off as being received by this instance when forwarding them
    mm.remote = 1;
    mm.signalLevel = 0;

    // Remove spaces on the left and on the right
    while (l && isspace(hex[l - 1])) {
        hex[l - 1] = '\0';
        l--;
    }
    while (isspace(*hex)) {
        hex++;
        l--;
    }

    // Turn the message into binary.
    // Accept *-AVR raw @-AVR/BEAST timeS+raw %-AVR timeS+raw (CRC good) <-BEAST timeS+sigL+raw
    // and some AVR records that we can understand
    if (hex[l - 1] != ';') {
        return (0);
    } // not complete - abort

    switch (hex[0]) {
        case '<':
        {
            mm.signalLevel = ((hexDigitVal(hex[13]) << 4) | hexDigitVal(hex[14])) / 255.0;
            mm.signalLevel = mm.signalLevel * mm.signalLevel;
            hex += 15;
            l -= 16; // Skip <, timestamp and siglevel, and ;
            break;
        }

        case '@': // No CRC check
        case '%':
        { // CRC is OK
            hex += 13;
            l -= 14; // Skip @,%, and timestamp, and ;
            break;
        }

        case '*':
        case ':':
        {
            hex++;
            l -= 2; // Skip * and ;
            break;
        }

        default:
        {
            return (0); // We don't know what this is, so abort
            break;
        }
    }

    if ((l != (MODEAC_MSG_BYTES * 2))
            && (l != (MODES_SHORT_MSG_BYTES * 2))
            && (l != (MODES_LONG_MSG_BYTES * 2))) {
        return (0);
    } // Too short or long message... broken

    if ((0 == Modes.mode_ac)
            && (l == (MODEAC_MSG_BYTES * 2))) {
        return (0);
    } // Right length for ModeA/C, but not enabled

    for (j = 0; j < l; j += 2) {
        int high = hexDigitVal(hex[j]);
        int low = hexDigitVal(hex[j + 1]);

        if (high == -1 || low == -1) return 0;
        msg[j / 2] = (high << 4) | low;
    }

    // record reception time as the time we read it.
    mm.sysTimestampMsg = mstime();

    if (l == (MODEAC_MSG_BYTES * 2)) { // ModeA or ModeC
        Modes.stats_current.remote_received_modeac++;
        decodeModeAMessage(&mm, ((msg[0] << 8) | msg[1]));
    } else { // Assume ModeS
        int result;

        Modes.stats_current.remote_received_modes++;
        result = decodeModesMessage(&mm, msg);
        if (result < 0) {
            if (result == -1)
                Modes.stats_current.remote_rejected_unknown_icao++;
            else
                Modes.stats_current.remote_rejected_bad++;
            return 0;
        } else {
            Modes.stats_current.remote_accepted[mm.correctedbits]++;
        }
    }

    useModesMessage(&mm);
    return (0);
}

__attribute__ ((format(printf, 3, 0))) static char *safe_vsnprintf(char *p, char *end, const char *format, va_list ap) {
    p += vsnprintf(p < end ? p : NULL, p < end ? (size_t) (end - p) : 0, format, ap);
    return p;
}

__attribute__ ((format(printf, 3, 4))) static char *safe_snprintf(char *p, char *end, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    p += vsnprintf(p < end ? p : NULL, p < end ? (size_t) (end - p) : 0, format, ap);
    va_end(ap);
    return p;
}

//
//=========================================================================
//
// Return a description of planes in json. No metric conversion
//
// usual caveats about function-returning-pointer-to-static-buffer apply
static const char *jsonEscapeString(const char *str) {
    static char buf[1024];
    const char *in = str;
    char *out = buf, *end = buf + sizeof (buf) - 10;

    for (; *in && out < end; ++in) {
        unsigned char ch = *in;
        if (ch == '"' || ch == '\\') {
            *out++ = '\\';
            *out++ = ch;
        } else if (ch < 32 || ch > 127) {
            out = safe_snprintf(out, end, "\\u%04x", ch);
        } else {
            *out++ = ch;
        }
    }

    *out++ = 0;
    return buf;
}

static char *append_flags(char *p, char *end, struct aircraft *a, datasource_t source) {
    p = safe_snprintf(p, end, "[");

    char *start = p;
    if (a->callsign_valid.source == source)
        p = safe_snprintf(p, end, "\"callsign\",");
    if (a->altitude_baro_valid.source == source)
        p = safe_snprintf(p, end, "\"altitude\",");
    if (a->altitude_geom_valid.source == source)
        p = safe_snprintf(p, end, "\"alt_geom\",");
    if (a->gs_valid.source == source)
        p = safe_snprintf(p, end, "\"gs\",");
    if (a->ias_valid.source == source)
        p = safe_snprintf(p, end, "\"ias\",");
    if (a->tas_valid.source == source)
        p = safe_snprintf(p, end, "\"tas\",");
    if (a->mach_valid.source == source)
        p = safe_snprintf(p, end, "\"mach\",");
    if (a->track_valid.source == source)
        p = safe_snprintf(p, end, "\"track\",");
    if (a->track_rate_valid.source == source)
        p = safe_snprintf(p, end, "\"track_rate\",");
    if (a->roll_valid.source == source)
        p = safe_snprintf(p, end, "\"roll\",");
    if (a->mag_heading_valid.source == source)
        p = safe_snprintf(p, end, "\"mag_heading\",");
    if (a->true_heading_valid.source == source)
        p = safe_snprintf(p, end, "\"true_heading\",");
    if (a->baro_rate_valid.source == source)
        p = safe_snprintf(p, end, "\"baro_rate\",");
    if (a->geom_rate_valid.source == source)
        p = safe_snprintf(p, end, "\"geom_rate\",");
    if (a->squawk_valid.source == source)
        p = safe_snprintf(p, end, "\"squawk\",");
    if (a->emergency_valid.source == source)
        p = safe_snprintf(p, end, "\"emergency\",");
    if (a->nav_qnh_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_qnh\",");
    if (a->nav_altitude_mcp_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_altitude_mcp\",");
    if (a->nav_altitude_fms_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_altitude_fms\",");
    if (a->nav_heading_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_heading\",");
    if (a->nav_modes_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_modes\",");
    if (a->position_valid.source == source)
        p = safe_snprintf(p, end, "\"lat\",\"lon\",\"nic\",\"rc\",");
    if (a->nic_baro_valid.source == source)
        p = safe_snprintf(p, end, "\"nic_baro\",");
    if (a->nac_p_valid.source == source)
        p = safe_snprintf(p, end, "\"nac_p\",");
    if (a->nac_v_valid.source == source)
        p = safe_snprintf(p, end, "\"nac_v\",");
    if (a->sil_valid.source == source)
        p = safe_snprintf(p, end, "\"sil\",\"sil_type\",");
    if (a->gva_valid.source == source)
        p = safe_snprintf(p, end, "\"gva\",");
    if (a->sda_valid.source == source)
        p = safe_snprintf(p, end, "\"sda\",");
    if (p != start)
        --p;
    p = safe_snprintf(p, end, "]");
    return p;
}

static struct {
    nav_modes_t flag;
    const char *name;
} nav_modes_names[] = {
    { NAV_MODE_AUTOPILOT, "autopilot"},
    { NAV_MODE_VNAV, "vnav"},
    { NAV_MODE_ALT_HOLD, "althold"},
    { NAV_MODE_APPROACH, "approach"},
    { NAV_MODE_LNAV, "lnav"},
    { NAV_MODE_TCAS, "tcas"},
    { 0, NULL}
};

static char *append_nav_modes(char *p, char *end, nav_modes_t flags, const char *quote, const char *sep) {
    int first = 1;
    for (int i = 0; nav_modes_names[i].name; ++i) {
        if (!(flags & nav_modes_names[i].flag)) {
            continue;
        }

        if (!first) {
            p = safe_snprintf(p, end, "%s", sep);
        }

        first = 0;
        p = safe_snprintf(p, end, "%s%s%s", quote, nav_modes_names[i].name, quote);
    }

    return p;
}

static const char *nav_modes_flags_string(nav_modes_t flags) {
    static char buf[256];
    buf[0] = 0;
    append_nav_modes(buf, buf + sizeof (buf), flags, "", " ");
    return buf;
}

static const char *addrtype_enum_string(addrtype_t type) {
    switch (type) {
        case ADDR_ADSB_ICAO:
            return "adsb_icao";
        case ADDR_ADSB_ICAO_NT:
            return "adsb_icao_nt";
        case ADDR_ADSR_ICAO:
            return "adsr_icao";
        case ADDR_TISB_ICAO:
            return "tisb_icao";
        case ADDR_ADSB_OTHER:
            return "adsb_other";
        case ADDR_ADSR_OTHER:
            return "adsr_other";
        case ADDR_TISB_OTHER:
            return "tisb_other";
        case ADDR_TISB_TRACKFILE:
            return "tisb_trackfile";
        default:
            return "unknown";
    }
}

static const char *emergency_enum_string(emergency_t emergency) {
    switch (emergency) {
        case EMERGENCY_NONE: return "none";
        case EMERGENCY_GENERAL: return "general";
        case EMERGENCY_LIFEGUARD: return "lifeguard";
        case EMERGENCY_MINFUEL: return "minfuel";
        case EMERGENCY_NORDO: return "nordo";
        case EMERGENCY_UNLAWFUL: return "unlawful";
        case EMERGENCY_DOWNED: return "downed";
        default: return "reserved";
    }
}

static const char *sil_type_enum_string(sil_type_t type) {
    switch (type) {
        case SIL_UNKNOWN: return "unknown";
        case SIL_PER_HOUR: return "perhour";
        case SIL_PER_SAMPLE: return "persample";
        default: return "invalid";
    }
}

static const char *nav_altitude_source_enum_string(nav_altitude_source_t src) {
    switch (src) {
        case NAV_ALT_INVALID: return "invalid";
        case NAV_ALT_UNKNOWN: return "unknown";
        case NAV_ALT_AIRCRAFT: return "aircraft";
        case NAV_ALT_MCP: return "mcp";
        case NAV_ALT_FMS: return "fms";
        default: return "invalid";
    }
}

char *generateAircraftJson(const char *url_path, int *len) {
    uint64_t now = mstime();
    struct aircraft *a;
    int buflen = 256*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;
    char *line_start;
    int first = 1;

    MODES_NOTUSED(url_path);

    _messageNow = now;

    p = safe_snprintf(p, end,
            "{ \"now\" : %.1f,\n"
            "  \"messages\" : %u,\n"
            "  \"aircraft\" : [",
            now / 1000.0,
            Modes.stats_current.messages_total + Modes.stats_alltime.messages_total);

    for (int j = 0; j < AIRCRAFTS_BUCKETS; j++) {
        for (a = Modes.aircrafts[j]; a; a = a->next) {
            if (a->messages < 2) { // basic filter for bad decodes
                continue;
            }
            if ((now - a->seen) > 90E3) // don't include stale aircraft in the JSON
                continue;

            if (first)
                first = 0;
            else
                *p++ = ',';

retry:
            line_start = p;
            p = safe_snprintf(p, end, "\n    {\"hex\":\"%s%06x\"", (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);
            if (a->addrtype != ADDR_ADSB_ICAO)
                p = safe_snprintf(p, end, ",\"type\":\"%s\"", addrtype_enum_string(a->addrtype));
            if (trackDataValid(&a->callsign_valid))
                p = safe_snprintf(p, end, ",\"flight\":\"%s\"", jsonEscapeString(a->callsign));
            if (trackDataValid(&a->airground_valid) && a->airground_valid.source >= SOURCE_MODE_S_CHECKED && a->airground == AG_GROUND)
                p = safe_snprintf(p, end, ",\"alt_baro\":\"ground\"");
            else {
                if (trackDataValid(&a->altitude_baro_valid) && a->altitude_baro_reliable >= 3)
                    p = safe_snprintf(p, end, ",\"alt_baro\":%d", a->altitude_baro);
                if (trackDataValid(&a->altitude_geom_valid))
                    p = safe_snprintf(p, end, ",\"alt_geom\":%d", a->altitude_geom);
            }
            if (trackDataValid(&a->gs_valid))
                p = safe_snprintf(p, end, ",\"gs\":%.1f", a->gs);
            if (trackDataValid(&a->ias_valid))
                p = safe_snprintf(p, end, ",\"ias\":%u", a->ias);
            if (trackDataValid(&a->tas_valid))
                p = safe_snprintf(p, end, ",\"tas\":%u", a->tas);
            if (trackDataValid(&a->mach_valid))
                p = safe_snprintf(p, end, ",\"mach\":%.3f", a->mach);
            if (trackDataValid(&a->track_valid))
                p = safe_snprintf(p, end, ",\"track\":%.1f", a->track);
            if (trackDataValid(&a->track_rate_valid))
                p = safe_snprintf(p, end, ",\"track_rate\":%.2f", a->track_rate);
            if (trackDataValid(&a->roll_valid))
                p = safe_snprintf(p, end, ",\"roll\":%.1f", a->roll);
            if (trackDataValid(&a->mag_heading_valid))
                p = safe_snprintf(p, end, ",\"mag_heading\":%.1f", a->mag_heading);
            if (trackDataValid(&a->true_heading_valid))
                p = safe_snprintf(p, end, ",\"true_heading\":%.1f", a->true_heading);
            if (trackDataValid(&a->baro_rate_valid))
                p = safe_snprintf(p, end, ",\"baro_rate\":%d", a->baro_rate);
            if (trackDataValid(&a->geom_rate_valid))
                p = safe_snprintf(p, end, ",\"geom_rate\":%d", a->geom_rate);
            if (trackDataValid(&a->squawk_valid))
                p = safe_snprintf(p, end, ",\"squawk\":\"%04x\"", a->squawk);
            if (trackDataValid(&a->emergency_valid))
                p = safe_snprintf(p, end, ",\"emergency\":\"%s\"", emergency_enum_string(a->emergency));
            if (a->category != 0)
                p = safe_snprintf(p, end, ",\"category\":\"%02X\"", a->category);
            if (trackDataValid(&a->nav_qnh_valid))
                p = safe_snprintf(p, end, ",\"nav_qnh\":%.1f", a->nav_qnh);
            if (trackDataValid(&a->nav_altitude_mcp_valid))
                p = safe_snprintf(p, end, ",\"nav_altitude_mcp\":%d", a->nav_altitude_mcp);
            if (trackDataValid(&a->nav_altitude_fms_valid))
                p = safe_snprintf(p, end, ",\"nav_altitude_fms\":%d", a->nav_altitude_fms);
            if (trackDataValid(&a->nav_heading_valid))
                p = safe_snprintf(p, end, ",\"nav_heading\":%.1f", a->nav_heading);
            if (trackDataValid(&a->nav_modes_valid)) {
                p = safe_snprintf(p, end, ",\"nav_modes\":[");
                p = append_nav_modes(p, end, a->nav_modes, "\"", ",");
                p = safe_snprintf(p, end, "]");
            }
            if (trackDataValid(&a->position_valid))
                p = safe_snprintf(p, end, ",\"lat\":%f,\"lon\":%f,\"nic\":%u,\"rc\":%u,\"seen_pos\":%.1f", a->lat, a->lon, a->pos_nic, a->pos_rc, (now - a->position_valid.updated) / 1000.0);
            if (a->adsb_version >= 0)
                p = safe_snprintf(p, end, ",\"version\":%d", a->adsb_version);
            if (trackDataValid(&a->nic_baro_valid))
                p = safe_snprintf(p, end, ",\"nic_baro\":%u", a->nic_baro);
            if (trackDataValid(&a->nac_p_valid))
                p = safe_snprintf(p, end, ",\"nac_p\":%u", a->nac_p);
            if (trackDataValid(&a->nac_v_valid))
                p = safe_snprintf(p, end, ",\"nac_v\":%u", a->nac_v);
            if (trackDataValid(&a->sil_valid))
                p = safe_snprintf(p, end, ",\"sil\":%u", a->sil);
            if (a->sil_type != SIL_INVALID)
                p = safe_snprintf(p, end, ",\"sil_type\":\"%s\"", sil_type_enum_string(a->sil_type));
            if (trackDataValid(&a->gva_valid))
                p = safe_snprintf(p, end, ",\"gva\":%u", a->gva);
            if (trackDataValid(&a->sda_valid))
                p = safe_snprintf(p, end, ",\"sda\":%u", a->sda);
            if (trackDataValid(&a->alert_valid))
                p = safe_snprintf(p, end, ",\"alert\":%u", a->alert);
            if (trackDataValid(&a->spi_valid))
                p = safe_snprintf(p, end, ",\"spi\":%u", a->spi);

            p = safe_snprintf(p, end, ",\"mlat\":");
            p = append_flags(p, end, a, SOURCE_MLAT);
            p = safe_snprintf(p, end, ",\"tisb\":");
            p = append_flags(p, end, a, SOURCE_TISB);

            p = safe_snprintf(p, end, ",\"messages\":%ld,\"seen\":%.1f,\"rssi\":%.1f}",
                    a->messages, (now - a->seen) / 1000.0,
                    10 * log10((a->signalLevel[0] + a->signalLevel[1] + a->signalLevel[2] + a->signalLevel[3] +
                            a->signalLevel[4] + a->signalLevel[5] + a->signalLevel[6] + a->signalLevel[7] + 1e-5) / 8));

            if ((p + 10) >= end) { // +10 to leave some space for the final line
                // overran the buffer
                int used = line_start - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
                goto retry;
            }
        }
    }

    p = safe_snprintf(p, end, "\n  ]\n}\n");
    *len = p - buf;
    return buf;
}

static char * appendStatsJson(char *p,
        char *end,
        struct stats *st,
        const char *key) {
    int i;

    p = safe_snprintf(p, end,
            "\"%s\":{\"start\":%.1f,\"end\":%.1f",
            key,
            st->start / 1000.0,
            st->end / 1000.0);

    if (!Modes.net_only) {
        p = safe_snprintf(p, end,
                ",\"local\":{\"samples_processed\":%llu"
                ",\"samples_dropped\":%llu"
                ",\"modeac\":%u"
                ",\"modes\":%u"
                ",\"bad\":%u"
                ",\"unknown_icao\":%u",
                (unsigned long long) st->samples_processed,
                (unsigned long long) st->samples_dropped,
                st->demod_modeac,
                st->demod_preambles,
                st->demod_rejected_bad,
                st->demod_rejected_unknown_icao);

        for (i = 0; i <= Modes.nfix_crc; ++i) {
            if (i == 0) p = safe_snprintf(p, end, ",\"accepted\":[%u", st->demod_accepted[i]);
            else p = safe_snprintf(p, end, ",%u", st->demod_accepted[i]);
        }

        p = safe_snprintf(p, end, "]");

        if (st->signal_power_sum > 0 && st->signal_power_count > 0)
            p = safe_snprintf(p, end, ",\"signal\":%.1f", 10 * log10(st->signal_power_sum / st->signal_power_count));
        if (st->noise_power_sum > 0 && st->noise_power_count > 0)
            p = safe_snprintf(p, end, ",\"noise\":%.1f", 10 * log10(st->noise_power_sum / st->noise_power_count));
        if (st->peak_signal_power > 0)
            p = safe_snprintf(p, end, ",\"peak_signal\":%.1f", 10 * log10(st->peak_signal_power));

        p = safe_snprintf(p, end, ",\"strong_signals\":%d}", st->strong_signal_count);
    }

    if (Modes.net) {
        p = safe_snprintf(p, end,
                ",\"remote\":{\"modeac\":%u"
                ",\"modes\":%u"
                ",\"bad\":%u"
                ",\"unknown_icao\":%u",
                st->remote_received_modeac,
                st->remote_received_modes,
                st->remote_rejected_bad,
                st->remote_rejected_unknown_icao);

        for (i = 0; i <= Modes.nfix_crc; ++i) {
            if (i == 0) p = safe_snprintf(p, end, ",\"accepted\":[%u", st->remote_accepted[i]);
            else p = safe_snprintf(p, end, ",%u", st->remote_accepted[i]);
        }

        p = safe_snprintf(p, end, "]}");
    }

    {
        uint64_t demod_cpu_millis = (uint64_t) st->demod_cpu.tv_sec * 1000UL + st->demod_cpu.tv_nsec / 1000000UL;
        uint64_t reader_cpu_millis = (uint64_t) st->reader_cpu.tv_sec * 1000UL + st->reader_cpu.tv_nsec / 1000000UL;
        uint64_t background_cpu_millis = (uint64_t) st->background_cpu.tv_sec * 1000UL + st->background_cpu.tv_nsec / 1000000UL;

        p = safe_snprintf(p, end,
                ",\"cpr\":{\"surface\":%u"
                ",\"airborne\":%u"
                ",\"global_ok\":%u"
                ",\"global_bad\":%u"
                ",\"global_range\":%u"
                ",\"global_speed\":%u"
                ",\"global_skipped\":%u"
                ",\"local_ok\":%u"
                ",\"local_aircraft_relative\":%u"
                ",\"local_receiver_relative\":%u"
                ",\"local_skipped\":%u"
                ",\"local_range\":%u"
                ",\"local_speed\":%u"
                ",\"filtered\":%u}"
                ",\"altitude_suppressed\":%u"
                ",\"cpu\":{\"demod\":%llu,\"reader\":%llu,\"background\":%llu}"
                ",\"tracks\":{\"all\":%u"
                ",\"single_message\":%u}"
                ",\"messages\":%u"
                ",\"max_distance_in_metres\":%ld"
                ",\"max_distance_in_nautical_miles\":%.1lf}",
                st->cpr_surface,
                st->cpr_airborne,
                st->cpr_global_ok,
                st->cpr_global_bad,
                st->cpr_global_range_checks,
                st->cpr_global_speed_checks,
                st->cpr_global_skipped,
                st->cpr_local_ok,
                st->cpr_local_aircraft_relative,
                st->cpr_local_receiver_relative,
                st->cpr_local_skipped,
                st->cpr_local_range_checks,
                st->cpr_local_speed_checks,
                st->cpr_filtered,
                st->suppressed_altitude_messages,
                (unsigned long long) demod_cpu_millis,
                (unsigned long long) reader_cpu_millis,
                (unsigned long long) background_cpu_millis,
                st->unique_aircraft,
                st->single_message_aircraft,
                st->messages_total,
                (long) st->longest_distance,
                st->longest_distance / 1852.0);
    }

    return p;
}

char *generateStatsJson(const char *url_path, int *len) {
    struct stats add;
    char *buf = (char *) malloc(4096), *p = buf, *end = buf + 4096;

    MODES_NOTUSED(url_path);

    p = safe_snprintf(p, end, "{\n");
    p = appendStatsJson(p, end, &Modes.stats_current, "latest");
    p = safe_snprintf(p, end, ",\n");

    p = appendStatsJson(p, end, &Modes.stats_1min[Modes.stats_latest_1min], "last1min");
    p = safe_snprintf(p, end, ",\n");

    p = appendStatsJson(p, end, &Modes.stats_5min, "last5min");
    p = safe_snprintf(p, end, ",\n");

    p = appendStatsJson(p, end, &Modes.stats_15min, "last15min");
    p = safe_snprintf(p, end, ",\n");

    add_stats(&Modes.stats_alltime, &Modes.stats_current, &add);
    p = appendStatsJson(p, end, &add, "total");
    p = safe_snprintf(p, end, "\n}\n");

    assert(p < end);

    *len = p - buf;
    return buf;
}

//
// Return a description of the receiver in json.
//
char *generateReceiverJson(const char *url_path, int *len) {
    char *buf = (char *) malloc(1024), *p = buf;

    MODES_NOTUSED(url_path);

    p += snprintf(p, 1024, "{ " \
                 "\"version\" : \"%s\", "
            "\"refresh\" : %.0f, "
            "\"history\" : %d",
            MODES_READSB_VERSION, 1.0 * Modes.json_interval, Modes.json_aircraft_history_next + 1 );

    if (Modes.json_location_accuracy && (Modes.fUserLat != 0.0 || Modes.fUserLon != 0.0)) {
        if (Modes.json_location_accuracy == 1) {
            p += snprintf(p, 1024, ", "                \
                         "\"lat\" : %.2f, "
                    "\"lon\" : %.2f",
                    Modes.fUserLat, Modes.fUserLon); // round to 2dp - about 0.5-1km accuracy - for privacy reasons
        } else {
            p += snprintf(p, 1024, ", "                \
                         "\"lat\" : %.6f, "
                    "\"lon\" : %.6f",
                    Modes.fUserLat, Modes.fUserLon); // exact location
        }
    }

    p += snprintf(p, 1024, " }\n");

    *len = (p - buf);
    return buf;
}

// Write JSON to file
void writeJsonToFile(const char *file, char * (*generator) (const char *, int*)) {
#ifndef _WIN32
    char pathbuf[PATH_MAX];
    char tmppath[PATH_MAX];
    int fd;
    int len = 0;
    mode_t mask;
    char *content;

    if (!Modes.json_dir)
        return;

    snprintf(tmppath, PATH_MAX, "%s/%s.XXXXXX", Modes.json_dir, file);
    tmppath[PATH_MAX - 1] = 0;
    fd = mkstemp(tmppath);
    if (fd < 0)
        return;

    mask = umask(0);
    umask(mask);
    fchmod(fd, 0644 & ~mask);

    snprintf(pathbuf, PATH_MAX, "/data/%s", file);
    pathbuf[PATH_MAX - 1] = 0;
    content = generator(pathbuf, &len);

    if (write(fd, content, len) != len)
        goto error_1;

    if (close(fd) < 0)
        goto error_2;

    snprintf(pathbuf, PATH_MAX, "%s/%s", Modes.json_dir, file);
    pathbuf[PATH_MAX - 1] = 0;
    rename(tmppath, pathbuf);
    free(content);
    return;

error_1:
    close(fd);
error_2:
    unlink(tmppath);
    free(content);
    return;
#endif
}

//
//=========================================================================
//
// This function polls the clients using read() in order to receive new
// messages from the net.
//
// The message is supposed to be separated from the next message by the
// separator 'sep', which is a null-terminated C string.
//
// Every full message received is decoded and passed to the higher layers
// calling the function's 'handler'.
//
// The handler returns 0 on success, or 1 to signal this function we should
// close the connection with the client in case of non-recoverable errors.
//
static void modesReadFromClient(struct client *c) {
    int left;
    int nread;
    int bContinue = 1;

    while (bContinue) {
        left = MODES_CLIENT_BUF_SIZE - c->buflen - 1; // leave 1 extra byte for NUL termination in the ASCII case

        // If our buffer is full discard it, this is some badly formatted shit
        if (left <= 0) {
            c->buflen = 0;
            left = MODES_CLIENT_BUF_SIZE;
            // If there is garbage, read more to discard it ASAP
        }
#ifndef _WIN32
        nread = read(c->fd, c->buf + c->buflen, left);
        int err = errno;
#else
        nread = recv(c->fd, c->buf + c->buflen, left, 0);
        if (nread < 0) {
            errno = WSAGetLastError();
        }
#endif

        // If we didn't get all the data we asked for, then return once we've processed what we did get.
        if (nread != left) {
            bContinue = 0;
        }

        if (nread == 0) { // End of file
            char h_p[31];
            get_host_port(c, h_p, 30);
            fprintf(stderr, "EOF from %s\n", h_p);
            modesCloseClient(c);
            return;
        }

#ifndef _WIN32
        if (nread < 0 && (err == EAGAIN || err == EWOULDBLOCK)) // No data available (not really an error)
#else
        if (nread < 0 && errno == EWOULDBLOCK) // No data available (not really an error)
#endif
        {
            return;
        }

        if (nread < 0) { // Other errors
            char h_p[31];
            get_host_port(c, h_p, 30);
            fprintf(stderr, "Read error from %s: %d (%s)\n", h_p, err, strerror(err));
            modesCloseClient(c);
            return;
        }
        if (mstime() - c->last_receive > 4 * 3600 * 1000) {
            char h_p[31];
            get_host_port(c, h_p, 30);
            fprintf(stderr, "No data from %s for 4 hours, closing...\n", h_p);
            modesCloseClient(c);
            return; // no data for four hours
        }

        c->last_receive = mstime();


        c->buflen += nread;

        char *som = c->buf; // first byte of next message
        char *eod = som + c->buflen; // one byte past end of data
        char *p;
        int remote = 1; // Messages will be marked remote by default
        if ((c->fd == Modes.beast_fd) && (Modes.sdr_type == SDR_MODESBEAST || Modes.sdr_type == SDR_GNS)) {
            /* Message from a local connected Modes-S beast or GNS5894 are passed off the internet */
            remote = 0;
        }

        switch (c->service->read_mode) {
            case READ_MODE_IGNORE:
                // drop the bytes on the floor
                som = eod;
                break;

            case READ_MODE_BEAST:
                // This is the Beast Binary scanning case.
                // If there is a complete message still in the buffer, there must be the separator 'sep'
                // in the buffer, note that we full-scan the buffer at every read for simplicity.

                while (som < eod && ((p = memchr(som, (char) 0x1a, eod - som)) != NULL)) { // The first byte of buffer 'should' be 0x1a

                    Modes.stats_current.remote_rejected_bad += ((p - som)/(8 + MODES_SHORT_MSG_BYTES));
                    som = p; // consume garbage up to the 0x1a
                    ++p; // skip 0x1a

                    if (p >= eod) {
                        // Incomplete message in buffer, retry later
                        break;
                    }

                    char *eom; // one byte past end of message
                    if (*p == '1') {
                        eom = p + MODEAC_MSG_BYTES + 8; // point past remainder of message
                    } else if (*p == '2') {
                        eom = p + MODES_SHORT_MSG_BYTES + 8;
                    } else if (*p == '3') {
                        eom = p + MODES_LONG_MSG_BYTES + 8;
                    } else if (*p == '4') {
                        eom = p + MODES_LONG_MSG_BYTES + 8;
                    } else if (*p == '5') {
                        eom = p + MODES_LONG_MSG_BYTES + 8;
                    } else {
                        // Not a valid beast message, skip 0x1a and try again
                        ++som;
                        continue;
                    }

                    // we need to be careful of double escape characters in the message body
                    for (p = som + 1; p < eod && p < eom; p++) {
                        if (0x1A == *p) {
                            p++;
                            eom++;
                        }
                    }

                    if (eom > eod) { // Incomplete message in buffer, retry later
                        break;
                    }


                    // Have a 0x1a followed by 1/2/3/4/5 - pass message to handler.
                    if (c->service->read_handler(c, som + 1, remote)) {
                        modesCloseClient(c);
                        return;
                    }

                    // advance to next message
                    som = eom;
                }
                break;

            case READ_MODE_BEAST_COMMAND:
                while (som < eod && ((p = memchr(som, (char) 0x1a, eod - som)) != NULL)) { // The first byte of buffer 'should' be 0x1a
                    char *eom; // one byte past end of message

                    som = p; // consume garbage up to the 0x1a
                    ++p; // skip 0x1a

                    if (p >= eod) {
                        // Incomplete message in buffer, retry later
                        break;
                    }

                    if (*p == '1') {
                        eom = p + 2;
                    } else {
                        // Not a valid beast command, skip 0x1a and try again
                        ++som;
                        continue;
                    }

                    // we need to be careful of double escape characters in the message body
                    for (p = som + 1; p < eod && p < eom; p++) {
                        if (0x1A == *p) {
                            p++;
                            eom++;
                        }
                    }

                    if (eom > eod) { // Incomplete message in buffer, retry later
                        break;
                    }

                    // Have a 0x1a followed by 1 - pass message to handler.
                    if (c->service->read_handler(c, som + 1, remote)) {
                        modesCloseClient(c);
                        return;
                    }

                    // advance to next message
                    som = eom;
                }
                break;

            case READ_MODE_ASCII:
                //
                // This is the ASCII scanning case, AVR RAW or HTTP at present
                // If there is a complete message still in the buffer, there must be the separator 'sep'
                // in the buffer, note that we full-scan the buffer at every read for simplicity.

                // Always NUL-terminate so we are free to use strstr()
                // nb: we never fill the last byte of the buffer with read data (see above) so this is safe
                *eod = '\0';

                while (som < eod && (p = strstr(som, c->service->read_sep)) != NULL) { // end of first message if found
                    *p = '\0'; // The handler expects null terminated strings
                    if (c->service->read_handler(c, som, remote)) { // Pass message to handler.
                        modesCloseClient(c); // Handler returns 1 on error to signal we .
                        return; // should close the client connection
                    }
                    som = p + strlen(c->service->read_sep); // Move to start of next message
                }

                break;
        }

        if (som > c->buf) { // We processed something - so
            c->buflen = eod - som; //     Update the unprocessed buffer length
            memmove(c->buf, som, c->buflen); //     Move what's remaining to the start of the buffer
        } else { // If no message was decoded process the next client
            return;
        }
    }
}

__attribute__ ((format(printf, 4, 5))) static char *appendFATSV(char *p, char *end, const char *field, const char *format, ...) {
    va_list ap;
    va_start(ap, format);

    p = safe_snprintf(p, end, "%s\t", field);
    p = safe_vsnprintf(p, end, format, ap);
    p = safe_snprintf(p, end, "\t");

    va_end(ap);
    return p;
}

#define TSV_MAX_PACKET_SIZE 800
#define TSV_VERSION "6E"

static void writeFATSVPositionUpdate(float lat, float lon, float alt) {
    static float last_lat, last_lon, last_alt;

    if (lat == last_lat && lon == last_lon && alt == last_alt)
        return;

    last_lat = lat;
    last_lon = lon;
    last_alt = alt;

    char *p = prepareWrite(&Modes.fatsv_out, TSV_MAX_PACKET_SIZE);
    if (!p)
        return;

    char *end = p + TSV_MAX_PACKET_SIZE;

    p = appendFATSV(p, end, "_v",     "%s", TSV_VERSION);
    p = appendFATSV(p, end, "clock", "%" PRIu64, messageNow() / 1000);
    p = appendFATSV(p, end, "type", "%s", "location_update");
    p = appendFATSV(p, end, "lat", "%.5f", lat);
    p = appendFATSV(p, end, "lon", "%.5f", lon);
    p = appendFATSV(p, end, "alt", "%.0f", alt);
    p = appendFATSV(p, end, "altref", "%s", "egm96_meters");
    --p; // remove last tab
    p = safe_snprintf(p, end, "\n");

    if (p < end)
        completeWrite(&Modes.fatsv_out, p);
    else
        fprintf(stderr, "fatsv: output too large (max %d, overran by %d)\n", TSV_MAX_PACKET_SIZE, (int) (p - end));
}

static void writeFATSVEventMessage(struct modesMessage *mm, const char *datafield, unsigned char *data, size_t len) {
    char *p = prepareWrite(&Modes.fatsv_out, TSV_MAX_PACKET_SIZE);
    if (!p)
        return;

    char *end = p + TSV_MAX_PACKET_SIZE;

    p = appendFATSV(p, end, "_v",    "%s", TSV_VERSION);
    p = appendFATSV(p, end, "clock", "%" PRIu64, messageNow() / 1000);
    p = appendFATSV(p, end, (mm->addr & MODES_NON_ICAO_ADDRESS) ? "otherid" : "hexid", "%06X", mm->addr & 0xFFFFFF);
    if (mm->addrtype != ADDR_ADSB_ICAO) {
        p = appendFATSV(p, end, "addrtype", "%s", addrtype_enum_string(mm->addrtype));
    }

    p = safe_snprintf(p, end, "%s\t", datafield);
    for (size_t i = 0; i < len; ++i) {
        p = safe_snprintf(p, end, "%02X", data[i]);
    }
    p = safe_snprintf(p, end, "\n");

    if (p <= end)
        completeWrite(&Modes.fatsv_out, p);
    else
        fprintf(stderr, "fatsv: output too large (max %d, overran by %d)\n", TSV_MAX_PACKET_SIZE, (int) (p - end));
#undef bufsize
}

static void writeFATSVEvent(struct modesMessage *mm, struct aircraft *a) {
    // Write event records for a couple of message types.

    if (!Modes.fatsv_out.service || !Modes.fatsv_out.service->connections) {
        return; // not enabled or no active connections
    }

    if (a->messages < 2) // basic filter for bad decodes
        return;

    switch (mm->msgtype) {
        case 20:
        case 21:
            // DF 20/21: Comm-B: emit if they've changed since we last sent them
            switch (mm->commb_format) {
                case COMMB_DATALINK_CAPS:
                    // BDS 1,0: data link capability report
                    if (memcmp(mm->MB, a->fatsv_emitted_bds_10, 7) != 0) {
                        memcpy(a->fatsv_emitted_bds_10, mm->MB, 7);
                        writeFATSVEventMessage(mm, "datalink_caps", mm->MB, 7);
                    }
                    break;

                case COMMB_ACAS_RA:
                    // BDS 3,0: ACAS RA report
                    if (memcmp(mm->MB, a->fatsv_emitted_bds_30, 7) != 0) {
                        memcpy(a->fatsv_emitted_bds_30, mm->MB, 7);
                        writeFATSVEventMessage(mm, "commb_acas_ra", mm->MB, 7);
                    }
                    break;

                default:
                    // nothing
                    break;
            }
            break;

        case 17:
        case 18:
            // DF 17/18: extended squitter
            if (mm->metype == 28 && mm->mesub == 2 && memcmp(mm->ME, &a->fatsv_emitted_es_acas_ra, 7) != 0) {
                // type 28 subtype 2: ACAS RA report
                // first byte has the type/subtype, remaining bytes match the BDS 3,0 format
                memcpy(a->fatsv_emitted_es_acas_ra, mm->ME, 7);
                writeFATSVEventMessage(mm, "es_acas_ra", mm->ME, 7);
            } else if (mm->metype == 31 && (mm->mesub == 0 || mm->mesub == 1) && memcmp(mm->ME, a->fatsv_emitted_es_status, 7) != 0) {
                // aircraft operational status
                memcpy(a->fatsv_emitted_es_status, mm->ME, 7);
                writeFATSVEventMessage(mm, "es_op_status", mm->ME, 7);
            }
            break;
    }
}

static inline unsigned unsigned_difference(unsigned v1, unsigned v2) {
    return (v1 > v2) ? (v1 - v2) : (v2 - v1);
}

static inline float heading_difference(float h1, float h2) {
    float d = fabs(h1 - h2);
    return (d < 180) ? d : (360 - d);
}

__attribute__ ((format(printf, 6, 7))) static char *appendFATSVMeta(char *p, char *end, const char *field, struct aircraft *a, const data_validity *source, const char *format, ...) {
    const char *sourcetype;
    switch (source->source) {
        case SOURCE_MODE_S:
            sourcetype = "U";
            break;
        case SOURCE_MODE_S_CHECKED:
            sourcetype = "S";
            break;
        case SOURCE_TISB:
            sourcetype = "T";
            break;
        case SOURCE_ADSB:
            sourcetype = "A";
            break;
        default:
            // don't want to forward data sourced from these
            return p;
    }

    if (!trackDataValid(source)) {
        // expired data
        return p;
    }

    if (source->updated > messageNow()) {
        // data in the future
        return p;
    }

    if (source->updated < a->fatsv_last_emitted) {
        // not updated since last time
        return p;
    }

    uint64_t age = (messageNow() - source->updated) / 1000;
    if (age > 255) {
        // too old
        return p;
    }

    p = safe_snprintf(p, end, "%s\t", field);

    va_list ap;
    va_start(ap, format);
    p = safe_vsnprintf(p, end, format, ap);
    va_end(ap);

    p = safe_snprintf(p, end, " %" PRIu64 " %s\t", age, sourcetype);
    return p;
}

static const char *airground_enum_string(airground_t ag) {
    switch (ag) {
        case AG_AIRBORNE:
            return "A+";
        case AG_GROUND:
            return "G+";
        default:
            return "?";
    }
}

static void writeFATSV() {
    struct aircraft *a;
    static uint64_t next_update;

    if (!Modes.fatsv_out.service || !Modes.fatsv_out.service->connections) {
        return; // not enabled or no active connections
    }

    uint64_t now = mstime();
    if (now < next_update) {
        return;
    }

    // scan once a second at most
    next_update = now + 1000;

    for (int j = 0; j < AIRCRAFTS_BUCKETS; j++) {
        for (a = Modes.aircrafts[j]; a; a = a->next) {
            if (a->messages < 2) // basic filter for bad decodes
                continue;

            // don't emit if it hasn't updated since last time
            if (a->seen < a->fatsv_last_emitted) {
                continue;
            }

            // Pretend we are "processing a message" so the validity checks work as expected
            _messageNow = a->seen;

            // some special cases:
            int altValid = trackDataValid(&a->altitude_baro_valid);
            int airgroundValid = trackDataValid(&a->airground_valid) && a->airground_valid.source >= SOURCE_MODE_S_CHECKED; // for non-ADS-B transponders, only trust DF11 CA field
            int gsValid = trackDataValid(&a->gs_valid);
            int squawkValid = trackDataValid(&a->squawk_valid);
            int callsignValid = trackDataValid(&a->callsign_valid) && strcmp(a->callsign, "        ") != 0;
            int positionValid = trackDataValid(&a->position_valid);

            // If we are definitely on the ground, suppress any unreliable altitude info.
            // When on the ground, ADS-B transponders don't emit an ADS-B message that includes
            // altitude, so a corrupted Mode S altitude response from some other in-the-air AC
            // might be taken as the "best available altitude" and produce e.g. "airGround G+ alt 31000".
            if (airgroundValid && a->airground == AG_GROUND && a->altitude_baro_valid.source < SOURCE_MODE_S_CHECKED)
                altValid = 0;

            // if it hasn't changed altitude, heading, or speed much,
            // don't update so often
            int changed =
                (altValid && abs(a->altitude_baro - a->fatsv_emitted_altitude_baro) >= 50) ||
                (trackDataValid(&a->altitude_geom_valid) && abs(a->altitude_geom - a->fatsv_emitted_altitude_geom) >= 50) ||
                (trackDataValid(&a->baro_rate_valid) && abs(a->baro_rate - a->fatsv_emitted_baro_rate) > 500) ||
                (trackDataValid(&a->geom_rate_valid) && abs(a->geom_rate - a->fatsv_emitted_geom_rate) > 500) ||
                (trackDataValid(&a->track_valid) && heading_difference(a->track, a->fatsv_emitted_track) >= 2) ||
                (trackDataValid(&a->track_rate_valid) && fabs(a->track_rate - a->fatsv_emitted_track_rate) >= 0.5) ||
                (trackDataValid(&a->roll_valid) && fabs(a->roll - a->fatsv_emitted_roll) >= 5.0) ||
                (trackDataValid(&a->mag_heading_valid) && heading_difference(a->mag_heading, a->fatsv_emitted_mag_heading) >= 2) ||
                (trackDataValid(&a->true_heading_valid) && heading_difference(a->true_heading, a->fatsv_emitted_true_heading) >= 2) ||
                (gsValid && fabs(a->gs - a->fatsv_emitted_gs) >= 25) ||
                (trackDataValid(&a->ias_valid) && unsigned_difference(a->ias, a->fatsv_emitted_ias) >= 25) ||
                (trackDataValid(&a->tas_valid) && unsigned_difference(a->tas, a->fatsv_emitted_tas) >= 25) ||
                (trackDataValid(&a->mach_valid) && fabs(a->mach - a->fatsv_emitted_mach) >= 0.02);

            int immediate =
                (trackDataValid(&a->nav_altitude_mcp_valid) && unsigned_difference(a->nav_altitude_mcp, a->fatsv_emitted_nav_altitude_mcp) > 50) ||
                (trackDataValid(&a->nav_altitude_fms_valid) && unsigned_difference(a->nav_altitude_fms, a->fatsv_emitted_nav_altitude_fms) > 50) ||
                (trackDataValid(&a->nav_altitude_src_valid) && a->nav_altitude_src != a->fatsv_emitted_nav_altitude_src) ||
                (trackDataValid(&a->nav_heading_valid) && heading_difference(a->nav_heading, a->fatsv_emitted_nav_heading) > 2) ||
                (trackDataValid(&a->nav_modes_valid) && a->nav_modes != a->fatsv_emitted_nav_modes) ||
                (trackDataValid(&a->nav_qnh_valid) && fabs(a->nav_qnh - a->fatsv_emitted_nav_qnh) > 0.8) || // 0.8 is the ES message resolution
                (callsignValid && strcmp(a->callsign, a->fatsv_emitted_callsign) != 0) ||
                (airgroundValid && a->airground == AG_AIRBORNE && a->fatsv_emitted_airground == AG_GROUND) ||
                (airgroundValid && a->airground == AG_GROUND && a->fatsv_emitted_airground == AG_AIRBORNE) ||
                (squawkValid && a->squawk != a->fatsv_emitted_squawk) ||
                (trackDataValid(&a->emergency_valid) && a->emergency != a->fatsv_emitted_emergency);

            uint64_t minAge;
            if (immediate) {
                // a change we want to emit right away
                minAge = 0;
            } else if (!positionValid) {
                // don't send mode S very often
                minAge = 30000;
            } else if ((airgroundValid && a->airground == AG_GROUND) ||
                    (altValid && a->altitude_baro < 500 && (!gsValid || a->gs < 200)) ||
                    (gsValid && a->gs < 100 && (!altValid || a->altitude_baro < 1000))) {
                // we are probably on the ground, increase the update rate
                minAge = 1000;
            } else if (!altValid || a->altitude_baro < 10000) {
                // Below 10000 feet, emit up to every 5s when changing, 10s otherwise
                minAge = (changed ? 5000 : 10000);
            } else {
                // Above 10000 feet, emit up to every 10s when changing, 30s otherwise
                minAge = (changed ? 10000 : 30000);
            }

            if ((now - a->fatsv_last_emitted) < minAge)
                continue;

            char *p = prepareWrite(&Modes.fatsv_out, TSV_MAX_PACKET_SIZE);
            if (!p)
                return;
            char *end = p + TSV_MAX_PACKET_SIZE;

            p = appendFATSV(p, end, "_v",    "%s", TSV_VERSION);
            p = appendFATSV(p, end, "clock", "%" PRIu64, messageNow() / 1000);
            p = appendFATSV(p, end, (a->addr & MODES_NON_ICAO_ADDRESS) ? "otherid" : "hexid", "%06X", a->addr & 0xFFFFFF);

            // for fields we only emit on change,
            // occasionally re-emit them all
            int forceEmit = (now - a->fatsv_last_force_emit) > 600000;

            // these don't change often / at all, only emit when they change
            if (forceEmit || a->addrtype != a->fatsv_emitted_addrtype) {
                p = appendFATSV(p, end, "addrtype", "%s", addrtype_enum_string(a->addrtype));
            }
            if (forceEmit || a->adsb_version != a->fatsv_emitted_adsb_version) {
                p = appendFATSV(p, end, "adsb_version", "%d", a->adsb_version);
            }
            if (forceEmit || a->category != a->fatsv_emitted_category) {
                p = appendFATSV(p, end, "category", "%02X", a->category);
            }
            if (trackDataValid(&a->nac_p_valid) && (forceEmit || a->nac_p != a->fatsv_emitted_nac_p)) {
                p = appendFATSVMeta(p, end, "nac_p", a, &a->nac_p_valid, "%u", a->nac_p);
            }
            if (trackDataValid(&a->nac_v_valid) && (forceEmit || a->nac_v != a->fatsv_emitted_nac_v)) {
                p = appendFATSVMeta(p, end, "nac_v", a, &a->nac_v_valid, "%u", a->nac_v);
            }
            if (trackDataValid(&a->sil_valid) && (forceEmit || a->sil != a->fatsv_emitted_sil)) {
                p = appendFATSVMeta(p, end, "sil", a, &a->sil_valid, "%u", a->sil);
            }
            if (trackDataValid(&a->sil_valid) && (forceEmit || a->sil_type != a->fatsv_emitted_sil_type)) {
                p = appendFATSVMeta(p, end, "sil_type", a, &a->sil_valid, "%s", sil_type_enum_string(a->sil_type));
            }
            if (trackDataValid(&a->nic_baro_valid) && (forceEmit || a->nic_baro != a->fatsv_emitted_nic_baro)) {
                p = appendFATSVMeta(p, end, "nic_baro", a, &a->nic_baro_valid, "%u", a->nic_baro);
            }

            // only emit alt, speed, latlon, track etc if they have been received since the last time
            // and are not stale

            char *dataStart = p;

            // special cases
            if (airgroundValid)
                p = appendFATSVMeta(p, end, "airGround", a, &a->airground_valid, "%s", airground_enum_string(a->airground));
            if (squawkValid)
                p = appendFATSVMeta(p, end, "squawk", a, &a->squawk_valid, "%04x", a->squawk);
            if (callsignValid)
                p = appendFATSVMeta(p, end, "ident", a, &a->callsign_valid, "{%s}", a->callsign);
            if (altValid)
                p = appendFATSVMeta(p, end, "alt", a, &a->altitude_baro_valid, "%d", a->altitude_baro);
            if (positionValid) {
                p = appendFATSVMeta(p, end, "position", a, &a->position_valid, "{%.5f %.5f %u %u}", a->lat, a->lon, a->pos_nic, a->pos_rc);
            }

            p = appendFATSVMeta(p, end, "alt_gnss", a, &a->altitude_geom_valid, "%d", a->altitude_geom);
            p = appendFATSVMeta(p, end, "vrate", a, &a->baro_rate_valid, "%d", a->baro_rate);
            p = appendFATSVMeta(p, end, "vrate_geom", a, &a->geom_rate_valid, "%d", a->geom_rate);
            p = appendFATSVMeta(p, end, "speed", a, &a->gs_valid, "%.1f", a->gs);
            p = appendFATSVMeta(p, end, "speed_ias", a, &a->ias_valid, "%u", a->ias);
            p = appendFATSVMeta(p, end, "speed_tas", a, &a->tas_valid, "%u", a->tas);
            p = appendFATSVMeta(p, end, "mach", a, &a->mach_valid, "%.3f", a->mach);
            p = appendFATSVMeta(p, end, "track", a, &a->track_valid, "%.1f", a->track);
            p = appendFATSVMeta(p, end, "track_rate", a, &a->track_rate_valid, "%.2f", a->track_rate);
            p = appendFATSVMeta(p, end, "roll", a, &a->roll_valid, "%.1f", a->roll);
            p = appendFATSVMeta(p, end, "heading_magnetic", a, &a->mag_heading_valid, "%.1f", a->mag_heading);
            p = appendFATSVMeta(p, end, "heading_true", a, &a->true_heading_valid,    "%.1f", a->true_heading);
            p = appendFATSVMeta(p, end, "nav_alt_mcp", a, &a->nav_altitude_mcp_valid, "%u",   a->nav_altitude_mcp);
            p = appendFATSVMeta(p, end, "nav_alt_fms", a, &a->nav_altitude_fms_valid, "%u",   a->nav_altitude_fms);
            p = appendFATSVMeta(p, end, "nav_alt_src", a, &a->nav_altitude_src_valid, "%s", nav_altitude_source_enum_string(a->nav_altitude_src));
            p = appendFATSVMeta(p, end, "nav_heading", a, &a->nav_heading_valid, "%.1f", a->nav_heading);
            p = appendFATSVMeta(p, end, "nav_modes", a, &a->nav_modes_valid, "{%s}", nav_modes_flags_string(a->nav_modes));
            p = appendFATSVMeta(p, end, "nav_qnh", a, &a->nav_qnh_valid, "%.1f", a->nav_qnh);
            p = appendFATSVMeta(p, end, "emergency", a, &a->emergency_valid, "%s", emergency_enum_string(a->emergency));

            // if we didn't get anything interesting, bail out.
            // We don't need to do anything special to unwind prepareWrite().
            if (p == dataStart) {
                continue;
            }

            --p; // remove last tab
            p = safe_snprintf(p, end, "\n");

            if (p < end)
                completeWrite(&Modes.fatsv_out, p);
            else
                fprintf(stderr, "fatsv: output too large (max %d, overran by %d)\n", TSV_MAX_PACKET_SIZE, (int) (p - end));

            a->fatsv_emitted_altitude_baro = a->altitude_baro;
            a->fatsv_emitted_altitude_geom = a->altitude_geom;
            a->fatsv_emitted_baro_rate = a->baro_rate;
            a->fatsv_emitted_geom_rate = a->geom_rate;
            a->fatsv_emitted_gs = a->gs;
            a->fatsv_emitted_ias = a->ias;
            a->fatsv_emitted_tas = a->tas;
            a->fatsv_emitted_mach = a->mach;
            a->fatsv_emitted_track = a->track;
            a->fatsv_emitted_track_rate = a->track_rate;
            a->fatsv_emitted_roll = a->roll;
            a->fatsv_emitted_mag_heading = a->mag_heading;
            a->fatsv_emitted_true_heading = a->true_heading;
            a->fatsv_emitted_airground = a->airground;
            a->fatsv_emitted_nav_altitude_mcp = a->nav_altitude_mcp;
            a->fatsv_emitted_nav_altitude_fms = a->nav_altitude_fms;
            a->fatsv_emitted_nav_altitude_src = a->nav_altitude_src;
            a->fatsv_emitted_nav_heading = a->nav_heading;
            a->fatsv_emitted_nav_modes = a->nav_modes;
            a->fatsv_emitted_nav_qnh = a->nav_qnh;
            memcpy(a->fatsv_emitted_callsign, a->callsign, sizeof (a->fatsv_emitted_callsign));
            a->fatsv_emitted_addrtype = a->addrtype;
            a->fatsv_emitted_adsb_version = a->adsb_version;
            a->fatsv_emitted_category = a->category;
            a->fatsv_emitted_squawk = a->squawk;
            a->fatsv_emitted_nac_p = a->nac_p;
            a->fatsv_emitted_nac_v = a->nac_v;
            a->fatsv_emitted_sil = a->sil;
            a->fatsv_emitted_sil_type = a->sil_type;
            a->fatsv_emitted_nic_baro = a->nic_baro;
            a->fatsv_emitted_emergency = a->emergency;
            a->fatsv_last_emitted = now;
            if (forceEmit) {
                a->fatsv_last_force_emit = now;
            }
        }
    }
}

//
// Perform periodic network work
//
void modesNetPeriodicWork(void) {
    struct client *c, **prev;
    struct net_service *s;
    uint64_t now = mstime();
    int need_flush = 0;

    // Accept new connections
    modesAcceptClients();

    // Read from clients, and if any need flushing, do so.
    for (c = Modes.clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->service->read_handler)
            modesReadFromClient(c);

        // Only if there is a sendq do we check to see if we need to flush it.
        // 5ms XXX magic number XXX
        if (c->sendq_len &&
                ((c->last_flush + 5) <= now)) {
            need_flush = 1;
        }
    }
    // Do this here, so that hopefully we have less SendQs by the time we process the services for more writer data
    if (need_flush) {
        flushClients();
    }

    // Generate FATSV output
    writeFATSV();

    // If we have generated no messages for a while, send
    // a heartbeat
    if (Modes.net_heartbeat_interval) {
        for (s = Modes.services; s; s = s->next) {
            if (s->writer &&
                    s->connections &&
                    s->writer->send_heartbeat &&
                    (s->writer->lastWrite + Modes.net_heartbeat_interval) <= now) {
                s->writer->send_heartbeat(s);
            }
        }
    }

    // If we have data that has been waiting to be written for a while,
    // write it now.
    for (s = Modes.services; s; s = s->next) {
        if (s->writer &&
                s->writer->dataUsed &&
                ((s->writer->lastWrite + Modes.net_output_flush_interval) <= now)) {
            flushWrites(s->writer);
        }
    }

    // Unlink and free closed clients
    for (prev = &Modes.clients, c = *prev; c; c = *prev) {
        if (c->fd == -1) {
            // Recently closed, prune from list
            *prev = c->next;
            free(c);
        } else {
            prev = &c->next;
        }
    }
}

/**
 * Reads data from serial client (GNS5894) via SignalIO trigger and
 * writes output. Speed up data handling since we have no influence on
 * flow control in that case.
 * Other periodic work is still done in function above and triggered from
 * backgroundTasks().
 */
void modesReadSerialClient(void) {
    struct client *c;

    // Search and read from marked serial client only
    for (c = Modes.clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->service->read_handler && c->service->serial_service)
            modesReadFromClient(c);
    }
    // Generate FATSV output
    writeFATSV();
}

void writeJsonToNet(struct net_writer *writer, char * (*generator) (const char *, int*)) {
    int len = 0;
    int written = 0;
    char *content;
    char *pos;
    int bytes = MODES_OUT_BUF_SIZE / 2;

    char *p = prepareWrite(writer, bytes);
    if (!p)
        return;

    content = generator(NULL, &len);
    pos = content;

    while (p && written < len) {
        if (bytes > len - written) {
            bytes = len - written;
        }
        memcpy(p, pos, bytes);
        p += bytes;
        pos += bytes;
        written += bytes;
        completeWrite(writer, p);

        p = prepareWrite(writer, bytes);
    }

    flushWrites(writer);
    free(content);
}

char *generateVRS(const char *url_path, int *len) {
    uint64_t now = mstime();
    struct aircraft *a;
    int buflen = 256*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;
    char *line_start;
    int first = 1;

    MODES_NOTUSED(url_path);

    _messageNow = now;

    p = safe_snprintf(p, end,
            "{\"acList\":[");

    for (int j = 0; j < AIRCRAFTS_BUCKETS; j++) {
        for (a = Modes.aircrafts[j]; a; a = a->next) {
            if (a->messages < 2) { // basic filter for bad decodes
                continue;
            }
            if ((now - a->seen) > 5E3) // don't include stale aircraft in the JSON
                continue;

            if (first)
                first = 0;
            else
                *p++ = ',';

retry:
            line_start = p;
            p = safe_snprintf(p, end, "{\"Sig\":%.0f",
                    255*((a->signalLevel[0] + a->signalLevel[1] + a->signalLevel[2] + a->signalLevel[3] +
                            a->signalLevel[4] + a->signalLevel[5] + a->signalLevel[6] + a->signalLevel[7] + 1e-5) / 8));

            p = safe_snprintf(p, end, ",\"Icao\":\"%s%06X\"", (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);

            if (trackDataValid(&a->altitude_baro_valid) && a->altitude_baro_reliable >= 3)
                p = safe_snprintf(p, end, ",\"Alt\":%d", a->altitude_baro);
            if (trackDataValid(&a->altitude_geom_valid))
                p = safe_snprintf(p, end, ",\"GAlt\":%d", a->altitude_geom);


            if (trackDataValid(&a->nav_qnh_valid))
                p = safe_snprintf(p, end, ",\"InHg\":%.2f", a->nav_qnh * 0.02952998307);

            //p = safe_snprintf(p, end, ",\"AltT\":%d", 0);

            if (trackDataValid(&a->nav_altitude_mcp_valid)) {
                p = safe_snprintf(p, end, ",\"TAlt\":%d", a->nav_altitude_mcp);
            } else if (trackDataValid(&a->nav_altitude_fms_valid)) {
                p = safe_snprintf(p, end, ",\"TAlt\":%d", a->nav_altitude_fms);
            }

            if (trackDataValid(&a->callsign_valid)) {
                p = safe_snprintf(p, end, ",\"Call\":\"%s\"", jsonEscapeString(a->callsign));
                //p = safe_snprintf(p, end, ",\"CallSus\":false");
            }

            if (trackDataValid(&a->position_valid)) {
                p = safe_snprintf(p, end, ",\"Lat\":%f,\"Long\":%f", a->lat, a->lon);
                p = safe_snprintf(p, end, ",\"PosTime\":%lu", a->position_valid.updated);
            }

            if (a->position_valid.source == SOURCE_MLAT)
                p = safe_snprintf(p, end, ",\"Mlat\":true");
            else
                p = safe_snprintf(p, end, ",\"Mlat\":false");
            if (a->position_valid.source == SOURCE_TISB)
                p = safe_snprintf(p, end, ",\"Tisb\":true");
            else
                p = safe_snprintf(p, end, ",\"Tisb\":false");


            if (trackDataValid(&a->gs_valid)) {
                p = safe_snprintf(p, end, ",\"Spd\":%.1f", a->gs);
                p = safe_snprintf(p, end, ",\"SpdTyp\":0");
            } else if (trackDataValid(&a->ias_valid)) {
                p = safe_snprintf(p, end, ",\"Spd\":%u", a->ias);
                p = safe_snprintf(p, end, ",\"SpdTyp\":2");
            } else if (trackDataValid(&a->tas_valid)) {
                p = safe_snprintf(p, end, ",\"Spd\":%u", a->tas);
                p = safe_snprintf(p, end, ",\"SpdTyp\":3");
            }

            if (trackDataValid(&a->track_valid)) {
                p = safe_snprintf(p, end, ",\"Trak\":%.1f", a->track);
                p = safe_snprintf(p, end, ",\"TrkH\":false");
            } else if (trackDataValid(&a->mag_heading_valid)) {
                p = safe_snprintf(p, end, ",\"Trak\":%.1f", a->mag_heading);
                p = safe_snprintf(p, end, ",\"TrkH\":true");
            } else if (trackDataValid(&a->true_heading_valid)) {
                p = safe_snprintf(p, end, ",\"Trak\":%.1f", a->true_heading);
                p = safe_snprintf(p, end, ",\"TrkH\":true");
            }

            if (trackDataValid(&a->nav_heading_valid))
                p = safe_snprintf(p, end, ",\"TTrk\":%.1f", a->nav_heading);

            if (trackDataValid(&a->squawk_valid))
                p = safe_snprintf(p, end, ",\"Sqk\":\"%04x\"", a->squawk);

            if (trackDataValid(&a->geom_rate_valid)) {
                p = safe_snprintf(p, end, ",\"Vsi\":%d", a->geom_rate);
                p = safe_snprintf(p, end, ",\"VsiT\":1");
			} else if (trackDataValid(&a->baro_rate_valid)) {
                p = safe_snprintf(p, end, ",\"Vsi\":%d", a->baro_rate);
                p = safe_snprintf(p, end, ",\"VsiT\":0");
            }


            if (trackDataValid(&a->airground_valid) && a->airground_valid.source >= SOURCE_MODE_S_CHECKED && a->airground == AG_GROUND)
                p = safe_snprintf(p, end, ",\"Gnd\":true");
            else
                p = safe_snprintf(p, end, ",\"Gnd\":false");

            if (a->adsb_version >= 0)
                p = safe_snprintf(p, end, ",\"Trt\":%d", a->adsb_version + 3);
			else
                p = safe_snprintf(p, end, ",\"Trt\":%d", 1);


            p = safe_snprintf(p, end, ",\"Cmsgs\":%ld", a->messages);

            p = safe_snprintf(p, end, "}");

            if ((p + 10) >= end) { // +10 to leave some space for the final line
                // overran the buffer
                int used = line_start - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
                goto retry;
            }
        }
    }

    p = safe_snprintf(p, end, "]}\n");
    *len = p - buf;
    return buf;
}

//
// =============================== Network IO ===========================
//
