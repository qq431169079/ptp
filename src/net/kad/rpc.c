/* Copyright (c) 2017 Foudil Brétel.  All rights reserved. */
#include <errno.h>
#include "log.h"
#include "net/kad/bencode.h"
#include "utils/safer.h"
#include "net/kad/rpc.h"

bool kad_rpc_init(struct kad_ctx *ctx)
{
    ctx->dht = dht_init();
    if (!ctx->dht) {
        log_error("Could not initialize dht.");
        return false;
    }
    list_init(&ctx->queries);
    log_debug("DHT initialized.");
    return true;
}

void kad_rpc_terminate(struct kad_ctx *ctx)
{
    dht_terminate(ctx->dht);
    struct list_item *query = &ctx->queries;
    list_free_all(query, struct kad_rpc_msg, item);
    log_debug("DHT terminated.");
}

bool kad_rpc_msg_validate(const struct kad_rpc_msg *msg)
{
    (void)msg; // FIXME:
    return true;  /* TODO: should we validate beforehand ? */
}

struct kad_rpc_msg *
kad_rpc_query_find(struct kad_ctx *ctx, const unsigned char tx_id[])
{
    struct kad_rpc_msg *m;
    struct list_item * it = &ctx->queries;
    list_for(it, &ctx->queries) {
        m = cont(it, struct kad_rpc_msg, item);
        for (int i = 0; i < KAD_RPC_MSG_TX_ID_LEN; ++i) {
            if (m->tx_id[i] != tx_id[i])
                continue;
            break;
        }
    }

    if (it == &ctx->queries) {
        log_warning("Query (tx_id=TODO:) not found.");
        return NULL;
    }

    return m;
}

static void
kad_rpc_update_dht(struct kad_ctx *ctx, const char host[], const char service[],
                   const struct kad_rpc_msg *msg)
{
    char *id = log_fmt_hex(LOG_DEBUG, msg->node_id.b, KAD_GUID_SPACE_IN_BYTES);
    struct kad_node_info info = {0};
    info.id = msg->node_id;
    strcpy(info.host, host);
    strcpy(info.service, service);
    int updated = dht_update(ctx->dht, &info);
    if (updated == 0)
        log_debug("DHT update of [%s]:%s (id=%s).", host, service, id);
    else if (updated > 0) { // insert needed
        if (dht_insert(ctx->dht, &info))
            log_debug("DHT insert of [%s]:%s (id=%s).", host, service, id);
        else
            log_warning("Failed to insert kad_node (id=%s).", id);
    }
    else
        log_warning("Failed to update kad_node (id=%s)", id);
    free_safer(id);
}

static bool kad_rpc_handle_error(const struct kad_rpc_msg *msg)
{
    log_error("Received error message (%zull) from id(TODO:): %s.",
              msg->err_code, msg->err_msg);
    return true;
}

static bool
kad_rpc_handle_query(struct kad_ctx *ctx, const struct kad_rpc_msg *msg,
                     struct iobuf *rsp)
{
    switch (msg->meth) {
    case KAD_RPC_METH_NONE: {
        log_error("Got query for method none.");
        return false;
    }

    case KAD_RPC_METH_PING: {
        struct kad_rpc_msg resp = {0};
        memcpy(&resp.tx_id, &msg->tx_id, KAD_RPC_MSG_TX_ID_LEN);
        resp.node_id = ctx->dht->self_id;
        resp.type = KAD_RPC_TYPE_RESPONSE;
        resp.meth = KAD_RPC_METH_PING;
        if (!benc_encode(&resp, rsp)) {
            log_error("Error while encoding ping response.");
            return false;
        }
    }

    default:
        break;
    }
    return true;
}

static bool
kad_rpc_handle_response(struct kad_ctx *ctx, const struct kad_rpc_msg *msg)
{
    struct kad_rpc_msg *query = kad_rpc_query_find(ctx, msg->tx_id);
    if (!query) {
        log_error("Query for response id(TODO:) not found.");
        return false;
    }

    list_delete(&query->item);
    free_safer(query);
    return true;
}

static void kad_rpc_generate_tx_id(unsigned char tx_id[])
{
    for (int i = 0; i < KAD_RPC_MSG_TX_ID_LEN; i++)
        tx_id[i] = (unsigned char)random();
}

