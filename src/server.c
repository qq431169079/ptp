/* Copyright (c) 2017 Foudil Brétel.  All rights reserved. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "utils/bits.h"
#include "utils/cont.h"
#include "utils/list.h"
#include "utils/safe.h"
#include "config.h"
#include "log.h"
#include "signals.h"
#include "proto/msg.h"
#include "server.h"

// FIXME: low for testing purpose.
#define SERVER_BUFLEN 10
#define POLL_EVENTS POLLIN|POLLPRI

enum conn_ret {CONN_OK, CONN_CLOSED};

struct peer {
    struct list_item        item;
    int                     fd;
    char                    host[NI_MAXHOST];
    char                    service[NI_MAXSERV];
    struct proto_msg_parser parser;
};

static int sock_geterr(int fd) {
   int err = 1;
   socklen_t len = sizeof err;
   if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == -1)
       log_perror("Failed getsockopt: %s.", errno);
   if (err)
      errno = err;              // set errno to the socket SO_ERROR
   return err;
}

/**
 * Endeavor to close a socket cleanly.
 * http://stackoverflow.com/a/12730776/421846
 */
static bool sock_close(int fd) {      // *not* the Windows closesocket()
   if (fd < 0) {
       log_error("sock_close() got negative sock.");
       return false;
   }

   sock_geterr(fd);    // first clear any errors, which can cause close to fail
   if (shutdown(fd, SHUT_RDWR) < 0) // secondly, terminate the 'reliable' delivery
       if (errno != ENOTCONN && errno != EINVAL) // SGI causes EINVAL
           log_perror("Failed shutdown: %s.", errno);
   if (close(fd) < 0) {         // finally call close()
       log_perror("Failed close: %s.", errno);
       return false;
   }

   return true;
}

static int server_sock_setnonblock(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        log_perror("Failed get fcntl: %s.", errno);
        return errno;
    }
    if (fcntl(sock, F_SETFL, flags |= O_NONBLOCK) == -1) {
        log_perror("Failed set fcntl: %s.", errno);
        return errno;
    }

    return 0;
}

static int server_sock_setopts(int sock, const int family) {
    const int so_false = 0, so_true = 1;
    if ((setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&so_true,
                   sizeof(so_true)) < 0) ||
        (family == AF_INET6 &&
         setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&so_false,
                   sizeof(so_true)) < 0)) {
        log_perror("Failed setsockopt: %s.", errno);
        return errno;
    }

    return 0;
}

/**
 * Returns the listening socket fd, or -1 if failure.
 */
static int server_init(const char bind_addr[], const char bind_port[])
{
    int listendfd = -1;

    // getaddrinfo for host
    struct addrinfo hints, *addrs;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    if (getaddrinfo(bind_addr, bind_port, &hints, &addrs)) {
        log_perror("Failed getaddrinfo: %s.", errno);
        return -1;
    }

    // socket and bind
    struct addrinfo *it;
    for (it = addrs; it; it=it->ai_next) {
        listendfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listendfd == -1)
            continue;

        if (server_sock_setopts(listendfd, it->ai_family))
            return -1;

        if (server_sock_setnonblock(listendfd))
            return -1;

        if (bind(listendfd, it->ai_addr, it->ai_addrlen) == 0)
            break;

        sock_close(listendfd);
    }

    if (!it) {
        log_perror("Failed connect: %s.", errno);
        return -1;
    }

    freeaddrinfo(addrs);

    if (listen(listendfd, 32)) {
        log_perror("Failed listen: %s.", errno);
        return -1;
    }

    return listendfd;
}

static bool server_shutdown(int sock)
{
    if (!sock_close(sock))
        return false;
    log_info("Stopping server.");
    return true;
}

static struct peer*
peer_register(struct list_item *peers, int conn,
              struct sockaddr_storage *addr, socklen_t *addr_len)
{
    char host[NI_MAXHOST], service[NI_MAXSERV];
    int rv = getnameinfo((struct sockaddr *) addr, *addr_len,
                         host, NI_MAXHOST, service, NI_MAXSERV,
                         NI_NUMERICHOST | NI_NUMERICSERV);
    if (rv) {
        log_error("Failed to getnameinfo: %s.", gai_strerror(rv));
        return NULL;
    }

    struct peer *peer = malloc(sizeof(struct peer));
    if (!peer) {
        log_perror("Failed malloc: %s.", errno);
        return NULL;
    }

    peer->fd = conn;
    strcpy(peer->host, host);
    strcpy(peer->service, service);
    proto_msg_parser_init(&peer->parser);
    list_init(&(peer->item));
    list_append(peers, &(peer->item));
    log_debug("Peer [%s]:%s registered (fd=%d).", host, service, conn);

