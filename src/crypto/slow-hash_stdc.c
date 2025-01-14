// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "int-util.h"
#include "crypto/hash-impl.h"
#include "crypto/oaes/oaes_lib.h"
#ifdef __APPLE__
#include "TargetConditionals.h"
#endif

static void (*const extra_hashes[4])(const void *, size_t, unsigned char *) = {
    hash_extra_blake, hash_extra_groestl, hash_extra_jh, hash_extra_skein};

#define MEMORY (1 << 21) /* 2 MiB */
#define ITER (1 << 20)
#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE 32 /*16*/
#define INIT_SIZE_BLK 8
#define INIT_SIZE_BYTE (INIT_SIZE_BLK * AES_BLOCK_SIZE)

static size_t e2i(const uint8_t *a, size_t count)
{
    return (*((uint64_t *)a) / AES_BLOCK_SIZE) & (count - 1);
}

static void mul(const uint8_t *a, const uint8_t *b, uint8_t *res)
{
    uint64_t a0, b0;
    uint64_t hi, lo;

    a0 = SWAP64LE(((uint64_t *)a)[0]);
    b0 = SWAP64LE(((uint64_t *)b)[0]);
    lo = mul128(a0, b0, &hi);
    ((uint64_t *)res)[0] = SWAP64LE(hi);
    ((uint64_t *)res)[1] = SWAP64LE(lo);
}

static void sum_half_blocks(uint8_t *a, const uint8_t *b)
{
    uint64_t a0, a1, b0, b1;

    a0 = SWAP64LE(((uint64_t *)a)[0]);
    a1 = SWAP64LE(((uint64_t *)a)[1]);
    b0 = SWAP64LE(((uint64_t *)b)[0]);
    b1 = SWAP64LE(((uint64_t *)b)[1]);
    a0 += b0;
    a1 += b1;
    ((uint64_t *)a)[0] = SWAP64LE(a0);
    ((uint64_t *)a)[1] = SWAP64LE(a1);
}

static void copy_block(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, AES_BLOCK_SIZE);
}

static void swap_blocks(uint8_t *a, uint8_t *b)
{
    size_t i;
    uint8_t t;
    for (i = 0; i < AES_BLOCK_SIZE; i++)
    {
        t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

static void xor_blocks(uint8_t *a, const uint8_t *b)
{
    size_t i;
    for (i = 0; i < AES_BLOCK_SIZE; i++)
    {
        a[i] ^= b[i];
    }
}

#pragma pack(push, 1)
union cn_slow_hash_state
{
    union hash_state hs;
    struct
    {
        uint8_t k[64];
        uint8_t init[INIT_SIZE_BYTE];
    };
};
#pragma pack(pop)

static void cn_slow_hash_platform_independent(void *scratchpad, const void *data, size_t length, void *hash)
{
    uint8_t *long_state = (uint8_t *)scratchpad;
    union cn_slow_hash_state state;
    uint8_t text[INIT_SIZE_BYTE];
    uint8_t a[AES_BLOCK_SIZE];
    uint8_t b[AES_BLOCK_SIZE];
    uint8_t c[AES_BLOCK_SIZE];
    uint8_t d[AES_BLOCK_SIZE];
    size_t i, j;
    uint8_t aes_key[AES_KEY_SIZE];
    OAES_CTX *aes_ctx;

    hash_process(&state.hs, data, length);
    memcpy(text, state.init, INIT_SIZE_BYTE);
    memcpy(aes_key, state.hs.b, AES_KEY_SIZE);
    aes_ctx = oaes_alloc();

    oaes_key_import_data(aes_ctx, aes_key, AES_KEY_SIZE);
    for (i = 0; i < MEMORY / INIT_SIZE_BYTE; i++)
    {
        for (j = 0; j < INIT_SIZE_BLK; j++)
            oaes_pseudo_encrypt_ecb(aes_ctx, &text[AES_BLOCK_SIZE * j]);

        memcpy(&long_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
    }

    for (i = 0; i < 16; i++)
    {
        a[i] = state.k[i] ^ state.k[32 + i];
        b[i] = state.k[16 + i] ^ state.k[48 + i];
    }

    for (i = 0; i < ITER / 2; i++)
    {
        /* Dependency chain: address -> read value ------+
		 * written value <-+ hard function (AES or MUL) <+
		 * next address  <-+
		 */
        /* Iteration 1 */
        j = e2i(a, MEMORY / AES_BLOCK_SIZE);
        copy_block(c, &long_state[j * AES_BLOCK_SIZE]);
        oaes_encryption_round(a, c);
        xor_blocks(b, c);
        swap_blocks(b, c);
        copy_block(&long_state[j * AES_BLOCK_SIZE], c);
        assert(j == e2i(a, MEMORY / AES_BLOCK_SIZE));
        swap_blocks(a, b);
        /* Iteration 2 */
        j = e2i(a, MEMORY / AES_BLOCK_SIZE);
        copy_block(c, &long_state[j * AES_BLOCK_SIZE]);
        mul(a, c, d);
        sum_half_blocks(b, d);
        swap_blocks(b, c);
        xor_blocks(b, c);
        copy_block(&long_state[j * AES_BLOCK_SIZE], c);
        assert(j == e2i(a, MEMORY / AES_BLOCK_SIZE));
        swap_blocks(a, b);
    }

    memcpy(text, state.init, INIT_SIZE_BYTE);
    oaes_key_import_data(aes_ctx, &state.hs.b[32], AES_KEY_SIZE);
    for (i = 0; i < MEMORY / INIT_SIZE_BYTE; i++)
    {
        for (j = 0; j < INIT_SIZE_BLK; j++)
        {
            xor_blocks(&text[j * AES_BLOCK_SIZE], &long_state[i * INIT_SIZE_BYTE + j * AES_BLOCK_SIZE]);
            oaes_pseudo_encrypt_ecb(aes_ctx, &text[j * AES_BLOCK_SIZE]);
        }
    }
    memcpy(state.init, text, INIT_SIZE_BYTE);
    hash_permutation(&state.hs);
    extra_hashes[state.hs.b[0] & 3](&state, 200, hash);
    oaes_free(&aes_ctx);
}

#if TARGET_OS_IPHONE || defined(__ANDROID__) // We need if !x86, but no portable way to express that
void cn_slow_hash_lite_v1(void *scratchpad, const void *data, size_t length, void *hash)
{
    cn_slow_hash_platform_independent(scratchpad, data, length, hash);
}
#endif // TARGET_OS_IPHONE