/**
 * Processes the incoming message in `buf` and places the response, if any,
 * into the provided `rsp` buffer.
 *
 * Returns 0 on success, -1 on failure, 1 if a response is ready.
 */
int kad_rpc_handle(struct kad_ctx *ctx, const char host[], const char service[],
                   const char buf[], const size_t slen, struct iobuf *rsp)
{
    int ret = 0;

    struct kad_rpc_msg *msg = malloc(sizeof(struct kad_rpc_msg));
    if (!msg) {
        log_perror(LOG_ERR, "Failed malloc: %s.", errno);
        return -1;
    }
    memset(msg, 0, sizeof(*msg));
    list_init(&(msg->item));

    if (!benc_decode(msg, buf, slen) || !kad_rpc_msg_validate(msg)) {
        log_error("Invalid message.");
        struct kad_rpc_msg rspmsg = {0};
        if (rspmsg.tx_id)  // just consider 0x0 as a reserved value
            memcpy(&rspmsg.tx_id, &msg->tx_id, KAD_RPC_MSG_TX_ID_LEN);
        else
            kad_rpc_generate_tx_id(rspmsg.tx_id); // TODO: track this tx ?
        rspmsg.node_id = ctx->dht->self_id;
        rspmsg.type = KAD_RPC_TYPE_ERROR;
        rspmsg.err_code = KAD_RPC_ERR_PROTOCOL;
        strcpy(rspmsg.err_msg, lookup_by_id(kad_rpc_err_names,
                                            KAD_RPC_ERR_PROTOCOL));
        if (!benc_encode(&rspmsg, rsp)) {
            log_error("Error while encoding error response.");
            ret = -1;
        }
        else
            ret = 1;
        goto end;
    }
    kad_rpc_msg_log(msg); // TESTING

    kad_rpc_update_dht(ctx, host, service, msg);

    switch (msg->type) {
    case KAD_RPC_TYPE_NONE: {
        log_error("Got msg of type none.");
        ret = -1;
        break;
    }

    case KAD_RPC_TYPE_ERROR: {
        if (!kad_rpc_handle_error(msg))
            ret = -1;
        break;
    }

    case KAD_RPC_TYPE_QUERY: {  /* We'll respond immediately */
        ret = kad_rpc_handle_query(ctx, msg, rsp) ? 1 : -1;
        break;
    }

    case KAD_RPC_TYPE_RESPONSE: {
        if (!kad_rpc_handle_response(ctx, msg))
            ret = -1;
        break;
    }

    default:
        log_error("Unknown msg type.");
        break;
    }

  end:
    free_safer(msg);
    return ret;
}

/**
 * For debugging only !
 */
void kad_rpc_msg_log(const struct kad_rpc_msg *msg)
{
    char *tx_id = log_fmt_hex(LOG_DEBUG, msg->tx_id, KAD_RPC_MSG_TX_ID_LEN);
    char *node_id = log_fmt_hex(LOG_DEBUG, msg->node_id.b, KAD_GUID_SPACE_IN_BYTES);
    log_debug(
        "msg={\n  tx_id=0x%s\n  node_id=0x%s\n  type=%d\n  err_code=%lld\n"
        "  err_msg=%s\n  meth=%d",
        tx_id, node_id, msg->type, msg->err_code, msg->err_msg,
        msg->meth, msg->target);
    free_safer(tx_id);
    free_safer(node_id);

    node_id = log_fmt_hex(LOG_DEBUG, msg->target.b,
                          *msg->target.b ? KAD_GUID_SPACE_IN_BYTES : 0);
    log_debug("  target=0x%s", node_id);
    free_safer(node_id);

    for (size_t i = 0; i < msg->nodes_len; i++) {
        node_id = log_fmt_hex(LOG_DEBUG, msg->nodes[i].id.b, KAD_GUID_SPACE_IN_BYTES);
        log_debug("  nodes[%zu]=0x%s:[%s]:%s", i, node_id,
                  msg->nodes[i].host, msg->nodes[i].service);
        free_safer(node_id);
    }
    log_debug("}");
}
