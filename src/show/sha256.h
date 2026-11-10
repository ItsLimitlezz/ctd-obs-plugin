/* SHA-256 (FIPS 180-4) for Show Profiles: content addresses of bundle files and assets. Pure C, no libobs. */
#ifndef CTD_SHOW_SHA256_H
#define CTD_SHOW_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sha256_ctx {
	uint32_t state[8];
	uint64_t bytes;
	unsigned char block[64];
	size_t used;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t size);
/* Writes 64 lowercase hex digits + NUL. */
void sha256_final_hex(struct sha256_ctx *ctx, char hex[65]);

void sha256_hex(const void *data, size_t size, char hex[65]);
/* Hashes a file (UTF-8 path). size_out may be NULL. interrupt (may be NULL) is polled per 1 MiB; non-zero aborts. */
bool sha256_file(const char *path, char hex[65], int64_t *size_out, int (*interrupt)(void *), void *opaque);

#ifdef __cplusplus
}
#endif

#endif
