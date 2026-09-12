#ifndef MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_SOURCE_H
#define MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A Bundle source is a read-only view of one immutable Bundle file. It hides
 * where the bytes live (BundleFS on NOR, a future NAND store, a LittleFS or
 * FAT directory) so the Bundle reader and everything above it only depend on
 * this contract.
 *
 * Sources are plain value types: the ops table is shared and immutable, and
 * all per-file state lives inline in `state`, so callers copy sources freely
 * and never free them. Mapping is optional. A source whose bytes cannot be
 * mapped into the CPU address space leaves `map` NULL; readers must then fall
 * back to `read`.
 */

/* Room for a store reference plus one opaque store file handle. */
#define MICROPIXEL_BUNDLE_SOURCE_STATE_WORDS 92U

typedef struct micropixel_bundle_source micropixel_bundle_source_t;
typedef struct micropixel_bundle_mapping micropixel_bundle_mapping_t;

typedef struct micropixel_bundle_mapping_ops {
    /* Releases `mapping` and clears it. Must tolerate an already-cleared mapping. */
    void (*unmap)(micropixel_bundle_mapping_t* mapping);
} micropixel_bundle_mapping_ops_t;

/*
 * One mapped range of a Bundle. `data` addresses exactly the requested bytes
 * and stays valid until `micropixel_bundle_mapping_release`. `base`, `owner`
 * and `handle` are private to the source that created the mapping.
 */
struct micropixel_bundle_mapping {
    const uint8_t* data;
    const void* base;
    const void* owner;
    uint32_t size;
    uint32_t handle;
    const micropixel_bundle_mapping_ops_t* ops;
};

typedef struct micropixel_bundle_source_ops {
    /* Logical file size in bytes. */
    bool (*size)(const micropixel_bundle_source_t* source, uint32_t* size_out);
    /* Copies `size` bytes at `offset` into `destination`; the range must lie inside the file. */
    bool (*read)(const micropixel_bundle_source_t* source, uint32_t offset, void* destination, uint32_t size);
    /* Optional zero-copy view of `[offset, offset + size)`. NULL when the storage cannot be mapped. */
    bool (*map)(const micropixel_bundle_source_t* source, uint32_t offset, uint32_t size,
                micropixel_bundle_mapping_t* mapping_out);
} micropixel_bundle_source_ops_t;

struct micropixel_bundle_source {
    const micropixel_bundle_source_ops_t* ops;
    uint32_t state[MICROPIXEL_BUNDLE_SOURCE_STATE_WORDS];
};

static inline bool micropixel_bundle_source_valid(const micropixel_bundle_source_t* source) {
    return source != NULL && source->ops != NULL && source->ops->size != NULL && source->ops->read != NULL;
}

static inline bool micropixel_bundle_source_size(const micropixel_bundle_source_t* source, uint32_t* size_out) {
    return micropixel_bundle_source_valid(source) && size_out != NULL && source->ops->size(source, size_out);
}

static inline bool micropixel_bundle_source_read(const micropixel_bundle_source_t* source, uint32_t offset,
                                                 void* destination, uint32_t size) {
    return micropixel_bundle_source_valid(source) && destination != NULL &&
           source->ops->read(source, offset, destination, size);
}

static inline bool micropixel_bundle_source_can_map(const micropixel_bundle_source_t* source) {
    return micropixel_bundle_source_valid(source) && source->ops->map != NULL;
}

/* Fails when the source cannot map; callers that need bytes regardless should use `read`. */
static inline bool micropixel_bundle_source_map(const micropixel_bundle_source_t* source, uint32_t offset,
                                                uint32_t size, micropixel_bundle_mapping_t* mapping_out) {
    return micropixel_bundle_source_can_map(source) && mapping_out != NULL &&
           source->ops->map(source, offset, size, mapping_out);
}

static inline void micropixel_bundle_mapping_release(micropixel_bundle_mapping_t* mapping) {
    if (mapping != NULL && mapping->ops != NULL && mapping->ops->unmap != NULL) {
        mapping->ops->unmap(mapping);
    }
}

#ifdef __cplusplus
}
#endif

#endif
