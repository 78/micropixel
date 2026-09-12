#ifndef MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLE_STORE_SOURCE_HPP
#define MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLE_STORE_SOURCE_HPP

#include "runtime/bundle/bundle_source.h"
#include "runtime/bundlefs/bundle_store.hpp"

namespace micropixel::runtime {

// Wraps one committed or staged store file as a storage-agnostic Bundle
// source. The source offers zero-copy mapping only when the store can map;
// otherwise readers copy the bytes they need into RAM. The store must outlive
// every source created from it.
[[nodiscard]] bool MakeBundleSource(BundleStore& store, const bundlefs_file_t& file,
                                    micropixel_bundle_source_t& source_out);

}  // namespace micropixel::runtime

#endif