    return peer;
}

static struct peer*
peer_find_by_fd(struct list_item *peers, const int fd)
{
    struct peer *p;
    struct list_item * it = peers;
    list_for(it, peers) {
        p = cont(it, struct peer, item);
        if (p->fd == fd)
            break;
    }

    if (it == peers) {
        log_warning("Peer not found fd=%d.", fd);
        return NULL;
    }

    return p;
}

static void peer_unregister(struct peer *peer)
{
    log_debug("Unregistering peer [%s]:%s.", peer->host, peer->service);
    proto_msg_parser_terminate(&peer->parser);
    list_delete(&peer->item);
    safe_free(peer);
}

static bool peer_msg_send(const struct peer *peer, enum proto_msg_type typ,
                          const char *msg, union u32 msg_len)
{
    size_t buf_len = msg_len.dd + 8;
    char buf[buf_len];
    char *p = buf;
    memcpy(p, proto_msg_type_get_name(typ), 4);
    memcpy(p+4, u32_hton(msg_len).db, 4);
    memcpy(p+8, msg, (size_t)msg_len.dd);

    int resp = send(peer->fd, buf, buf_len, MSG_NOSIGNAL);
    if (resp < 0) {
        if (errno == EPIPE)
            log_info("Peer fd=%u disconnected while sending.");
        else
            log_perror("Failed send: %s.", errno);
        return false;
    }

    return true;
}

/**
 * Drain all incoming connections
 *
 * Returns 0 on success, -1 on error, 1 when max_peers reached.
 */
