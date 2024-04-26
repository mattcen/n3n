/** @file
 * A connection slot abstraction for network services
 *
 * Copyright (C) Hamish Coleman
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#define _GNU_SOURCE
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <netinet/in.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#endif
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "connslot.h"

#ifdef _WIN32
// Windows is a strange place to live, if you are a POSIX programmer
// Taken from https://stackoverflow.com/questions/52988769/writing-own-memmem-for-windows
void *memmem(void *haystack, size_t haystack_len, void * needle, size_t needle_len)
{
    if (haystack == NULL) return NULL; // or assert(haystack != NULL);
    if (haystack_len == 0) return NULL;
    if (needle == NULL) return NULL; // or assert(needle != NULL);
    if (needle_len == 0) return NULL;

    for (char *h = haystack;
            haystack_len >= needle_len;
            ++h, --haystack_len) {
        if (!memcmp(h, needle, needle_len)) {
            return h;
        }
    }
    return NULL;
}
#endif

#ifndef _WIN32
// something something, posix, something something lies
#define closesocket(fh) close(fh)
#endif

void conn_zero(conn_t *conn) {
    conn->fd = -1;
    conn->state = CONN_EMPTY;
    conn->reply = NULL;
    conn->reply_sendpos = 0;
    conn->activity = 0;

    if (conn->request) {
        sb_zero(conn->request);
    }
    if (conn->reply_header) {
        sb_zero(conn->reply_header);
    }
}

int conn_init(conn_t *conn, size_t request_max, size_t reply_header_max) {
    conn->request = sb_malloc(48, request_max);
    conn->reply_header = sb_malloc(48, reply_header_max);

    conn_zero(conn);

    if (!conn->request || !conn->reply_header) {
        return -1;
    }
    return 0;
}

void conn_read(conn_t *conn) {
    conn->state = CONN_READING;

    // If no space available, try increasing our capacity
    if (!sb_avail(conn->request)) {
        strbuf_t *p = sb_realloc(&conn->request, conn->request->capacity + 16);
        if (!p) {
            abort(); // FIXME: do something smarter?
        }
    }

    ssize_t size = sb_read(conn->fd, conn->request);

    if (size == 0) {
        // As we are dealing with non blocking sockets, and have made a non
        // zero-sized read request, the only time we get a zero back is if the
        // far end has closed
        conn->state = CONN_CLOSED;
        return;
    }

    if (size == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            conn->state = CONN_EMPTY;
            return;
        }
        conn->state = CONN_ERROR;
        return;
    }

    // This will truncate the time to a int - usually 32bits
    conn->activity = time(NULL);

    // case protocol==HTTP

    if (sb_len(conn->request)<4) {
        // Not enough bytes to match the end of header check
        return;
    }

    // retrieve the cached expected length, if any
    unsigned int expected_length = conn->request->rd_pos;

    if (expected_length == 0) {
        char *p = memmem(conn->request->str, sb_len(conn->request), "\r\n\r\n", 4);
        if (!p) {
            // As yet, we dont have an entire header
            return;
        }

        int body_pos = p - conn->request->str + 4;

        // Determine if we need to read a body
        p = memmem(
                conn->request->str,
                body_pos,
                "Content-Length:",
                15
        );

        if (!p) {
            // We have an end of header, and the header has no content length field
            // so assume there is no body to read
            conn->state = CONN_READY;
            return;
        }

        p+=15; // Skip the field name
        unsigned int content_length = strtoul(p, NULL, 10);
        expected_length = body_pos + content_length;
        // FIXME: what if Content-Length: is larger than unsigned int?
    }

    // By this point we must have an expected_length

    // cache the calculated total length in the conn
    conn->request->rd_pos = expected_length;

    if (sb_len(conn->request) < expected_length) {
        // Dont have enough length
        return;
    }

    // Do have enough length
    conn->state = CONN_READY;
    conn->request->rd_pos = 0;
    return;
}

ssize_t conn_write(conn_t *conn) {
    ssize_t sent;

    conn->state = CONN_SENDING;

    if (conn->fd == -1) {
        return 0;
    }
#ifndef _WIN32
    int nr = 0;
    unsigned int reply_pos = 0;
    unsigned int end_pos = 0;
    struct iovec vecs[2];

    if (conn->reply_header) {
        end_pos += sb_len(conn->reply_header);
        if (conn->reply_sendpos < sb_len(conn->reply_header)) {
            size_t size = sb_len(conn->reply_header) - conn->reply_sendpos;
            vecs[nr].iov_base = &conn->reply_header->str[conn->reply_sendpos];
            vecs[nr].iov_len = size;
            nr++;
        } else {
            reply_pos = conn->reply_sendpos - sb_len(conn->reply_header);
        }
    }

    if (conn->reply) {
        end_pos += sb_len(conn->reply);
        vecs[nr].iov_base = &conn->reply->str[reply_pos];
        vecs[nr].iov_len = sb_len(conn->reply) - reply_pos;
        nr++;
    }

    sent = writev(conn->fd, &vecs[0], nr);
#else
// no iovec
//
    if (conn->reply_sendpos < sb_len(conn->reply_header)) {
        sent = sb_write(
                conn->fd,
                conn->reply_header,
                conn->reply_sendpos,
                -1
        );
    } else {
        sent = sb_write(
                conn->fd,
                conn->reply,
                conn->reply_sendpos - sb_len(conn->reply_header),
                -1
        );
    }
    unsigned int end_pos = sb_len(conn->reply_header) + sb_len(conn->reply);
#endif

    conn->reply_sendpos += sent;

    if (conn->reply_sendpos >= end_pos) {
        // We have sent the last bytes of this reply
        conn->state = CONN_EMPTY;
        conn->reply_sendpos = 0;
        sb_zero(conn->reply_header);
        sb_zero(conn->request);
    }

    // This will truncate the time to a int - usually 32bits
    conn->activity = time(NULL);
    return sent;
}

int conn_iswriter(conn_t *conn) {
    switch (conn->state) {
        case CONN_SENDING:
            return 1;
        default:
            return 0;
    }
}

void conn_close(conn_t *conn) {
    closesocket(conn->fd);
    conn_zero(conn);
    // TODO: could shrink the size here, maybe in certain circumstances?
}

void conn_dump(strbuf_t **buf, conn_t *conn) {
    sb_reprintf(
        buf,
        "%i:%i@%i;%i ",
        conn->fd,
        conn->state,
        conn->reply_sendpos,
        conn->activity
    );

    if (conn->request) {
        sb_reprintf(
            buf,
            "%p:%u/%u ",
            conn->request,
            conn->request->wr_pos,
            conn->request->capacity
        );
    } else {
        sb_reprintf(buf, "NULL ");
    }

    if (conn->reply) {
        sb_reprintf(
            buf,
            "%p:%u/%u ",
            conn->reply,
            conn->reply->wr_pos,
            conn->reply->capacity
        );
    } else {
        sb_reprintf(buf, "NULL ");
    }

    if (conn->reply_header) {
        sb_reprintf(
            buf,
            "%p:%u/%u ",
            conn->reply_header,
            conn->reply_header->wr_pos,
            conn->reply_header->capacity
        );
    } else {
        sb_reprintf(buf, "NULL ");
    }

    sb_reprintf(buf, "\n");

    // TODO: strbuf capacity_max and contents?
}

void slots_free(slots_t *slots) {
    for (int i=0; i < slots->nr_slots; i++) {
        conn_t *conn = &slots->conn[i];

        // Since it makes buffer handling significantly simpler, It is a
        // common pattern that the request buffer is reused for the reply.
        // Avoid double-free by checking for that.
        //
        // Usually, this has not mattered, but Ubuntu defaults to using
        // some pedantic malloc() settings - and I cannot argue against that.
        if(conn->request != conn->reply) {
            // TODO: the application usually owns conn->reply, should we free?
            free(conn->reply);
            conn->reply = NULL;
        }

        free(conn->request);
        conn->request = NULL;
        free(conn->reply_header);
        conn->reply_header = NULL;

        // TODO:
        // - close any open sockets?
        // - decrement the nr_open?
    }
    free(slots);
}

slots_t *slots_malloc(int nr_slots, size_t req_max, size_t reply_header_max) {
    size_t bytes = sizeof(slots_t) + nr_slots * sizeof(conn_t);
    slots_t *slots = malloc(bytes);
    if (!slots) {
        return NULL;
    }

    slots->nr_slots = nr_slots;

    // Set any defaults
    slots->timeout = 60;
    slots->nr_open = 0;

    for (int i=0; i < SLOTS_LISTEN; i++) {
        slots->listen[i] = -1;
    }

    int r = 0;
    for (int i=0; i < nr_slots; i++) {
        r += conn_init(&slots->conn[i], req_max, reply_header_max);
    }

    if (r!=0) {
        slots_free(slots);
        slots=NULL;
    }
    return slots;
}

static int _slots_listen_find_empty(slots_t *slots) {
    int listen_nr;
    for (listen_nr=0; listen_nr < SLOTS_LISTEN; listen_nr++) {
        if (slots->listen[listen_nr] == -1) {
            break;
        }
    }
    if (listen_nr == SLOTS_LISTEN) {
        // All listen slots full
        return -1;
    }
    return listen_nr;
}

int slots_listen_tcp(slots_t *slots, int port, bool allow_remote) {
    int listen_nr = _slots_listen_find_empty(slots);
    if (listen_nr <0) {
        return -2;
    }

    int server;
#ifndef _WIN32
    int on = 1;
    int off = 0;
#else
    char on = 1;
    char off = 0;
#endif
    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };

    if (!allow_remote) {
        memcpy(&addr.sin6_addr, &in6addr_loopback, sizeof(in6addr_loopback));
    }

    if ((server = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        // try again with IPv4
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        if (allow_remote) {
            addr4->sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            addr4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if ((server = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            return -1;
        }
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    if (bind(server, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        return -1;
    }

    // backlog of 1 - low, but sheds load quickly when we run out of slots
    if (listen(server, 1) < 0) {
        return -1;
    }

    slots->listen[listen_nr] = server;
    return 0;
}

#ifndef _WIN32
int slots_listen_unix(slots_t *slots, char *path, int mode, int uid, int gid) {
    int listen_nr = _slots_listen_find_empty(slots);
    if (listen_nr <0) {
        return -2;
    }

    struct sockaddr_un addr;

    if (strlen(path) > sizeof(addr.sun_path) -1) {
        return -1;
    }

    int server;

    if ((server = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    addr.sun_family = AF_UNIX,
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) -1);

    if (remove(path) == -1 && errno != ENOENT) {
        return -1;
    }

    if (bind(server, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        return -1;
    }

    // For both the chmod and chown, we ignore the result: either it worked or
    // not.
    //
    // TODO:
    // - mark it so the compiler doesnt complain

    if(mode > 0) {
        fchmod(server, mode);
    }

    if(uid != -1 && gid != -1) {
        chown(path, uid, gid);
    }

    // backlog of 1 - low, but sheds load quickly when we run out of slots
    if (listen(server, 1) < 0) {
        return -1;
    }

    slots->listen[listen_nr] = server;
    return 0;
}
#endif

/*
 * Close any listening sockets.
 * We dont check for or care about any errors as this is assumed to be used
 * during a shutdown event.
 * (Mostly, as a signaling feature for windows and its signals/select
 * brain damage)
 */
