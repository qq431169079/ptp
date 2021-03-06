/* Copyright (c) 2017-2019 Foudil Brétel.  All rights reserved. */
#include <unistd.h>
#include "utils/array.h"
#include "config.h"
#include "log.h"
#include "net/kad/rpc.h"
#include "net/socket.h"
#include "timers.h"
#include "net/actions.h"

#define BOOTSTRAP_NODES_LEN 64
// FIXME: low for testing purpose.
#define SERVER_TCP_BUFLEN 10
#define SERVER_UDP_BUFLEN 1400

bool node_handle_data(int sock, struct kad_ctx *kctx)
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
        log_info("Handling incoming message did not produce response. Not responding.");
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

/**
 * Drain all incoming connections
 *
 * Returns 0 on success, -1 on error, 1 when max_peers reached.
 */
int peer_conn_accept_all(const int listenfd, struct list_item *peers,
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

struct peer*
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

int peer_conn_handle_data(struct peer *peer, struct kad_ctx *kctx)
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

bool peer_conn_close(struct peer *peer)
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

int peer_conn_close_all(struct list_item *peers)
{
    int fail = 0;
    while (!list_is_empty(peers)) {
        struct peer *p = cont(peers->prev, struct peer, item);
        if (!peer_conn_close(p))
            fail++;
    }
    return fail;
}

bool kad_refresh(void *data)
{
    (void)data;
    log_info("FIXME refresh cb");
    return true;
}

// Attempt to read bootstrap nodes. Only warn if we find none.
bool kad_bootstrap(struct list_item *timer_list, const struct config *conf,
                   struct kad_ctx *kctx, const int sock)
{
    char bootstrap_nodes_path[PATH_MAX];
    bool found = false;
    const char *elts[] = {conf->conf_dir, DATADIR, NULL};
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

    struct sockaddr_storage nodes[BOOTSTRAP_NODES_LEN];
    int nnodes = kad_read_bootstrap_nodes(nodes, ARRAY_LEN(nodes), bootstrap_nodes_path);
    if (nnodes < 0) {
        log_error("Failed to read bootstrap nodes.");
        return false;
    }
    log_info("%d bootstrap nodes read.", nnodes);
    if (nnodes == 0) {
        log_warning("No bootstrap nodes read.");
    }


    struct list_item timer_list_tmp;
    list_init(&timer_list_tmp);
    struct event *event_node_ping[nnodes];
    int i=0;
    for (; i<nnodes; i++) {
        event_node_ping[i] = malloc(sizeof(struct event));
        if (!event_node_ping[i]) {
            log_perror(LOG_ERR, "Failed malloc: %s.", errno);
            goto cleanup;
        }
        *event_node_ping[i] = (struct event){
            "node-ping", .cb=event_node_ping_cb,
            .args.node_ping={
                .kctx=kctx, .sock=sock,
                .node={.id={{0},0}, .addr=nodes[i], .addr_str={0}}
            },
            .fatal=false, .self=event_node_ping[i]
        };
        sockaddr_storage_fmt(event_node_ping[i]->args.node_ping.node.addr_str,
                             &event_node_ping[i]->args.node_ping.node.addr);

        struct timer *timer_node_ping = malloc(sizeof(struct timer));
        if (!timer_node_ping) {
            log_perror(LOG_ERR, "Failed malloc: %s.", errno);
            goto cleanup;
        }
        *timer_node_ping = (struct timer){
            .name="node-ping", .ms=0, .expire=now_millis(),
            .event=event_node_ping[i], .once=true, .self=timer_node_ping
        };
        list_append(&timer_list_tmp, &timer_node_ping->item);
    }

    list_concat(timer_list, &timer_list_tmp);
    return true;

  cleanup:
    for (int j=0; j<i; j++) {
        free(event_node_ping[j]);
    }
    struct list_item *timer_it = &timer_list_tmp;
    list_for(timer_it, &timer_list_tmp) {
        struct timer *p = cont(timer_it, struct timer, item);
        if (p)
            free(p->self);
    }
    return false;
}

bool node_ping(struct kad_ctx *kctx, const int sock, const struct kad_node_info node)
{
    log_info("Kad pinging %s", node.addr_str);

    struct kad_rpc_query *query = calloc(1, sizeof(struct kad_rpc_query));
    if (!query) {
        log_perror(LOG_ERR, "Failed malloc: %s.", errno);
        return false;
    }
    query->node = node;

    struct iobuf qbuf = {0};
    if (!kad_rpc_query_ping(kctx, &qbuf, query)) {
        goto failed;
    }

    socklen_t addr_len = sizeof(struct sockaddr_storage);
    ssize_t slen = sendto(sock, qbuf.buf, qbuf.pos, 0, (struct sockaddr *)&node.addr, addr_len);
    if (slen < 0) {
        if (errno != EWOULDBLOCK) {
            log_perror(LOG_ERR, "Failed sendto: %s", errno);
        }
        goto failed;
    }
    log_debug("Sent %d bytes.", slen);
    iobuf_reset(&qbuf);

    list_append(&kctx->queries, &query->item);
    char *id = log_fmt_hex(LOG_DEBUG, query->msg.tx_id.bytes, KAD_RPC_MSG_TX_ID_LEN);
    log_debug("Query (tx_id=%s) saved.", id);
    free_safer(id);

    return true;

  failed:
    iobuf_reset(&qbuf);
    free_safer(query);
    return false;
}
