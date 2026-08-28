#include "platform/lvgl/display/ppa_srm_blitter.hpp"

#include <limits>

namespace micropixel::platform::lvgl {
namespace {

bool ValidRegion(const PpaSrmRect& region, uint32_t width, uint32_t height) {
    return region.width > 0U && region.height > 0U && region.x < width && region.y < height &&
           region.width <= width - region.x && region.height <= height - region.y;
}

}  // namespace

PpaSrmBlitter::~PpaSrmBlitter() { Release(); }

esp_err_t PpaSrmBlitter::Initialize(uint32_t max_pending_transactions) {
    if (max_pending_transactions == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client_ != nullptr) {
        return ESP_OK;
    }
    ppa_client_config_t config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = max_pending_transactions,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        .flags = {},
    };
    return ppa_register_client(&config, &client_);
}

esp_err_t PpaSrmBlitter::Blit(const PpaSrmBlit& request) const {
    if (client_ == nullptr || request.source == nullptr || request.destination == nullptr ||
        request.source_width == 0U || request.source_height == 0U || request.destination_width == 0U ||
        request.destination_height == 0U || request.destination_allocation_bytes == 0U ||
        !ValidRegion(request.source_region, request.source_width, request.source_height) ||
        request.destination_x >= request.destination_width || request.destination_y >= request.destination_height ||
        request.scale_x <= 0.0F || request.scale_y <= 0.0F) {
        return ESP_ERR_INVALID_ARG;
    }

    const double scaled_width = static_cast<double>(request.source_region.width) * request.scale_x;
    const double scaled_height = static_cast<double>(request.source_region.height) * request.scale_y;
    if (scaled_width > std::numeric_limits<uint32_t>::max() || scaled_height > std::numeric_limits<uint32_t>::max()) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t output_width = static_cast<uint32_t>(scaled_width);
    const uint32_t output_height = static_cast<uint32_t>(scaled_height);
    if (output_width == 0U || output_height == 0U || output_width > request.destination_width - request.destination_x ||
        output_height > request.destination_height - request.destination_y) {
        return ESP_ERR_INVALID_SIZE;
    }

    ppa_srm_oper_config_t config{};
    config.in.buffer = request.source;
    config.in.pic_w = request.source_width;
    config.in.pic_h = request.source_height;
    config.in.block_offset_x = request.source_region.x;
    config.in.block_offset_y = request.source_region.y;
    config.in.block_w = request.source_region.width;
    config.in.block_h = request.source_region.height;
    config.in.srm_cm = request.source_mode;
    config.out.buffer = request.destination;
    config.out.buffer_size = request.destination_allocation_bytes;
    config.out.pic_w = request.destination_width;
    config.out.pic_h = request.destination_height;
    config.out.block_offset_x = request.destination_x;
    config.out.block_offset_y = request.destination_y;
    config.out.srm_cm = request.destination_mode;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    config.scale_x = request.scale_x;
    config.scale_y = request.scale_y;
    config.byte_swap = request.input_byte_swap;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(client_, &config);
}

esp_err_t PpaSrmBlitter::SwapRgb565Bytes(const void* source, void* destination, uint32_t width, uint32_t height,
                                         uint32_t destination_allocation_bytes) const {
    if (source == destination) {
        return ESP_ERR_INVALID_ARG;
    }
    return Blit({
        .source = source,
        .source_width = width,
        .source_height = height,
        .source_region = {.x = 0U, .y = 0U, .width = width, .height = height},
        .source_mode = PPA_SRM_COLOR_MODE_RGB565,
        .destination = destination,
        .destination_width = width,
        .destination_height = height,
        .destination_allocation_bytes = destination_allocation_bytes,
        .destination_x = 0U,
        .destination_y = 0U,
        .destination_mode = PPA_SRM_COLOR_MODE_RGB565,
        .scale_x = 1.0F,
        .scale_y = 1.0F,
        .input_byte_swap = true,
    });
}

void PpaSrmBlitter::Release() {
    if (client_ != nullptr) {
        (void)ppa_unregister_client(client_);
        client_ = nullptr;
    }
}

}  // namespace micropixel::platform::lvgl
