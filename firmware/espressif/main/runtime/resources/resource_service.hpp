#ifndef MICROPIXEL_RUNTIME_RESOURCES_RESOURCE_SERVICE_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_RESOURCE_SERVICE_HPP

#include <atomic>
#include <cstdint>

#include "device/graphics.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/resources/bitmap_store.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::runtime {

class ResourceService final {
   public:
    ResourceService(const micropixel_aot_package_t& package, work::BackgroundExecutor& background_executor);
    ResourceService(const ResourceService&) = delete;
    ResourceService& operator=(const ResourceService&) = delete;
    ~ResourceService();

    [[nodiscard]] bool valid() const;  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> LoadTexture(uint32_t asset_id);
    [[nodiscard]] ServiceResult<void> ReleaseTexture(micropixel_texture_handle_t texture);
    [[nodiscard]] ServiceResult<device::FontResourceView> FindFont(uint32_t resource_id) const;
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> CreateStreamingTexture(uint32_t width, uint32_t height,
                                                                                  uint32_t pixel_format);
    [[nodiscard]] ServiceResult<device::BitmapView> MutableTexture(micropixel_texture_handle_t texture) const;
    [[nodiscard]] bool ResolveTexture(micropixel_texture_handle_t texture, device::BitmapView& view_out) const;
    [[nodiscard]] bool RetainSceneTexture(micropixel_texture_handle_t texture);
    void ReleaseSceneTexture(micropixel_texture_handle_t texture);
    void Shutdown();

   private:
    struct Work final {
        ResourceService* service{};
        micropixel_bundle_asset_view_t asset{};
    };

    static void ProcessEntry(void* argument);
    void Process(const Work& work);
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> AddAsset(const micropixel_bundle_asset_view_t& asset);
    [[nodiscard]] micropixel_texture_info_t TextureInfo(micropixel_texture_handle_t texture,
                                                        const device::BitmapView& view) const;

    // AotPackage owns the mapping for the complete AppSession.
    micropixel_aot_package_t package_{};
    work::BackgroundExecutor& background_executor_;
    SemaphoreHandle_t work_done_{};
    micropixel_texture_handle_t completed_texture_{};
    int32_t completed_status_{MICROPIXEL_STATUS_INTERNAL};
    BitmapStore bitmaps_;
    std::atomic<bool> stopping_{};
    bool shutdown_complete_{};
};

}  // namespace micropixel::runtime

#endif