void slots_listen_close(slots_t *slots) {
    for (int i=0; i < SLOTS_LISTEN; i++) {
        closesocket(slots->listen[i]);
        slots->listen[i] = -1;
    }
}

int slots_fdset(slots_t *slots, fd_set *readers, fd_set *writers) {
    int i;
    int fdmax = 0;

    int nr_open = 0;

    for (i=0; i<slots->nr_slots; i++) {
        if (slots->conn[i].fd == -1) {
            continue;
        }
        nr_open++;
        int fd = slots->conn[i].fd;
        FD_SET(fd, readers);
        if (conn_iswriter(&slots->conn[i])) {
            FD_SET(fd, writers);
        }
        fdmax = (fd > fdmax)? fd : fdmax;
    }

    // Since we scan all the slots, we have an accurate nr_open count
    // (library users could be converting slots into a different protocols)
    slots->nr_open = nr_open;

    // If we have room for more connections, we listen on the server socket(s)
    if (slots->listen[0] && slots->nr_open < slots->nr_slots) {
        for (i=0; i<SLOTS_LISTEN; i++) {
            if (slots->listen[i] == -1) {
                continue;
            }
            int fd = slots->listen[i];
            FD_SET(fd, readers);
            fdmax = (fd > fdmax)? fd : fdmax;
        }
    }

    return fdmax;
}

