/* Copyright (c) 2017-2019 Foudil Brétel.  All rights reserved. */
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "net/util.h"
#include "utils/array.h"
#include "utils/bits.h"
#include "utils/cont.h"
#include "utils/list.h"
#include "utils/safer.h"
#include "config.h"
#include "log.h"
#include "signals.h"
#include "net/kad/rpc.h"
#include "net/msg.h"
#include "timers.h"
#include "server.h"

// FIXME: low for testing purpose.
#define SERVER_TCP_BUFLEN 10
#define SERVER_UDP_BUFLEN 1400
#define POLL_EVENTS POLLIN|POLLPRI

#define BOOSTRAP_NODES_LEN 64

enum conn_ret {CONN_OK, CONN_CLOSED};

/**
 * A "peer" is a client/server listening on a TCP port that implements some
 * specific protocol (msg). A "node" is a client/server listening on a UDP port
 * implementing the distributed hash table protocol (kad).
 */
struct peer {
    struct list_item        item;
    int                     fd;
    struct sockaddr_storage addr;
    // used for logging = addr:port in hex
    char                    addr_str[32+1+4+1];
    struct proto_msg_parser parser;
};

static int sock_geterr(int fd) {
   int err = 1;
   socklen_t len = sizeof err;
   if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == -1)
       log_perror(LOG_ERR, "Failed getsockopt: %s.", errno);
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
           log_perror(LOG_ERR, "Failed shutdown: %s.", errno);
   if (close(fd) < 0) {         // finally call close()
       log_perror(LOG_ERR, "Failed close: %s.", errno);
       return false;
   }

   return true;
}

static int server_sock_setnonblock(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        log_perror(LOG_ERR, "Failed get fcntl: %s.", errno);
        return errno;
    }
    if (fcntl(sock, F_SETFL, flags |= O_NONBLOCK) == -1) {
        log_perror(LOG_ERR, "Failed set fcntl: %s.", errno);
        return errno;
    }

    return 0;
}

static int server_sock_setopts(int sock, const int family, const int socktype) {
    const int so_false = 0, so_true = 1;
    if ((setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&so_true,
                   sizeof(so_true)) < 0) ||
        (family == AF_INET6 &&
         setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&so_false,
                   sizeof(so_true)) < 0)) {
        log_perror(LOG_ERR, "Failed setsockopt: %s.", errno);
        return errno;
    }

    if (socktype == SOCK_DGRAM) {
        int n = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
        socklen_t len = sizeof(n);
        if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, &len) != -1)
            log_debug("UDP socket SO_RCVBUF=%d", n);
    }

    return 0;
}

/**
 * Returns the listening socket fd, or -1 if failure.
 */
static int
socket_init(const int socktype, const char bind_addr[], const char bind_port[])
{
    int sockfd = -1;
    if (socktype != SOCK_STREAM && socktype != SOCK_DGRAM) {
        log_error("Server init with unsupported socket type.");
        return sockfd;
    }

    // getaddrinfo for host
    struct addrinfo hints, *addrs;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags    = AI_PASSIVE;
    if (getaddrinfo(bind_addr, bind_port, &hints, &addrs)) {
        log_perror(LOG_ERR, "Failed getaddrinfo: %s.", errno);
        return -1;
    }

    // socket and bind
    struct addrinfo *it;
    for (it = addrs; it; it=it->ai_next) {
        sockfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sockfd == -1)
            continue;

        if (server_sock_setopts(sockfd, it->ai_family, it->ai_socktype))
            return -1;

        if (server_sock_setnonblock(sockfd))
            return -1;

        if (bind(sockfd, it->ai_addr, it->ai_addrlen) == 0)
            break;

        sock_close(sockfd);
    }

    freeaddrinfo(addrs);

    if (!it) {
        log_perror(LOG_ERR, "Failed connect: %s.", errno);
        return -1;
    }

    if (socktype == SOCK_STREAM && listen(sockfd, 32)) {
        log_perror(LOG_ERR, "Failed listen: %s.", errno);
        return -1;
    }

    return sockfd;
}

static bool socket_shutdown(int sock)
{
    if (!sock_close(sock))
        return false;
    log_info("Socket closed.");
    return true;
}

