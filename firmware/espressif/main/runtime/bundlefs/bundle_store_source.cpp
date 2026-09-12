#include "runtime/bundlefs/bundle_store_source.hpp"

#include <cstring>

namespace micropixel::runtime {
namespace {

struct SourceState final {
    BundleStore* store{};
    bundlefs_file_t file{};
};

static_assert(sizeof(SourceState) <= sizeof(micropixel_bundle_source_t::state),
              "store reference and file handle must fit inside the Bundle source state");

SourceState StateOf(const micropixel_bundle_source_t* source) {
    SourceState state{};
    std::memcpy(&state, source->state, sizeof(state));
    return state;
}

bool SourceSize(const micropixel_bundle_source_t* source, uint32_t* size_out) {
    const SourceState state = StateOf(source);
    bundlefs_file_info_t info{};
    if (state.store == nullptr || state.store->GetFileInfo(state.file, info) != BUNDLEFS_OK) {
        return false;
    }
    *size_out = info.size;
    return true;
}

bool SourceRead(const micropixel_bundle_source_t* source, uint32_t offset, void* destination, uint32_t size) {
    const SourceState state = StateOf(source);
    return state.store != nullptr && state.store->Read(state.file, offset, destination, size) == BUNDLEFS_OK;
}

void SourceUnmap(micropixel_bundle_mapping_t* mapping) {
    if (mapping->base != nullptr && mapping->owner != nullptr) {
        // `owner` is the BundleStore that produced the mapping; stores outlive
        // every source and mapping handed out from them.
        auto* store = static_cast<BundleStore*>(const_cast<void*>(mapping->owner));
        bundlefs_mapping_t store_mapping{
            .data = mapping->data,
            .mapping = mapping->base,
            .size = mapping->size,
            .mapping_handle = mapping->handle,
        };
        store->Unmap(store_mapping);
    }
    std::memset(mapping, 0, sizeof(*mapping));
}

constexpr micropixel_bundle_mapping_ops_t kMappingOps{.unmap = SourceUnmap};

bool SourceMap(const micropixel_bundle_source_t* source, uint32_t offset, uint32_t size,
               micropixel_bundle_mapping_t* mapping_out) {
    const SourceState state = StateOf(source);
    bundlefs_mapping_t store_mapping{};
    if (state.store == nullptr || state.store->Map(state.file, offset, size, store_mapping) != BUNDLEFS_OK) {
        return false;
    }
    mapping_out->data = store_mapping.data;
    mapping_out->base = store_mapping.mapping;
    mapping_out->owner = state.store;
    mapping_out->size = store_mapping.size;
    mapping_out->handle = store_mapping.mapping_handle;
    mapping_out->ops = &kMappingOps;
    return true;
}

constexpr micropixel_bundle_source_ops_t kMappableSourceOps{.size = SourceSize, .read = SourceRead, .map = SourceMap};
constexpr micropixel_bundle_source_ops_t kReadOnlySourceOps{.size = SourceSize, .read = SourceRead, .map = nullptr};

}  // namespace

bool MakeBundleSource(BundleStore& store, const bundlefs_file_t& file, micropixel_bundle_source_t& source_out) {
    std::memset(&source_out, 0, sizeof(source_out));
    source_out.ops = store.mappable() ? &kMappableSourceOps : &kReadOnlySourceOps;
    const SourceState state{.store = &store, .file = file};
    std::memcpy(source_out.state, &state, sizeof(state));
    return true;
}

}  // namespace micropixel::runtime
