#ifndef MICROPIXEL_RUNTIME_BUNDLE_MEMORY_BUNDLE_SOURCE_H
#define MICROPIXEL_RUNTIME_BUNDLE_MEMORY_BUNDLE_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/bundle/bundle_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wraps a complete Bundle that already lives in RAM (an install payload, a
 * test fixture) as a mappable Bundle source. The caller keeps `data` alive
 * and unchanged for as long as the source or any mapping from it is in use.
 */
bool micropixel_memory_bundle_source(const uint8_t* data, uint32_t size, micropixel_bundle_source_t* source_out);

#ifdef __cplusplus
}
#endif

#endif
