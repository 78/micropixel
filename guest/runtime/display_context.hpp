#ifndef MICROPIXEL_GUEST_RUNTIME_DISPLAY_CONTEXT_HPP
#define MICROPIXEL_GUEST_RUNTIME_DISPLAY_CONTEXT_HPP

#include "runtime/display_transform.hpp"
#include "runtime/graphics_limits.hpp"
#include "runtime/service_binding.hpp"
#include "sdk/geometry.hpp"

namespace micropixel::runtime {

// Graphics and input share a physical coordinate contract and lazy caches.
ServiceCache& GraphicsService();
const micropixel_graphics_info_t& LoadPhysicalGraphicsInfo();
const detail::DisplayTransform& LoadDisplayContext();
const micropixel_input_info_t& LoadInputInfo();
Point ToLogical(Point point);
int32_t ScaleCoordinate(int32_t value, uint32_t numerator, uint32_t denominator);

}  // namespace micropixel::runtime

#endif  // MICROPIXEL_GUEST_RUNTIME_DISPLAY_CONTEXT_HPP
