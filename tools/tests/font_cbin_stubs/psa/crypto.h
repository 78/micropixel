#ifndef MICROPIXEL_TEST_FONT_CBIN_PSA_CRYPTO_H
#define MICROPIXEL_TEST_FONT_CBIN_PSA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009U)

static inline psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

static inline uint32_t micropixel_test_sha256_rotr(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

static inline uint32_t micropixel_test_sha256_load_be(const uint8_t* data) {
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) | ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static inline void micropixel_test_sha256_store_be(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static inline void micropixel_test_sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t round_constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    uint32_t schedule[64];
    for (uint32_t index = 0U; index < 16U; ++index) {
        schedule[index] = micropixel_test_sha256_load_be(block + index * 4U);
    }
    for (uint32_t index = 16U; index < 64U; ++index) {
        const uint32_t previous_15 = schedule[index - 15U];
        const uint32_t previous_2 = schedule[index - 2U];
        const uint32_t sigma0 = micropixel_test_sha256_rotr(previous_15, 7U) ^
                                micropixel_test_sha256_rotr(previous_15, 18U) ^ (previous_15 >> 3U);
        const uint32_t sigma1 = micropixel_test_sha256_rotr(previous_2, 17U) ^
                                micropixel_test_sha256_rotr(previous_2, 19U) ^ (previous_2 >> 10U);
        schedule[index] = schedule[index - 16U] + sigma0 + schedule[index - 7U] + sigma1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (uint32_t index = 0U; index < 64U; ++index) {
        const uint32_t sum1 = micropixel_test_sha256_rotr(e, 6U) ^ micropixel_test_sha256_rotr(e, 11U) ^
                              micropixel_test_sha256_rotr(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice + round_constants[index] + schedule[index];
        const uint32_t sum0 = micropixel_test_sha256_rotr(a, 2U) ^ micropixel_test_sha256_rotr(a, 13U) ^
                              micropixel_test_sha256_rotr(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static inline psa_status_t psa_hash_compute(psa_algorithm_t algorithm, const uint8_t* input, size_t input_length,
                                            uint8_t* hash, size_t hash_size, size_t* hash_length) {
    if (algorithm != PSA_ALG_SHA_256 || (input == NULL && input_length != 0U) || hash == NULL || hash_size < 32U ||
        hash_length == NULL) {
        return -1;
    }
    uint32_t state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    size_t offset = 0U;
    while (input_length - offset >= 64U) {
        micropixel_test_sha256_compress(state, input + offset);
        offset += 64U;
    }

    uint8_t tail[128] = {0U};
    const size_t remaining = input_length - offset;
    for (size_t index = 0U; index < remaining; ++index) {
        tail[index] = input[offset + index];
    }
    tail[remaining] = 0x80U;
    const size_t padded_size = remaining < 56U ? 64U : 128U;
    const uint64_t bit_length = (uint64_t)input_length * 8U;
    for (uint32_t index = 0U; index < 8U; ++index) {
        tail[padded_size - 1U - index] = (uint8_t)(bit_length >> (index * 8U));
    }
    micropixel_test_sha256_compress(state, tail);
    if (padded_size == 128U) {
        micropixel_test_sha256_compress(state, tail + 64U);
    }
    for (uint32_t index = 0U; index < 8U; ++index) {
        micropixel_test_sha256_store_be(hash + index * 4U, state[index]);
    }
    *hash_length = 32U;
    return PSA_SUCCESS;
}

#endif