static int peer_conn_accept_all(const int listenfd, struct list_item *peers,
                                const int nfds, const struct config *conf)
{
    struct sockaddr_storage peer_addr = {0};
    socklen_t peer_addr_len = sizeof(peer_addr);
    int conn = -1;
    int fail = 0, skipped = 0;
    int npeer = nfds;
    do {
        conn = accept(listenfd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (conn < 0) {
            if (errno != EWOULDBLOCK) {
                log_perror("Failed server_conn_accept: %s.", errno);
                return -1;
            }
            break;
        }
        log_debug("Incoming connection...");

        /* Close the connection nicely when max_peers reached. Another approach
           would be to close the listening socket and reopen it when we're
           ready, which would result in ECONNREFUSED on the client side. */
        if ((size_t)npeer > conf->max_peers) {
            log_error("Can't accept new connections: maximum number of peers"
                      " reached (%d/%zd). conn=%d", npeer - 1, conf->max_peers, conn);
            const char err[] = "Too many connections. Please try later...\n";
            send(conn, err, strlen(err), 0);
            sock_close(conn);
            skipped++;
            continue;
        }

        struct peer *p = peer_register(peers, conn, &peer_addr, &peer_addr_len);
        if (!p) {
            log_error("Failed to register peer fd=%d."
                      " Trying to close connection gracefully.", conn);
            if (sock_close(conn))
                skipped++;
            else { // we have more open connections than actually registered...
                log_fatal("Failed to close connection fd=%d. INCONSISTENT STATE");
                fail++;
            }
            continue;
        }
        log_info("Accepted connection from peer [%s]:%s.", p->host, p->service);
        npeer++;

    } while (conn != -1);

    if (fail)
        return -1;
    else if (skipped)
        return 1;
    else
        return 0;
}

static bool peer_conn_close(struct peer *peer)
{
    bool ret = true;
    log_info("Closing connection with peer [%s]:%s.", peer->host, peer->service);
    if (!sock_close(peer->fd)) {
        log_perror("Failed closed for peer: %s.", errno);
        ret = false;
    }
    peer_unregister(peer);
    return ret;
}

static int peer_conn_close_all(struct list_item *peers)
{
    int fail = 0;
    while (!list_is_empty(peers)) {
        struct peer *p = cont(peers->prev, struct peer, item);
        if (!peer_conn_close(p))
            fail++;
    }
    return fail;
}

static int peer_conn_handle_data(struct peer *peer)
{
    enum conn_ret ret = CONN_OK;

    char buf[SERVER_BUFLEN];
    memset(buf, 0, SERVER_BUFLEN);
    ssize_t slen = recv(peer->fd, buf, SERVER_BUFLEN, 0);
    if (slen < 0) {
        if (errno != EWOULDBLOCK) {
            log_perror("Failed recv: %s", errno);
            ret = CONN_CLOSED;
        }
        goto end;
    }

    if (slen == 0) {
        log_info("Peer [%s]:%s closed connection.", peer->host, peer->service);
        ret = CONN_CLOSED;
        goto end;
    }
    log_debug("Received %d bytes.", slen);

    if (peer->parser.stage == PROTO_MSG_STAGE_ERROR) {
        log_debug_hex(buf, slen);
        goto end;
    }

    if (!proto_msg_parse(&peer->parser, buf, slen)) {
        log_debug("Failed parsing of chunk.");
        /* TODO: how do we get out of the error state ? We could send a
           PROTO_MSG_TYPE_ERROR, then watch for a special PROTO_MSG_TYPE_RESET
           msg (RSET64FE*64). But how about we just close the connection. */
        const char err[] = "Could not parse chunk.";
        union u32 err_len = {strlen(err)};
        if (peer_msg_send(peer, PROTO_MSG_TYPE_ERROR, err, err_len)) {
            log_info("Notified peer [%s]:%s of error state.",
                     peer->host, peer->service);
        }
        else {
            log_warning("Failed to notify peer [%s]:%s of error state.",
                        peer->host, peer->service);
            ret = CONN_CLOSED;
        }
        goto end;
    }
    log_debug("Successful parsing of chunk.");

    if (peer->parser.stage == PROTO_MSG_STAGE_NONE) {
        log_info("Got msg %s from peer [%s]:%s.",
                 proto_msg_type_get_name(peer->parser.msg_type),
                 peer->host, peer->service);
    }

  end:
    return ret;
}

static int pollfds_update(struct pollfd fds[], struct list_item *peer_list)
{
    struct list_item * it = peer_list;
    int npeer = 1;
    list_for(it, peer_list) {
        struct peer *p = cont(it, struct peer, item);
        fds[npeer].fd = p->fd;
        /* TODO: we will have to add POLLOUT when all data haven't been written
           in one loop, and probably have 1 inbuf and 1 outbuf. */
        fds[npeer].events = POLL_EVENTS;
        npeer++;
    }
    return npeer;
}

/**
 * Main event loop
 *
 * poll(2) is portable and should be sufficient as we don't expect to handle
 * thousands of peer connections.
 *
 * Initially inspired from
 * https://www.ibm.com/support/knowledgecenter/en/ssw_i5_54/rzab6/poll.htm
 *
 * TODO: we could use a thread pool with pipes for dispatching heavy tasks (?)
 * http://people.clarkson.edu/~jmatthew/cs644.archive/cs644.fa2001/proj/locksmith/code/ExampleTest/threadpool.c
 * http://stackoverflow.com/a/6954584/421846
 * https://github.com/Pithikos/C-Thread-Pool/blob/master/thpool.c
 */
void server_run(const struct config *conf)
{
    int sock = server_init(conf->bind_addr, conf->bind_port);
    if (sock < 0) {
        log_fatal("Could not start server. Aborting.");
        return;
    }
    log_info("Server started and listening on [%s]:%s.",
             conf->bind_addr, conf->bind_port);

    struct pollfd fds[conf->max_peers];
    memset(fds, 0, sizeof(fds));
    int nfds = 1;
    fds[0].fd = sock;
    fds[0].events = POLL_EVENTS;
    struct list_item peer_list = LIST_ITEM_INIT(peer_list);

    bool server_end = false;
    do {
        if (BITS_CHK(sig_events, EV_SIGINT)) {
            BITS_CLR(sig_events, EV_SIGINT);
            log_info("Caught SIGINT. Shutting down.");
            server_end = true;
            break;
        }

        log_debug("Waiting to poll...");
        if (poll(fds, nfds, -1) < 0) {  // event_wait
            if (errno == EINTR)
                continue;
            else {
                log_perror("Failed poll: %s", errno);
                break;
            }
        }

        for (int i = 0; i < nfds; i++) {
            // event_get_next
            if (fds[i].revents == 0)
                continue;

            if (!BITS_CHK(fds[i].revents, POLL_EVENTS)) {
                log_error("Unexpected revents: %#x", fds[i].revents);
                server_end = true;
                break;
            }

            if (fds[i].fd == sock) {
                if (peer_conn_accept_all(sock, &peer_list, nfds, conf) >= 0)
                    continue;
                else {
                    server_end = true;
                    break;
                }
            }

            log_debug("Data available on fd %d.", fds[i].fd);

            // event_dispatch
            struct peer *p = peer_find_by_fd(&peer_list, fds[i].fd);
            if (!p) {
                log_fatal("Unregistered peer fd=%d.", fds[i].fd);
                server_end = true;
                break;
            }

            if (peer_conn_handle_data(p) == CONN_CLOSED && !peer_conn_close(p)) {
                log_fatal("Could not close connection of peer fd=%d.", fds[i].fd);
                server_end = true;
                break;
            }

        } /* End loop poll fds */

        nfds = pollfds_update(fds, &peer_list);

    } while (!server_end);

    peer_conn_close_all(&peer_list);
    server_shutdown(sock);
}
