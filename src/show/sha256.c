/* SHA-256; see sha256.h. */
#include "sha256.h"
#include "show-fs.h"

#include <stdlib.h>
#include <string.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void transform(struct sha256_ctx *ctx, const unsigned char *b)
{
	uint32_t w[64], s[8];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) | ((uint32_t)b[i * 4 + 2] << 8) |
		       (uint32_t)b[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(s, ctx->state, sizeof(s));
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25);
		uint32_t ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
		uint32_t t1 = s[7] + S1 + ch + K[i] + w[i];
		uint32_t S0 = ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22);
		uint32_t maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
		uint32_t t2 = S0 + maj;
		s[7] = s[6];
		s[6] = s[5];
		s[5] = s[4];
		s[4] = s[3] + t1;
		s[3] = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++)
		ctx->state[i] += s[i];
}

void sha256_init(struct sha256_ctx *ctx)
{
	static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
					 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(ctx->state, init, sizeof(init));
	ctx->bytes = 0;
	ctx->used = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, size_t size)
{
	const unsigned char *p = data;
	ctx->bytes += size;
	while (size) {
		size_t n = 64 - ctx->used;
		if (n > size)
			n = size;
		memcpy(ctx->block + ctx->used, p, n);
		ctx->used += n;
		p += n;
		size -= n;
		if (ctx->used == 64) {
			transform(ctx, ctx->block);
			ctx->used = 0;
		}
	}
}

void sha256_final_hex(struct sha256_ctx *ctx, char hex[65])
{
	static const char digits[] = "0123456789abcdef";
	uint64_t bits = ctx->bytes * 8;
	unsigned char pad = 0x80;
	sha256_update(ctx, &pad, 1);
	pad = 0;
	while (ctx->used != 56)
		sha256_update(ctx, &pad, 1);
	unsigned char len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (unsigned char)(bits >> (56 - i * 8));
	sha256_update(ctx, len, 8);
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 4; j++) {
			unsigned char byte = (unsigned char)(ctx->state[i] >> (24 - j * 8));
			hex[i * 8 + j * 2] = digits[byte >> 4];
			hex[i * 8 + j * 2 + 1] = digits[byte & 15];
		}
	}
	hex[64] = '\0';
}

void sha256_hex(const void *data, size_t size, char hex[65])
{
	struct sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, size);
	sha256_final_hex(&ctx, hex);
}

bool sha256_file(const char *path, char hex[65], int64_t *size_out, int (*interrupt)(void *), void *opaque)
{
	FILE *f = sfs_fopen(path, "rb");
	if (!f)
		return false;
	const size_t chunk = 1 << 20;
	unsigned char *buf = malloc(chunk);
	struct sha256_ctx ctx;
	sha256_init(&ctx);
	bool ok = buf != NULL;
	while (ok) {
		size_t got = fread(buf, 1, chunk, f);
		if (got)
			sha256_update(&ctx, buf, got);
		if (got < chunk) {
			ok = !ferror(f);
			break;
		}
		if (interrupt && interrupt(opaque))
			ok = false;
	}
	free(buf);
	fclose(f);
	if (!ok)
		return false;
	if (size_out)
		*size_out = (int64_t)ctx.bytes;
	sha256_final_hex(&ctx, hex);
	return true;
}
