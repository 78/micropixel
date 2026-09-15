// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "platform/input/ft6336_report.hpp"

namespace {

using micropixel::platform::input::ReadFt6336Report;

struct FakePanel final {
    std::array<uint8_t, 13> report{};
    esp_err_t status{ESP_OK};
    uint32_t reads{};

    void Begin(uint8_t count) {
        report.fill(0xffU);
        report[0] = count;
    }

    void Point(size_t slot, uint8_t id, uint8_t event, uint16_t x, uint16_t y) {
        const size_t offset = 1U + slot * 6U;
        report[offset] = static_cast<uint8_t>((event << 6U) | (x >> 8U));
        report[offset + 1U] = static_cast<uint8_t>(x);
        report[offset + 2U] = static_cast<uint8_t>((id << 4U) | (y >> 8U));
        report[offset + 3U] = static_cast<uint8_t>(y);
    }
};

struct Fixture final {
    FakePanel panel{};
    esp_lcd_touch_t touch{};

    Fixture() {
        touch.io = &panel;
        touch.config.x_max = 240U;
        touch.config.y_max = 320U;
    }

    void Read(uint8_t count) {
        const uint32_t previous_reads = panel.reads;
        assert(ReadFt6336Report(&touch) == ESP_OK);
        assert(panel.reads == previous_reads + 1U);
        assert(touch.data.points == count);
    }

    void Reject() {
        const auto previous = touch.data;
        assert(ReadFt6336Report(&touch) == ESP_ERR_INVALID_RESPONSE);
        assert(touch.data.points == previous.points);
        for (size_t index = 0U; index < previous.points; ++index) {
            assert(touch.data.coords[index].track_id == previous.coords[index].track_id);
            assert(touch.data.coords[index].x == previous.coords[index].x);
            assert(touch.data.coords[index].y == previous.coords[index].y);
        }
    }
};

void HeldFingersKeepTheirIdsAcrossSlotChanges() {
    Fixture fixture;
    fixture.panel.Begin(2U);
    fixture.panel.Point(0U, 3U, 0U, 30U, 60U);
    fixture.panel.Point(1U, 7U, 0U, 160U, 260U);
    fixture.Read(2U);
    for (uint32_t frame = 0U; frame < 1000U; ++frame) {
        fixture.panel.Begin(2U);
        fixture.panel.Point(frame % 2U, 3U, 2U, 30U, 60U);
        fixture.panel.Point(1U - frame % 2U, 7U, 2U, 160U, 260U);
        fixture.Read(2U);
        assert(fixture.touch.data.coords[frame % 2U].track_id == 3U);
        assert(fixture.touch.data.coords[1U - frame % 2U].track_id == 7U);
    }
}

void LiftedFirstSlotDoesNotHideTheSecondFinger() {
    Fixture fixture;
    fixture.panel.Begin(1U);
    fixture.panel.Point(0U, 3U, 1U, 30U, 60U);
    fixture.panel.Point(1U, 7U, 2U, 160U, 260U);
    fixture.Read(1U);
    assert(fixture.touch.data.coords[0].track_id == 7U);
    assert(fixture.touch.data.coords[0].x == 160U);

    // The remaining track may later move to the first register slot.
    fixture.panel.Begin(1U);
    fixture.panel.Point(0U, 7U, 2U, 170U, 280U);
    fixture.Read(1U);
    assert(fixture.touch.data.coords[0].track_id == 7U);
    assert(fixture.touch.data.coords[0].y == 280U);

    // TD_STATUS=0 releases even if the coordinate registers retain old data.
    fixture.panel.report[0] = 0U;
    fixture.Read(0U);
}

void InconsistentReportsDoNotReplaceHeldFingers() {
    Fixture fixture;
    fixture.panel.Begin(2U);
    fixture.panel.Point(0U, 0U, 0U, 30U, 60U);
    fixture.panel.Point(1U, 1U, 0U, 160U, 260U);
    fixture.Read(2U);

    // Duplicate IDs otherwise collapse two independent tracks into one.
    fixture.panel.Point(1U, 0U, 2U, 160U, 260U);
    fixture.Reject();
    // Out-of-range data must not become a clamped edge swipe.
    fixture.panel.Point(1U, 1U, 2U, 4095U, 260U);
    fixture.Reject();
    fixture.panel.Point(1U, 1U, 2U, 160U, 4095U);
    fixture.Reject();
    // The count and active records disagree, including reserved events/IDs.
    fixture.panel.Point(1U, 1U, 1U, 160U, 260U);
    fixture.Reject();
    fixture.panel.Point(1U, 1U, 3U, 160U, 260U);
    fixture.Reject();
    fixture.panel.Point(1U, 15U, 2U, 160U, 260U);
    fixture.Reject();
    fixture.panel.Point(1U, 1U, 2U, 160U, 260U);
    fixture.panel.report[0] = 1U;
    fixture.Reject();
    fixture.panel.report[0] = 3U;
    fixture.Reject();

    // A subsequent valid report resumes both original tracks immediately.
    fixture.panel.report[0] = 2U;
    fixture.Read(2U);
    assert(fixture.touch.data.coords[0].track_id == 0U);
    assert(fixture.touch.data.coords[1].track_id == 1U);
}

void SwipeCoordinatesAndCountBitsAreDecoded() {
    Fixture fixture;
    for (uint16_t y = 20U; y <= 320U; y += 20U) {
        // Raw Y becomes landscape X in SZPI's existing swap/mirror transform.
        fixture.panel.Begin(0xa1U);
        fixture.panel.Point(0U, 4U, y == 20U ? 0U : 2U, 240U, y);
        fixture.Read(1U);
        assert(fixture.touch.data.coords[0].track_id == 4U);
        assert(fixture.touch.data.coords[0].x == 240U);
        assert(fixture.touch.data.coords[0].y == y);
    }
    fixture.panel.status = ESP_FAIL;
    assert(ReadFt6336Report(&fixture.touch) == ESP_FAIL);
    assert(fixture.touch.data.points == 1U);
    assert(ReadFt6336Report(nullptr) == ESP_ERR_INVALID_ARG);
    fixture.touch.io = nullptr;
    assert(ReadFt6336Report(&fixture.touch) == ESP_ERR_INVALID_ARG);
}

}  // namespace

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int command, void* data, size_t length) {
    auto& panel = *static_cast<FakePanel*>(io);
    // Enforce one complete read starting at TD_STATUS, even for one/no finger.
    assert(command == 0x02);
    assert(length == panel.report.size());
    ++panel.reads;
    if (panel.status == ESP_OK) {
        std::memcpy(data, panel.report.data(), length);
    }
    return panel.status;
}

int main() {
    HeldFingersKeepTheirIdsAcrossSlotChanges();
    LiftedFirstSlotDoesNotHideTheSecondFinger();
    InconsistentReportsDoNotReplaceHeldFingers();
    SwipeCoordinatesAndCountBitsAreDecoded();
    std::puts("FT6336 report tests passed");
}
