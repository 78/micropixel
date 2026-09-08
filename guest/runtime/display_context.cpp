#include "runtime/display_context.hpp"

#include "runtime/graphics_limits.hpp"

namespace micropixel::runtime {

namespace {
ServiceCache graphics_service;
GraphicsLimits cached_graphics_limits{};
bool graphics_limits_loaded{};
ServiceCache input_service;
micropixel_graphics_info_t cached_graphics_info{};
bool graphics_info_loaded{};
micropixel::detail::DisplayTransform cached_display_context{};
bool display_context_loaded{};
micropixel_input_info_t cached_input_info{};
bool input_info_loaded{};
}  // namespace

ServiceCache& GraphicsService() { return graphics_service; }

const micropixel_graphics_info_t& LoadPhysicalGraphicsInfo() {
    RequireOk(OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                          MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
              "graphics.open");
    if (!graphics_info_loaded) {
        uint32_t response_size = 0U;
        RequireOk(CallService(graphics_service, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U, &cached_graphics_info,
                              sizeof(cached_graphics_info), response_size),
                  "graphics.info");
        if (response_size < sizeof(cached_graphics_info) || cached_graphics_info.size < sizeof(cached_graphics_info) ||
            (cached_graphics_info.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
             cached_graphics_info.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) ||
            cached_graphics_info.width == 0U || cached_graphics_info.height == 0U ||
            static_cast<uint32_t>(cached_graphics_info.safe_inset_left) + cached_graphics_info.safe_inset_right >=
                cached_graphics_info.width ||
            static_cast<uint32_t>(cached_graphics_info.safe_inset_top) + cached_graphics_info.safe_inset_bottom >=
                cached_graphics_info.height ||
            cached_graphics_info.reserved0 != 0U || cached_graphics_info.max_surface_buffers == 0U ||
            cached_graphics_info.max_text_bytes == 0U ||
            cached_graphics_info.max_scene_bytes < sizeof(micropixel_graphics_scene_header_t)) {
            micropixel::runtime::Panic("graphics.info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        if (input_info_loaded && (cached_graphics_info.width != cached_input_info.logical_width ||
                                  cached_graphics_info.height != cached_input_info.logical_height)) {
            micropixel::runtime::Panic("graphics.info.coordinate_space", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        graphics_info_loaded = true;
    }
    return cached_graphics_info;
}

const GraphicsLimits& LoadGraphicsLimits() {
    if (!graphics_limits_loaded) {
        const micropixel_graphics_info_t& info = LoadPhysicalGraphicsInfo();
        const auto clamp = [](uint32_t host, uint32_t guest) { return host < guest ? host : guest; };
        cached_graphics_limits.max_text_bytes = clamp(info.max_text_bytes, limits::kMaxTextBytes);
        cached_graphics_limits.max_scene_bytes = clamp(info.max_scene_bytes, limits::kMaxSceneBytes);
        cached_graphics_limits.max_raster_bytes = clamp(info.max_raster_bytes, limits::kMaxRasterBytes);
        cached_graphics_limits.max_surface_buffers = clamp(info.max_surface_buffers, limits::kMaxSurfaceBuffers);
        graphics_limits_loaded = true;
    }
    return cached_graphics_limits;
}

const micropixel::detail::DisplayTransform& LoadDisplayContext() {
    const micropixel_graphics_info_t& physical = LoadPhysicalGraphicsInfo();
    if (!display_context_loaded) {
        cached_display_context = micropixel::detail::MakeDisplayTransform(physical.width, physical.height);
        if (cached_display_context.logical_width == 0U || cached_display_context.logical_height == 0U) {
            // A display with no usable dimensions cannot define the shared
            // logical coordinate space exposed through RendererInfo.
            micropixel::runtime::Panic("application.display.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        display_context_loaded = true;
    }
    return cached_display_context;
}

int32_t ScaleCoordinate(int32_t value, uint32_t numerator, uint32_t denominator) {
    return micropixel::detail::ScaleCoordinate(value, numerator, denominator);
}

micropixel::Point ToLogical(micropixel::Point point) {
    const auto& context = LoadDisplayContext();
    return {ScaleCoordinate(point.x - context.offset_x, context.logical_width, context.physical_width),
            ScaleCoordinate(point.y - context.offset_y, context.logical_height, context.physical_height)};
}

const micropixel_input_info_t& LoadInputInfo() {
    if (!input_info_loaded) {
        RequireOk(OpenService(input_service, MICROPIXEL_SERVICE_INPUT, MICROPIXEL_INPUT_INTERFACE_MAJOR, 0U),
                  "input.open");
        uint32_t response_size = 0U;
        RequireOk(CallService(input_service, MICROPIXEL_INPUT_METHOD_GET_INFO, nullptr, 0U, &cached_input_info,
                              sizeof(cached_input_info), response_size),
                  "input.info");
        if (response_size < sizeof(cached_input_info) || cached_input_info.size < sizeof(cached_input_info) ||
            cached_input_info.logical_width == 0U || cached_input_info.logical_height == 0U ||
            cached_input_info.max_touch_points == 0U) {
            micropixel::runtime::Panic("input.info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        if (graphics_info_loaded && (cached_input_info.logical_width != cached_graphics_info.width ||
                                     cached_input_info.logical_height != cached_graphics_info.height)) {
            micropixel::runtime::Panic("input.info.coordinate_space", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        input_info_loaded = true;
    }
    return cached_input_info;
}

}  // namespace micropixel::runtime

namespace micropixel::detail {

const DisplayTransform& CurrentDisplayTransform() { return runtime::LoadDisplayContext(); }

}  // namespace micropixel::detail
