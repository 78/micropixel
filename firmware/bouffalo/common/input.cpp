// SPDX-License-Identifier: Apache-2.0
#include "input.hpp"

#include <mutex>

extern "C" {
#include "bflb_mtimer.h"
#include "gt911.h"
#include "lcd.h"
int mpx_gt911_read(uint8_t*, touch_coord_t*, uint8_t*, uint8_t);
}
namespace mpx_port {
namespace d = micropixel::device;
static touch_coord_t touch_range{};
bool Input::Initialize() {
    hardware_ = gt911_i2c_init(&touch_range) == 0 && touch_range.coord_x && touch_range.coord_y;
    return hardware_;
}
int32_t Input::GetInfo(micropixel_input_info_t& i) {
    i = {};
    i.size = sizeof(i);
    i.max_touch_points = 5;
    i.logical_width = LCD_W;
    i.logical_height = LCD_H;
    i.capabilities = MICROPIXEL_INPUT_CAP_KEY_EVENTS;
    return MICROPIXEL_STATUS_OK;
}
void Input::BindTouchSink(d::TouchSink s, void* c) {
    std::lock_guard l(mutex_);
    touch_ = s;
    touch_context_ = c;
}
void Input::UnbindTouchSink(void* c) {
    std::lock_guard l(mutex_);
    if (touch_context_ == c) {
        touch_ = nullptr;
        touch_context_ = nullptr;
    }
}
void Input::BindKeySink(d::KeySink s, void* c) {
    std::lock_guard l(mutex_);
    key_ = s;
    key_context_ = c;
}
void Input::UnbindKeySink(void* c) {
    std::lock_guard l(mutex_);
    if (key_context_ == c) {
        key_ = nullptr;
        key_context_ = nullptr;
    }
}
void Input::SetActivitySink(d::InputActivitySink s, void* c) {
    std::lock_guard l(mutex_);
    activity_ = s;
    activity_context_ = c;
}
void Input::BindHostTouch(d::TouchSink sink, void* context) {
    std::lock_guard l(mutex_);
    host_touch_ = sink;
    host_context_ = context;
}
bool Input::InjectTouch(const d::TouchSample& s) {
    std::lock_guard l(mutex_);
    if (s.x < 0 || s.y < 0 || s.x >= LCD_W || s.y >= LCD_H) return false;
    if (host_touch_ && host_touch_(host_context_, s)) return true;
    if (activity_) activity_(activity_context_);
    return touch_ && touch_(touch_context_, s);
}
bool Input::InjectKey(const d::KeySample& s) {
    std::lock_guard l(mutex_);
    if (activity_) activity_(activity_context_);
    return key_ && key_(key_context_, s);
}
void Input::Poll() {
    if (!hardware_) return;
    touch_coord_t p[5];
    uint8_t ids[5], n{};
    int e = mpx_gt911_read(&n, p, ids, 5);
    if (e <= 0) return;
    bool now[16]{};
    uint64_t ts = bflb_mtimer_get_time_us();
    for (uint8_t j = 0; j < n; ++j) {
        uint8_t id = ids[j];
        if (id >= 16 || now[id]) continue;
        now[id] = true;
        int32_t x = (uint32_t)p[j].coord_x * LCD_W / touch_range.coord_x,
                y = (uint32_t)p[j].coord_y * LCD_H / touch_range.coord_y;
        if (x >= LCD_W) x = LCD_W - 1;
        if (y >= LCD_H) y = LCD_H - 1;
        d::TouchSample s{ts, id, x, y, 0, active_[id] ? d::TouchPhase::kMove : d::TouchPhase::kDown};
        (void)InjectTouch(s);
        x_[id] = x;
        y_[id] = y;
    }
    for (uint32_t id = 0; id < 16; ++id) {
        if (active_[id] && !now[id]) (void)InjectTouch({ts, id, x_[id], y_[id], 0, d::TouchPhase::kUp});
        active_[id] = now[id];
    }
}
}  // namespace mpx_port
