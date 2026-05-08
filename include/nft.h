#ifndef ATP_NFT_H
#define ATP_NFT_H

#include <stdint.h>
#include <stddef.h>

typedef struct nft_ctx nft_ctx_t;

nft_ctx_t* nft_create(int family);
void nft_destroy(nft_ctx_t *ctx);

int nft_begin(nft_ctx_t *ctx);
int nft_add_chain(nft_ctx_t *ctx, const char *table, const char *chain, const char *hook, int priority);
int nft_add_rule(nft_ctx_t *ctx, const char *table, const char *chain, const char *expr);
int nft_add_set(nft_ctx_t *ctx, const char *table, const char *set_name, int family, int size);
int nft_commit(nft_ctx_t *ctx);
int nft_flush_table(nft_ctx_t *ctx, const char *table);

#endif
