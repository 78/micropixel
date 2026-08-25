#ifndef MICROPIXEL_TEST_STUB_PSA_CRYPTO_H
#define MICROPIXEL_TEST_STUB_PSA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 0x02000009U

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;

typedef struct {
    uint32_t hash;
    uint64_t size;
    uint8_t active;
} psa_hash_operation_t;

#define PSA_HASH_OPERATION_INIT {2166136261U, 0U, 0U}

static inline psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

static inline psa_status_t psa_hash_setup(psa_hash_operation_t* operation, psa_algorithm_t algorithm) {
    if (operation == NULL || algorithm != PSA_ALG_SHA_256) {
        return -1;
    }
    operation->hash = 2166136261U;
    operation->size = 0U;
    operation->active = 1U;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_update(psa_hash_operation_t* operation, const uint8_t* input, size_t input_length) {
    if (operation == NULL || operation->active == 0U || (input == NULL && input_length != 0U)) {
        return -1;
    }
    for (size_t index = 0U; index < input_length; ++index) {
        operation->hash ^= input[index];
        operation->hash *= 16777619U;
    }
    operation->size += input_length;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_finish(psa_hash_operation_t* operation, uint8_t* hash, size_t hash_size,
                                           size_t* hash_length) {
    if (operation == NULL || operation->active == 0U || hash == NULL || hash_size < 32U || hash_length == NULL) {
        return -1;
    }
    uint32_t value = operation->hash ^ (uint32_t)operation->size ^ (uint32_t)(operation->size >> 32U);
    for (size_t index = 0U; index < 32U; ++index) {
        value ^= value << 13U;
        value ^= value >> 17U;
        value ^= value << 5U;
        hash[index] = (uint8_t)(value >> ((index & 3U) * 8U));
    }
    *hash_length = 32U;
    operation->active = 0U;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_abort(psa_hash_operation_t* operation) {
    if (operation != NULL) {
        operation->active = 0U;
    }
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_compute(psa_algorithm_t algorithm, const uint8_t* input, size_t input_length,
                                            uint8_t* hash, size_t hash_size, size_t* hash_length) {
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, algorithm) != PSA_SUCCESS ||
        psa_hash_update(&operation, input, input_length) != PSA_SUCCESS) {
        return -1;
    }
    return psa_hash_finish(&operation, hash, hash_size, hash_length);
}

#endif