int slots_accept(slots_t *slots, int listen_nr) {
    int i;

    // TODO: remember previous checked slot and dont start at zero
    for (i=0; i<slots->nr_slots; i++) {
        if (slots->conn[i].fd == -1) {
            break;
        }
    }

    if (i == slots->nr_slots) {
        // No room, inform the caller
        return -2;
    }

    int client = accept(slots->listen[listen_nr], NULL, 0);
    if (client == -1) {
        return -1;
    }

#ifndef _WIN32
    fcntl(client, F_SETFL, O_NONBLOCK);
#else
    u_long arg = 1;
    ioctlsocket(client, FIONBIO, &arg);
#endif

    slots->nr_open++;
    // This will truncate the time to a int - usually 32bits
    slots->conn[i].activity = time(NULL);
    slots->conn[i].fd = client;
    return i;
}

int slots_closeidle(slots_t *slots) {
    int i;
    int nr_closed = 0;
    int now = time(NULL);

    for (i=0; i<slots->nr_slots; i++) {
        if (slots->conn[i].fd == -1) {
            continue;
        }
        int delta_t = now - slots->conn[i].activity;
        if (delta_t > slots->timeout) {
            // TODO: metrics timeouts ++
            conn_close(&slots->conn[i]);
            nr_closed++;
        }
    }
    slots->nr_open -= nr_closed;
    if (slots->nr_open < 0) {
        slots->nr_open = 0;
        // should not happen
    }

    return nr_closed;
}

