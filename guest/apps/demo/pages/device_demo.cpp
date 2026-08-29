// Devices, typed Sensors, GPIO, Haptics, and PowerInfo usage. See ../README.md.

#include <utility>

#include "apps/demo/demo_page.hpp"

using micropixel::literals::operator""_ms;

namespace demo {

namespace {

[[nodiscard]] const char* KindName(micropixel::DeviceKind kind) {
    switch (kind) {
        case micropixel::DeviceKind::kDisplay:
            return "Display";
        case micropixel::DeviceKind::kTouch:
            return "Touch";
        case micropixel::DeviceKind::kAudioInput:
            return "Audio input";
        case micropixel::DeviceKind::kAudioOutput:
            return "Audio output";
        case micropixel::DeviceKind::kSensor:
            return "Sensor";
        case micropixel::DeviceKind::kGpioLine:
            return "GPIO line";
        case micropixel::DeviceKind::kHaptics:
            return "Haptics";
        case micropixel::DeviceKind::kPower:
            return "Power";
        case micropixel::DeviceKind::kGamepad:
            return "Gamepad";
        case micropixel::DeviceKind::kCamera:
            return "Camera";
        case micropixel::DeviceKind::kLocation:
            return "Location";
        case micropixel::DeviceKind::kStorage:
            return "Storage";
        case micropixel::DeviceKind::kNetwork:
            return "Network";
        case micropixel::DeviceKind::kAny:
            return "Unknown";
    }
    return "Unknown";
}

void AppendFixed2(Line& line, float value) {
    const int32_t scaled = static_cast<int32_t>(value * 100.0F);
    const uint32_t magnitude = static_cast<uint32_t>(scaled < 0 ? -scaled : scaled);
    if (scaled < 0) {
        line.Append("-");
    }
    line.AppendUint(magnitude / 100U);
    line.Append(".");
    const uint64_t fraction = magnitude % 100U;
    if (fraction < 10U) {
        line.Append("0");
    }
    line.AppendUint(fraction);
}

class DevicePage final {
   public:
    void Enter(DemoContext& context) {
        status_.Clear();
        RefreshCatalog(context);
        LayoutButtons(context);
    }

    void Exit() { CloseSelected(); }

