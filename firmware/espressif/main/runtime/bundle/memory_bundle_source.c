#include "runtime/bundle/memory_bundle_source.h"

#include <assert.h>
#include <string.h>

typedef struct memory_source_state {
    const uint8_t* data;
    uint32_t size;
} memory_source_state_t;

static_assert(sizeof(memory_source_state_t) <= sizeof(((micropixel_bundle_source_t*)0)->state),
              "memory source state must fit inside the Bundle source state");

static memory_source_state_t state_of(const micropixel_bundle_source_t* source) {
    memory_source_state_t state;
    memcpy(&state, source->state, sizeof(state));
    return state;
}

static bool memory_size(const micropixel_bundle_source_t* source, uint32_t* size_out) {
    *size_out = state_of(source).size;
    return true;
}

static bool memory_read(const micropixel_bundle_source_t* source, uint32_t offset, void* destination, uint32_t size) {
    const memory_source_state_t state = state_of(source);
    if (state.data == NULL || offset > state.size || size > state.size - offset) {
        return false;
    }
    memcpy(destination, state.data + offset, size);
    return true;
}

static void memory_unmap(micropixel_bundle_mapping_t* mapping) { memset(mapping, 0, sizeof(*mapping)); }

static const micropixel_bundle_mapping_ops_t kMemoryMappingOps = {.unmap = memory_unmap};

static bool memory_map(const micropixel_bundle_source_t* source, uint32_t offset, uint32_t size,
                       micropixel_bundle_mapping_t* mapping_out) {
    const memory_source_state_t state = state_of(source);
    if (state.data == NULL || size == 0U || offset > state.size || size > state.size - offset) {
        return false;
    }
    memset(mapping_out, 0, sizeof(*mapping_out));
    mapping_out->data = state.data + offset;
    mapping_out->base = state.data;
    mapping_out->size = size;
    mapping_out->ops = &kMemoryMappingOps;
    return true;
}

static const micropixel_bundle_source_ops_t kMemorySourceOps = {
    .size = memory_size,
    .read = memory_read,
    .map = memory_map,
};

bool micropixel_memory_bundle_source(const uint8_t* data, uint32_t size, micropixel_bundle_source_t* source_out) {
    if (data == NULL || size == 0U || source_out == NULL) {
        return false;
    }
    memset(source_out, 0, sizeof(*source_out));
    source_out->ops = &kMemorySourceOps;
    const memory_source_state_t state = {.data = data, .size = size};
    memcpy(source_out->state, &state, sizeof(state));
    return true;
}