int slots_fdset_loop(slots_t *slots, fd_set *readers, fd_set *writers) {
    for (int i=0; i<SLOTS_LISTEN; i++) {
        if (slots->listen[i] == -1) {
            continue;
        }
        if (FD_ISSET(slots->listen[i], readers)) {
            // A new connection
            int slotnr = slots_accept(slots, i);

            switch (slotnr) {
                case -1:
                case -2:
                    return slotnr;

                default:
                    // Schedule slot for immediately reading
                    // TODO: if protocol == http
                    FD_SET(slots->conn[slotnr].fd, readers);
            }
        }
    }

    int nr_ready = 0;
    int nr_open = 0;

    for (int i=0; i<slots->nr_slots; i++) {
        if (slots->conn[i].fd == -1) {
            continue;
        }
        nr_open++;

        if (FD_ISSET(slots->conn[i].fd, readers)) {
            conn_read(&slots->conn[i]);
            // possibly sets state to CONN_READY
        }

        switch (slots->conn[i].state) {
            case CONN_READY:
                // After a read, we could be CONN_EMPTY or CONN_READY
                // we reach state CONN_READY once there is a full request buf
                nr_ready++;
                // TODO:
                // - parse request
                // - possibly callback to generate reply
                break;
            case CONN_ERROR:
                // Slots with errors are dead to us
                /* fallsthrough */
            case CONN_CLOSED:
                slots->nr_open--;
                conn_close(&slots->conn[i]);
                continue;
            default:
                break;
        }

        if (FD_ISSET(slots->conn[i].fd, writers)) {
            conn_write(&slots->conn[i]);
        }
    }

    // Since we scan all the slots, we have an accurate nr_open count
    // (library users could be converting slots into a different protocols)
    slots->nr_open = nr_open;

    return nr_ready;
}

void slots_dump(strbuf_t **buf, slots_t *slots) {
    if (!slots) {
        sb_reprintf(buf, "NULL\n");
        return;
    }
    sb_reprintf(
        buf,
        "slots: %i/%i, timeout=%i, listen=",
        slots->nr_open,
        slots->nr_slots,
        slots->timeout
    );

    for (int i=0; i < SLOTS_LISTEN; i++) {
        sb_reprintf(buf, "%i,", slots->listen[i]);
    }
    sb_reprintf(buf, "\n");
    for (int i=0; i < slots->nr_slots; i++) {
        conn_dump(buf, &slots->conn[i]);
    }
}
