#ifndef MICROPIXEL_GUEST_RUNTIME_DISPLAY_CONTEXT_HPP
#define MICROPIXEL_GUEST_RUNTIME_DISPLAY_CONTEXT_HPP

#include "runtime/display_transform.hpp"
#include "runtime/graphics_limits.hpp"
#include "runtime/service_binding.hpp"
#include "sdk/geometry.hpp"
#include "sdk/result.hpp"

namespace micropixel::runtime {

// Graphics and input share a physical coordinate contract and lazy caches.
ServiceCache& GraphicsService();
const micropixel_graphics_info_t& LoadPhysicalGraphicsInfo();
const detail::DisplayTransform& LoadDisplayContext();
Result<void> ConfigureDisplayContext(const DisplayConfiguration& configuration);
// DirectSurface creation adopts the buffer size as the logical canvas when the
// App never called ConfigureDisplay and no Scene exists, so touch arrives in
// buffer pixels (Godot's viewport stretch, SDL3's logical presentation). An
// explicitly configured or Scene-committed canvas is left alone. Returns
// whether the canvas was adopted.
bool AdoptSurfaceCanvas(uint32_t buffer_width, uint32_t buffer_height);
// True while any Scene is alive; defined by the scene graph.
bool AnyLiveScene();
const micropixel_input_info_t& LoadInputInfo();
Point ToLogical(Point point);
int32_t ScaleCoordinate(int32_t value, uint32_t numerator, uint32_t denominator);

}  // namespace micropixel::runtime

#endif  // MICROPIXEL_GUEST_RUNTIME_DISPLAY_CONTEXT_HPP