static struct peer*
peer_register(struct list_item *peers, int conn, struct sockaddr_storage *addr)
{
    struct peer *peer = calloc(1, sizeof(struct peer));
    if (!peer) {
        log_perror(LOG_ERR, "Failed malloc: %s.", errno);
        return NULL;
    }

    peer->fd = conn;
    peer->addr = *addr;
    sockaddr_storage_fmt(peer->addr_str, &peer->addr);
    proto_msg_parser_init(&peer->parser);
    list_init(&(peer->item));
    list_append(peers, &(peer->item));
    log_debug("Peer %s registered (fd=%d).", peer->addr_str, conn);

    return peer;
}

static struct peer*
peer_find_by_fd(struct list_item *peers, const int fd)
{
    struct peer *p;
    struct list_item * it = peers;
    list_for(it, peers) {
        p = cont(it, struct peer, item);
        if (!p) {
            log_error("Undefined container in list.");
            return NULL;
        }
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
    log_debug("Unregistering peer %s.", peer->addr_str);
    proto_msg_parser_terminate(&peer->parser);
    list_delete(&peer->item);
    free_safer(peer);
}

static bool peer_msg_send(const struct peer *peer, enum proto_msg_type typ,
                          const char *msg, union u32 msg_len)
{
    size_t buf_len = msg_len.dd + PROTO_MSG_FIELD_TYPE_LEN + PROTO_MSG_FIELD_LENGTH_LEN;
    char buf[buf_len];
    char *bufp = buf;
    memcpy(bufp, lookup_by_id(proto_msg_type_names, typ), PROTO_MSG_FIELD_LENGTH_LEN);
    bufp += PROTO_MSG_FIELD_TYPE_LEN;
    memcpy(bufp, u32_hton(msg_len).db, PROTO_MSG_FIELD_LENGTH_LEN);
    bufp += PROTO_MSG_FIELD_LENGTH_LEN;
    memcpy(bufp, msg, (size_t)msg_len.dd);

    int resp = send(peer->fd, buf, buf_len, MSG_NOSIGNAL);
    if (resp < 0) {
        if (errno == EPIPE)
            log_info("Peer fd=%u disconnected while sending.");
        else
            log_perror(LOG_ERR, "Failed send: %s.", errno);
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
                log_perror(LOG_ERR, "Failed server_conn_accept: %s.", errno);
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

        struct peer *p = peer_register(peers, conn, &peer_addr);
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
        log_info("Accepted connection from peer %s.", p->addr_str);
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
    log_info("Closing connection with peer %s.", peer->addr_str);
    if (!sock_close(peer->fd)) {
        log_perror(LOG_ERR, "Failed closed for peer: %s.", errno);
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

static int peer_conn_handle_data(struct peer *peer, struct kad_ctx *kctx)
{
    (void)kctx; // FIXME:
    enum conn_ret ret = CONN_OK;

    char buf[SERVER_TCP_BUFLEN];
    memset(buf, 0, SERVER_TCP_BUFLEN);
    ssize_t slen = recv(peer->fd, buf, SERVER_TCP_BUFLEN, 0);
    if (slen < 0) {
        if (errno != EWOULDBLOCK) {
            log_perror(LOG_ERR, "Failed recv: %s", errno);
            ret = CONN_CLOSED;
        }
        goto end;
    }

    if (slen == 0) {
        log_info("Peer %s closed connection.", peer->addr_str);
        ret = CONN_CLOSED;
        goto end;
    }
    log_debug("Received %d bytes.", slen);

    if (peer->parser.stage == PROTO_MSG_STAGE_ERROR) {
        char *bufx = log_fmt_hex(LOG_ERR, (unsigned char*)buf, slen);
        log_error("Parsing error. buf=%s", bufx);
        free_safer(bufx);
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
            log_info("Notified peer %s of error state.", peer->addr_str);
        }
        else {
            log_warning("Failed to notify peer %s of error state.", peer->addr_str);
            ret = CONN_CLOSED;
        }
        goto end;
    }
    log_debug("Successful parsing of chunk.");

    if (peer->parser.stage == PROTO_MSG_STAGE_NONE) {
        log_info("Got msg %s from peer %s.",
                 lookup_by_id(proto_msg_type_names, peer->parser.msg_type),
                 peer->addr_str);
        // TODO: call tcp handlers here.
    }

  end:
    return ret;
}

static bool node_handle_data(int sock, struct kad_ctx *kctx)
{
    bool ret = true;

    char buf[SERVER_UDP_BUFLEN];
    memset(buf, 0, SERVER_UDP_BUFLEN);
    struct sockaddr_storage node_addr;
    socklen_t node_addr_len = sizeof(struct sockaddr_storage);
    ssize_t slen = recvfrom(sock, buf, SERVER_UDP_BUFLEN, 0,
                            (struct sockaddr *)&node_addr, &node_addr_len);
    if (slen < 0) {
        if (errno != EWOULDBLOCK) {
            log_perror(LOG_ERR, "Failed recv: %s", errno);
            return false;
        }
        return true;
    }
    log_debug("Received %d bytes.", slen);

    struct iobuf rsp = {0};
    bool resp = kad_rpc_handle(kctx, &node_addr, buf, (size_t)slen, &rsp);
    if (rsp.pos == 0) {
        log_info("Empty response. Not responding.");
        ret = resp; goto cleanup;
    }
    if (rsp.pos > SERVER_UDP_BUFLEN) {
        log_error("Response too long.");
        ret = false; goto cleanup;
    }

    slen = sendto(sock, rsp.buf, rsp.pos, 0,
                  (struct sockaddr *)&node_addr, node_addr_len);
    if (slen < 0) {
        if (errno != EWOULDBLOCK) {
            log_perror(LOG_ERR, "Failed sendto: %s", errno);
            ret = false; goto cleanup;
        }
        goto cleanup;
    }
    log_debug("Sent %d bytes.", slen);

  cleanup:
    iobuf_reset(&rsp);
    return ret;
}

static int pollfds_update(struct pollfd fds[], const int nlisten,
                          struct list_item *peer_list)
{
    struct list_item * it = peer_list;
    int npeer = nlisten;
    list_for(it, peer_list) {
        struct peer *p = cont(it, struct peer, item);
        if (!p) {
            log_error("Undefined container in list.");
            return npeer;
        }

        fds[npeer].fd = p->fd;
        /* TODO: we will have to add POLLOUT when all data haven't been written
           in one loop, and probably have 1 inbuf and 1 outbuf. */
        fds[npeer].events = POLL_EVENTS;
        npeer++;
    }
    return npeer;
}

static bool kad_refresh_cb(void *data) {
    (void)data;
    log_info("FIXME refresh cb");
    return true;
}

// Attempt to read bootstrap nodes. Only warn if we find none.
static bool kad_boostrap_cb(void *data) {
    char bootstrap_nodes_path[PATH_MAX];
    bool found = false;
    const char *elts[] = {((struct config*)data)->conf_dir, DATADIR, NULL};
    const char **it = elts;
    while (*it) {
        snprintf(bootstrap_nodes_path, PATH_MAX-1, "%s/nodes.dat", *it);
        bootstrap_nodes_path[PATH_MAX-1] = '\0';
        if (access(bootstrap_nodes_path, R_OK|W_OK) != -1) {
            found = true;
            break;
        }
        it++;
    }

    if (!found) {
        log_warning("Bootstrap node file not readable and writable.");
        return true;
    }

    struct sockaddr_storage nodes[BOOSTRAP_NODES_LEN];
    int nnodes = kad_read_bootstrap_nodes(nodes, ARRAY_LEN(nodes), bootstrap_nodes_path);
    if (nnodes < 0) {
        log_error("Failed to read bootstrap nodes.");
        return false;
    }
    log_info("%d bootstrap nodes read.", nnodes);
    if (nnodes == 0) {
        log_warning("No bootstrap nodes read.");
    }

    /* TODO NEXT ping nodes read from nodes.dat. See node_handle_data,
       kad_rpc_handle_query. Need to:
       - create ping msg
       - sendto()
       - register query into kctx
    */

    return true;
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
bool server_run(const struct config *conf)
{
    bool ret = true;

    if (!timers_clock_res_is_millis()) {
        log_fatal("Time resolution is greater than millisecond. Aborting.");
        return false;
    }

    int sock_tcp = socket_init(SOCK_STREAM, conf->bind_addr, conf->bind_port);
    if (sock_tcp < 0) {
        log_fatal("Failed to start tcp socket. Aborting.");
        return false;
    }
    int sock_udp = socket_init(SOCK_DGRAM, conf->bind_addr, conf->bind_port);
    if (sock_udp < 0) {
        log_fatal("Failed to start udp socket. Aborting.");
        return false;
    }
    log_info("Server started. Listening on [%s]:%s tcp and udp.",
             conf->bind_addr, conf->bind_port);

    struct list_item timer_list = LIST_ITEM_INIT(timer_list);
    struct timer timer_kad_refresh = {
        .name = "kad-refresh",
        .ms = 300000,
        .cb = kad_refresh_cb,
        .item = LIST_ITEM_INIT(timer_kad_refresh.item)
    };
    list_append(&timer_list, &timer_kad_refresh.item);
    struct timer *timer_kad_bootstrap = NULL;

    struct kad_ctx kctx = {0};
    int nodes_len = kad_rpc_init(&kctx, conf->conf_dir);
    if (nodes_len == -1) {
        log_fatal("Failed to initialize DHT. Aborting.");
        return false;
    }
    else if (nodes_len == 0) {
        // TODO turn timer into scheduled event once we have an event queue
        timer_kad_bootstrap = malloc(sizeof(struct timer));
        if (!timer_kad_bootstrap) {
            log_perror(LOG_ERR, "Failed malloc: %s.", errno);
            return false;
        }
        *timer_kad_bootstrap = (struct timer){
            .name = "kad-bootstrap",
            .ms = 0,
            .cb = kad_boostrap_cb,
            .once = true,
            .selfp = &timer_kad_bootstrap
        };
        list_append(&timer_list, &timer_kad_bootstrap->item);
    }
    else {
        log_debug("Loaded %d nodes from config.");
    }

    int nlisten = 2;
    struct pollfd fds[nlisten+conf->max_peers];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = sock_udp;
    fds[0].events = POLL_EVENTS;
    fds[1].fd = sock_tcp;
    fds[1].events = POLL_EVENTS;
    int nfds = nlisten;
    struct list_item peer_list = LIST_ITEM_INIT(peer_list);

    if (!timers_init(&timer_list)) {
        log_fatal("Timers' initialization failed. Aborting.");
        return false;
    }

    /* TODO use proper event queue: enqueue network and timer events
       instead of handling them directly. */
    while (true) {
        if (BITS_CHK(sig_events, EV_SIGINT)) {
            BITS_CLR(sig_events, EV_SIGINT);
            log_info("Caught SIGINT. Shutting down.");
            break;
        }

        int timeout = timers_get_soonest(&timer_list);
        if (timeout < -1) {
            log_fatal("Timeout calculation failed. Aborting.");
            ret = false;
            break;
        }
        log_debug("Waiting to poll (timeout=%li)...", timeout);
        if (poll(fds, nfds, timeout) < 0) {  // event_wait
            if (errno == EINTR)
                continue;
            else {
                log_perror(LOG_ERR, "Failed poll: %s", errno);
                ret = false;
                break;
            }
        }

        for (int i = 0; i < nfds; i++) {
            // event_get_next
            if (fds[i].revents == 0)
                continue;

            if (!BITS_CHK(fds[i].revents, POLL_EVENTS)) {
                log_error("Unexpected revents: %#x", fds[i].revents);
                ret = false;
                goto server_end;
            }

            if (fds[i].fd == sock_udp) {
                node_handle_data(sock_udp, &kctx);
                continue;
            }

            if (fds[i].fd == sock_tcp) {
                if (peer_conn_accept_all(sock_tcp, &peer_list, nfds, conf) >= 0)
                    continue;
                else {
                    log_error("Could not accept tcp connection.");
                    ret = false;
                    goto server_end;
                }
            }

            log_debug("Data available on fd %d.", fds[i].fd);

            // event_dispatch
            struct peer *p = peer_find_by_fd(&peer_list, fds[i].fd);
            if (!p) {
                log_fatal("Unregistered peer fd=%d.", fds[i].fd);
                ret = false;
                goto server_end;
            }

            if (peer_conn_handle_data(p, &kctx) == CONN_CLOSED &&
                !peer_conn_close(p)) {
                log_fatal("Could not close connection of peer fd=%d.", fds[i].fd);
                ret = false;
                goto server_end;
            }

        } /* End loop poll fds */

        nfds = pollfds_update(fds, nlisten, &peer_list);

        if (!timers_apply(&timer_list, (void*)conf)) {
            log_error("Failed to apply all timers.");
            ret = false;
            break;
        }

    } /* End event loop */

  server_end:
    peer_conn_close_all(&peer_list);

    kad_rpc_terminate(&kctx, conf->conf_dir);

    socket_shutdown(sock_tcp);
    socket_shutdown(sock_udp);
    log_info("Server stopped.");
    return ret;
}
