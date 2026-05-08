#include "nft.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <libnftnl/table.h>
#include <libnftnl/chain.h>
#include <libnftnl/rule.h>
#include <libnftnl/expr.h>
#include <libnftnl/set.h>
#include <libnftnl/batch.h>

struct nft_ctx {
    struct mnl_socket *nl;
    uint32_t seq;
    int family;
    struct nftnl_batch *batch;
};

nft_ctx_t* nft_create(int family) {
    nft_ctx_t *ctx = calloc(1, sizeof(nft_ctx_t));
    if (!ctx) return NULL;

    ctx->nl = mnl_socket_open(NETLINK_NETFILTER);
    if (!ctx->nl) {
        free(ctx);
        return NULL;
    }

    if (mnl_socket_bind(ctx->nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(ctx->nl);
        free(ctx);
        return NULL;
    }

    ctx->family = family;
    ctx->seq = 1;
    ctx->batch = nftnl_batch_alloc();
    if (!ctx->batch) {
        mnl_socket_close(ctx->nl);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void nft_destroy(nft_ctx_t *ctx) {
    if (ctx) {
        if (ctx->batch) nftnl_batch_free(ctx->batch);
        if (ctx->nl) mnl_socket_close(ctx->nl);
        free(ctx);
    }
}

int nft_begin(nft_ctx_t *ctx) {
    if (!ctx) return -1;

    nftnl_batch_begin(ctx->batch, NFTNL_BATCH_FRONT);

    struct nftnl_table *t = nftnl_table_alloc();
    nftnl_table_set_str(t, NFTNL_TABLE_NAME, "atp");
    nftnl_table_set_u32(t, NFTNL_TABLE_FAMILY, ctx->family);
    nftnl_batch_add(ctx->batch, NFTNL_BATCH_FRONT, t, NLM_F_CREATE, ctx->seq++);

    return 0;
}

int nft_add_chain(nft_ctx_t *ctx, const char *table, const char *chain, const char *hook, int priority) {
    if (!ctx || !chain) return -1;

    struct nftnl_chain *c = nftnl_chain_alloc();
    nftnl_chain_set_str(c, NFTNL_CHAIN_TABLE, table);
    nftnl_chain_set_str(c, NFTNL_CHAIN_NAME, chain);

    if (hook) {
        nftnl_chain_set_u32(c, NFTNL_CHAIN_FAMILY, ctx->family);
        nftnl_chain_set_str(c, NFTNL_CHAIN_TYPE, hook);
        nftnl_chain_set_s32(c, NFTNL_CHAIN_PRIO, priority);
        nftnl_chain_set_u32(c, NFTNL_CHAIN_HOOKNUM, 
            strcmp(hook, "prerouting") == 0 ? NF_INET_PRE_ROUTING :
            strcmp(hook, "input") == 0 ? NF_INET_LOCAL_IN :
            strcmp(hook, "forward") == 0 ? NF_INET_FORWARD :
            strcmp(hook, "output") == 0 ? NF_INET_LOCAL_OUT :
            NF_INET_POST_ROUTING);
    }

    nftnl_batch_add(ctx->batch, NFTNL_BATCH_FRONT, c, NLM_F_CREATE, ctx->seq++);
    return 0;
}

int nft_add_rule(nft_ctx_t *ctx, const char *table, const char *chain_name, const char *expr) {
    if (!ctx || !chain_name || !expr) return -1;

    struct nftnl_rule *r = nftnl_rule_alloc();
    nftnl_rule_set_str(r, NFTNL_RULE_TABLE, table);
    nftnl_rule_set_str(r, NFTNL_RULE_CHAIN, chain_name);
    nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, ctx->family);

    struct nftnl_expr *e = nftnl_expr_alloc("counter");
    if (e) {
        nftnl_rule_add_expr(r, e);
    }

    struct nftnl_expr *ve = nftnl_expr_alloc("verdict");
    if (ve) {
        char *verdict = strdup(expr);
        int drop = 0;

        if (strstr(verdict, "-j ACCEPT")) {
            nftnl_expr_set_u32(ve, NFTNL_EXPR_VERDICT_CODE, NF_ACCEPT);
        } else if (strstr(verdict, "-j DROP") || strstr(verdict, "-j REJECT")) {
            nftnl_expr_set_u32(ve, NFTNL_EXPR_VERDICT_CODE, NF_DROP);
        } else if (strstr(verdict, "-j RETURN")) {
            nftnl_expr_set_u32(ve, NFTNL_EXPR_VERDICT_CODE, NFT_RETURN);
        } else if (strstr(verdict, "-j ")) {
            char *target = strstr(verdict, "-j ") + 3;
            nftnl_expr_set_str(ve, NFTNL_EXPR_VERDICT_CHAIN, target);
            nftnl_expr_set_u32(ve, NFTNL_EXPR_VERDICT_CODE, NFT_JUMP);
        } else {
            drop = 1;
        }

        if (!drop) {
            nftnl_rule_add_expr(r, ve);
        } else {
            nftnl_expr_free(ve);
        }
        free(verdict);
    }

    nftnl_batch_add(ctx->batch, NFTNL_BATCH_FRONT, r, NLM_F_CREATE | NLM_F_APPEND, ctx->seq++);
    return 0;
}

int nft_commit(nft_ctx_t *ctx) {
    if (!ctx) return -1;

    nftnl_batch_end(ctx->batch, NFTNL_BATCH_FRONT);

    if (mnl_socket_sendto(ctx->nl, nftnl_batch_buffer(ctx->batch),
                          nftnl_batch_len(ctx->batch)) < 0) {
        LOG_ERROR("nft: batch send failed: %s", strerror(errno));
        return -1;
    }

    char buf[8192];
    ssize_t ret = mnl_socket_recvfrom(ctx->nl, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERROR("nft: batch recv failed: %s", strerror(errno));
        return -1;
    }

    LOG_INFO("nft: batch committed (%zd bytes)", ret);
    return 0;
}

int nft_flush_table(nft_ctx_t *ctx, const char *table) {
    if (!ctx) return -1;

    struct nftnl_table *t = nftnl_table_alloc();
    nftnl_table_set_str(t, NFTNL_TABLE_NAME, table);
    nftnl_table_set_u32(t, NFTNL_TABLE_FAMILY, ctx->family);

    nftnl_batch_begin(ctx->batch, NFTNL_BATCH_FRONT);
    nftnl_batch_add(ctx->batch, NFTNL_BATCH_FRONT, t, 0, ctx->seq++);
    nftnl_batch_end(ctx->batch, NFTNL_BATCH_FRONT);

    mnl_socket_sendto(ctx->nl, nftnl_batch_buffer(ctx->batch),
                      nftnl_batch_len(ctx->batch));

    char buf[8192];
    mnl_socket_recvfrom(ctx->nl, buf, sizeof(buf));

    return 0;
}