    [[nodiscard]] bool OnTimer(DemoContext& context, const micropixel::TimerEvent&) {
        ++timer_ticks_;
        if (accelerometer_.valid()) {
            auto sample = accelerometer_.Read();
            if (!sample) {
                if (sample.error().code() == micropixel::ErrorCode::kWouldBlock) {
                    return false;
                }
                SetError("Accelerometer read failed: ", sample.error());
                return true;
            }
            acceleration_ = sample->value;
            has_acceleration_ = true;
            return true;
        }
        if (magnetometer_.valid()) {
            auto sample = magnetometer_.Read();
            if (!sample) {
                if (sample.error().code() == micropixel::ErrorCode::kWouldBlock) {
                    return false;
                }
                SetError("Magnetometer read failed: ", sample.error());
                return true;
            }
            magnetic_field_ = sample->value;
            has_magnetic_field_ = true;
            return true;
        }
        if (selected_valid_ && selected_.kind == micropixel::DeviceKind::kPower && timer_ticks_ % 50U == 0U) {
            ReadPower(context);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool OnEvent(DemoContext& context, const micropixel::Event& event) {
        if (const micropixel::GpioEdgeEvent* edge = event.EdgeFrom(gpio_input_)) {
            gpio_value_ = edge->value();
            has_gpio_value_ = true;
            SetStatus(edge->edge() == micropixel::GpioEdge::kRising ? "Rising edge: INPUT HIGH"
                                                                    : "Falling edge: INPUT LOW");
            return true;
        }
        if (event.HapticFrom(haptic_) != nullptr) {
            SetStatus("Vibration finished");
            return true;
        }
        if (event.type() == micropixel::EventType::kDeviceAdded ||
            event.type() == micropixel::EventType::kDeviceRemoved) {
            RefreshCatalog(context);
            LayoutButtons(context);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool OnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
        bool redraw = false;
        for (uint32_t index = 0U; index < 3U; ++index) {
            const micropixel::ui::ButtonUpdate update = buttons_[index].OnTouch(event);
            redraw = redraw || update.redraw();
            if (!update.clicked) {
                continue;
            }
            if (index == 0U) {
                SelectRelative(context, -1);
            } else if (index == 1U) {
                RunAction(context);
            } else {
                SelectRelative(context, 1);
            }
            redraw = true;
        }
        return redraw;
    }

    void Render(DemoContext& context, DemoView& commands) {
        const int32_t center_x = PageCenterX(context);
        if (!selected_valid_) {
            commands.CenteredText(center_x, PageY(context, 80, 90), "No devices discovered", DangerColor(),
                                  micropixel::SystemFont::kTitle);
            commands.CenteredText(center_x, PageY(context, 140, 150), status_.c_str(), MutedColor(),
                                  micropixel::SystemFont::kMedium);
        } else {
            Line heading;
            heading.Append("Device ");
            heading.AppendUint(selected_index_ + 1U);
            heading.Append(" / ");
            heading.AppendUint(devices_.size());
            commands.CenteredText(center_x, PageY(context, 10, 12), heading.c_str(), MutedColor(),
                                  micropixel::SystemFont::kMedium);
            commands.CenteredText(center_x, PageY(context, 40, 48), selected_.name.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kLarge);

            Line identity;
            identity.Append(KindName(selected_.kind));
            identity.Append("   DeviceId ");
            identity.AppendUint(selected_.id.value());
            commands.CenteredText(center_x, PageY(context, 78, 94), identity.c_str(), AccentColor(),
                                  micropixel::SystemFont::kMedium);
            RenderSelected(context, commands, center_x);
            commands.CenteredText(center_x, context.layout.page_content.y + context.layout.page_content.height - 30,
                                  status_.c_str(), MutedColor(), micropixel::SystemFont::kMedium);
        }

        DrawButton(commands, buttons_[0], "PREV", BlueColor());
        if (HasAction()) {
            const char* action = "ACTION";
            if (selected_.kind == micropixel::DeviceKind::kGpioLine) {
                action = gpio_output_.valid() ? "TOGGLE" : "READ";
            }
            DrawButton(commands, buttons_[1], action, AccentColor());
        }
        DrawButton(commands, buttons_[2], "NEXT", BlueColor());
    }

   private:
    void RefreshCatalog(DemoContext& context) {
        CloseSelected();
        auto listed = context.app.devices().List();
        if (!listed) {
            selected_valid_ = false;
            SetError("List failed: ", listed.error());
            return;
        }
        devices_ = *listed;
        if (devices_.empty()) {
            selected_valid_ = false;
            SetStatus("Devices::List returned an empty catalog");
            return;
        }
        if (selected_index_ >= devices_.size()) {
            selected_index_ = 0U;
        }
        OpenSelected(context);
    }

    void SelectRelative(DemoContext& context, int32_t delta) {
        if (devices_.empty()) {
            RefreshCatalog(context);
            return;
        }
        const int32_t count = static_cast<int32_t>(devices_.size());
        int32_t index = static_cast<int32_t>(selected_index_) + delta;
        if (index < 0) {
            index = count - 1;
        } else if (index >= count) {
            index = 0;
        }
        selected_index_ = static_cast<uint32_t>(index);
        CloseSelected();
        OpenSelected(context);
        LayoutButtons(context);
    }

    [[nodiscard]] bool HasAction() const {
        if (!selected_valid_) {
            return false;
        }
        return selected_.kind == micropixel::DeviceKind::kHaptics ||
               selected_.kind == micropixel::DeviceKind::kGpioLine || selected_.kind == micropixel::DeviceKind::kPower;
    }

    [[nodiscard]] bool ActionEnabled() const {
        if (!HasAction()) {
            return false;
        }
        if (selected_.kind == micropixel::DeviceKind::kHaptics) {
            return haptic_.valid();
        }
        if (selected_.kind == micropixel::DeviceKind::kGpioLine) {
            return gpio_input_.valid() || gpio_output_.valid();
        }
        return true;
    }

    void LayoutButtons(const DemoContext& context) {
        if (HasAction()) {
            LayoutButtonRow(context, buttons_);
            buttons_[1].SetEnabled(ActionEnabled());
            return;
        }

        constexpr std::array items{micropixel::ui::FlexItem::Grow(), micropixel::ui::FlexItem::Grow()};
        std::array<micropixel::Rect, items.size()> rects{};
        const int32_t horizontal_padding = context.layout.compact() ? 20 : 28;
        const int32_t vertical_padding = context.layout.compact() ? 12 : 16;
        auto result = micropixel::ui::ComputeFlexLayout(
            context.layout.page_actions,
            micropixel::ui::FlexLayout{
                .direction = micropixel::ui::FlexDirection::kHorizontal,
                .padding = {vertical_padding, horizontal_padding, vertical_padding, horizontal_padding},
                .gap_pixels = context.layout.compact() ? 10 : 14},
            items, rects);
        micropixel::Assert(result.has_value(), "demo.device: navigation button layout failed");
        buttons_[0].SetBounds(rects[0]);
        buttons_[1].SetBounds({});
        buttons_[1].SetEnabled(false);
        buttons_[2].SetBounds(rects[1]);
    }

    void OpenSelected(DemoContext& context) {
        auto info = context.app.devices().GetInfo(devices_[selected_index_]);
        if (!info) {
            selected_valid_ = false;
            SetError("GetInfo failed: ", info.error());
            return;
        }
        selected_ = *info;
        selected_valid_ = true;
        timer_ticks_ = 0U;
        SetStatus("Enumerated through Devices Service");

        if (selected_.kind == micropixel::DeviceKind::kSensor) {
            auto sensor_info = context.app.sensors().GetInfo(selected_.id);
            if (!sensor_info) {
                SetError("Sensor info failed: ", sensor_info.error());
                return;
            }
            sensor_kind_ = sensor_info->kind;
            if (sensor_kind_ == micropixel::SensorKind::kAcceleration) {
                auto opened = context.app.sensors().Open<micropixel::Acceleration>(selected_.id);
                if (opened) {
                    accelerometer_ = std::move(*opened);
                    ConfigureSensor(accelerometer_);
                } else {
                    SetError("Sensor open failed: ", opened.error());
                }
            } else if (sensor_kind_ == micropixel::SensorKind::kMagneticField) {
                auto opened = context.app.sensors().Open<micropixel::MagneticField>(selected_.id);
                if (opened) {
                    magnetometer_ = std::move(*opened);
                    ConfigureSensor(magnetometer_);
                } else {
                    SetError("Sensor open failed: ", opened.error());
                }
            }
        } else if (selected_.kind == micropixel::DeviceKind::kHaptics) {
            auto opened = context.app.haptics().Open(selected_.id);
            if (opened) {
                haptic_ = std::move(*opened);
                SetStatus("ACTION plays the selected haptic device");
            } else {
                SetError("Haptics open failed: ", opened.error());
            }
        } else if (selected_.kind == micropixel::DeviceKind::kGpioLine) {
            auto info_result = context.app.gpio().GetInfo(selected_.id);
            if (!info_result) {
                SetError("GPIO info failed: ", info_result.error());
                return;
            }
            gpio_info_ = *info_result;
            if (gpio_info_.Supports(micropixel::GpioCapability::kInput)) {
                auto opened = context.app.gpio().OpenInput(
                    selected_.id, micropixel::GpioInputOptions{.pull = micropixel::GpioPull::kDown,
                                                               .edge = micropixel::GpioEdgeTrigger::kBoth});
                if (!opened) {
                    SetError("GPIO input failed: ", opened.error());
                    return;
                }
                gpio_input_ = std::move(*opened);
                SetStatus("Pull-down active; connect this line to 3.3V");
                (void)ReadGpio();
            } else if (gpio_info_.Supports(micropixel::GpioCapability::kOutput)) {
                auto opened = context.app.gpio().OpenOutput(selected_.id, false);
                if (!opened) {
                    SetError("GPIO output failed: ", opened.error());
                    return;
                }
                gpio_output_ = std::move(*opened);
                gpio_value_ = false;
                has_gpio_value_ = true;
                SetStatus("TOGGLE controls this logical output");
            } else {
                SetStatus("GPIO has no Demo-compatible mode");
            }
        } else if (selected_.kind == micropixel::DeviceKind::kPower) {
            ReadPower(context);
        }
    }

    void CloseSelected() {
        accelerometer_.Reset();
        magnetometer_.Reset();
        gpio_input_.Reset();
        gpio_output_.Reset();
        haptic_.Reset();
        selected_valid_ = false;
        has_acceleration_ = false;
        has_magnetic_field_ = false;
        has_power_ = false;
        has_gpio_value_ = false;
        gpio_value_ = false;
    }

    template <typename Reading>
    void ConfigureSensor(micropixel::Sensor<Reading>& sensor) {
        auto configured = sensor.SetSampleInterval(10_ms);
        if (!configured) {
            SetError("Sensor rate failed: ", configured.error());
            return;
        }
        SetStatus("Sensor 100 Hz; display reads at 50 Hz");
    }

    void RunAction(DemoContext& context) {
        if (!selected_valid_) {
            RefreshCatalog(context);
            return;
        }
        if (selected_.kind == micropixel::DeviceKind::kHaptics && haptic_.valid()) {
            const auto played = haptic_.Play(300_ms, 1000U);
            if (played) {
                SetStatus("Vibrating for 300 ms");
            } else {
                SetError("Haptics play failed: ", played.error());
            }
            return;
        }
        if (selected_.kind == micropixel::DeviceKind::kGpioLine) {
            if (gpio_output_.valid()) {
                const bool next_value = !gpio_value_;
                auto written = gpio_output_.Write(next_value);
                if (!written) {
                    SetError("GPIO write failed: ", written.error());
                    return;
                }
                gpio_value_ = next_value;
                has_gpio_value_ = true;
                SetStatus(gpio_value_ ? "Logical output is ON" : "Logical output is OFF");
                return;
            }
            (void)ReadGpio(true);
            return;
        }
        if (selected_.kind == micropixel::DeviceKind::kPower) {
            ReadPower(context);
            return;
        }
        SetStatus("This device is discoverable; no Demo action");
    }

    [[nodiscard]] bool ReadGpio(bool report = false) {
        const bool previous_value = gpio_value_;
        const bool previously_valid = has_gpio_value_;
        auto value = gpio_input_.Read();
        if (!value) {
            has_gpio_value_ = false;
            SetError("GPIO read failed: ", value.error());
            return true;
        }
        gpio_value_ = *value;
        has_gpio_value_ = true;
        if (report) {
            SetStatus(gpio_value_ ? "Read now: INPUT HIGH" : "Read now: INPUT LOW");
        }
        return !previously_valid || previous_value != gpio_value_;
    }

    void ReadPower(DemoContext& context) {
        auto power = context.app.power_info().Get(selected_.id);
        if (power) {
            power_ = *power;
            has_power_ = true;
            SetStatus("PowerInfo refreshed");
        } else {
            has_power_ = false;
            SetError("PowerInfo failed: ", power.error());
        }
    }

    void RenderSelected(DemoContext& context, DemoView& commands, int32_t center_x) const {
        if (selected_.kind == micropixel::DeviceKind::kSensor) {
            Line axes;
            if (sensor_kind_ == micropixel::SensorKind::kAcceleration && has_acceleration_) {
                axes.Append("m/s2  x ");
                AppendFixed2(axes, acceleration_.meters_per_second_squared.x);
                axes.Append("  y ");
                AppendFixed2(axes, acceleration_.meters_per_second_squared.y);
                axes.Append("  z ");
                AppendFixed2(axes, acceleration_.meters_per_second_squared.z);
            } else if (sensor_kind_ == micropixel::SensorKind::kMagneticField && has_magnetic_field_) {
                axes.Append("uT  x ");
                AppendFixed2(axes, magnetic_field_.microtesla.x);
                axes.Append("  y ");
                AppendFixed2(axes, magnetic_field_.microtesla.y);
                axes.Append("  z ");
                AppendFixed2(axes, magnetic_field_.microtesla.z);
            } else {
                axes.Append("Waiting for the first cached sample...");
            }
            commands.CenteredText(center_x, PageY(context, 135, 178), axes.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        } else if (selected_.kind == micropixel::DeviceKind::kGpioLine) {
            Line gpio;
            gpio.Append("Physical line ");
            gpio.AppendUint(gpio_info_.line_number);
            if (gpio_output_.valid() && has_gpio_value_) {
                gpio.Append(gpio_value_ ? "   OUTPUT ON" : "   OUTPUT OFF");
            } else if (!gpio_input_.valid() || !has_gpio_value_) {
                gpio.Append("   input unavailable");
            } else {
                gpio.Append(gpio_value_ ? "   INPUT HIGH" : "   INPUT LOW");
            }
            commands.CenteredText(center_x, PageY(context, 135, 178), gpio.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        } else if (selected_.kind == micropixel::DeviceKind::kPower && has_power_) {
            Line power;
            power.Append("Battery ");
            power.AppendUint(power_.battery_percent);
            power.Append("%   ");
            power.Append(power_.external_connected ? "external power" : "battery power");
            commands.CenteredText(center_x, PageY(context, 135, 178), power.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        } else {
            commands.CenteredText(center_x, PageY(context, 135, 178), "Select devices by opaque DeviceId",
                                  micropixel::Color::White(), micropixel::SystemFont::kMedium);
        }
    }

    void SetStatus(const char* text) {
        status_.Clear();
        status_.Append(text);
    }

    void SetError(const char* prefix, micropixel::Error error) {
        status_.Clear();
        status_.Append(prefix);
        status_.Append(error.name());
    }

    micropixel::DeviceList devices_{};
    micropixel::DeviceInfo selected_{};
    micropixel::GpioInfo gpio_info_{};
    micropixel::PowerState power_{};
    micropixel::Accelerometer accelerometer_{};
    micropixel::Magnetometer magnetometer_{};
    micropixel::GpioInput gpio_input_{};
    micropixel::GpioOutput gpio_output_{};
    micropixel::Haptic haptic_{};
    micropixel::Acceleration acceleration_{};
    micropixel::MagneticField magnetic_field_{};
    micropixel::SensorKind sensor_kind_{micropixel::SensorKind::kAcceleration};
    Line status_{};
    uint32_t selected_index_{};
    uint32_t timer_ticks_{};
    bool selected_valid_{};
    bool has_acceleration_{};
    bool has_magnetic_field_{};
    bool has_power_{};
    bool has_gpio_value_{};
    bool gpio_value_{};
    micropixel::ui::Button buttons_[3]{};
};

[[clang::no_destroy]] DevicePage device_page;

}  // namespace

micropixel::Timer CreateDeviceTicker(micropixel::Application& app) { return app.timers().Every(20_ms); }

void DeviceDemoEnter(DemoContext& context) { device_page.Enter(context); }
void DeviceDemoExit(DemoContext&) { device_page.Exit(); }
bool DeviceDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event) {
    return device_page.OnTimer(context, event);
}
bool DeviceDemoOnEvent(DemoContext& context, const micropixel::Event& event) {
    return device_page.OnEvent(context, event);
}
bool DeviceDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return device_page.OnTouch(context, event);
}
void DeviceDemoRender(DemoContext& context, DemoView& commands) { device_page.Render(context, commands); }

}  // namespace demo
