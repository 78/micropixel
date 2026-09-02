#include "platform/boards/esp32-s3-box-3/host_mute_control.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/esp32-s3-box-3/board_hardware.hpp"

namespace micropixel::platform::esp32_s3_box_3 {
namespace {

constexpr char kTag[] = "box3_mute";
constexpr int kMutedLevel = 0;

}  // namespace

HostMuteControl::~HostMuteControl() {
    if (isr_registered_) {
        (void)gpio_intr_disable(kMuteStatus);
        (void)gpio_isr_handler_remove(kMuteStatus);
    }
}

esp_err_t HostMuteControl::Initialize(audio::AudioEngine& audio) {
    if (audio_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_config_t config{};
    config.pin_bit_mask = BIT64(kMuteStatus);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure mute status input failed");

    audio_ = &audio;
    ApplyLevel();
    esp_err_t status = gpio_isr_handler_add(kMuteStatus, OnEdge, this);
    if (status == ESP_ERR_INVALID_STATE) {
        status = gpio_install_isr_service(0);
        if (status == ESP_OK) {
            status = gpio_isr_handler_add(kMuteStatus, OnEdge, this);
        }
    }
    if (status == ESP_OK) {
        isr_registered_ = true;
        status = gpio_set_intr_type(kMuteStatus, GPIO_INTR_ANYEDGE);
    }
    if (status == ESP_OK) {
        status = gpio_intr_enable(kMuteStatus);
    }
    if (status != ESP_OK) {
        if (isr_registered_) {
            (void)gpio_isr_handler_remove(kMuteStatus);
            isr_registered_ = false;
        }
        audio_ = nullptr;
        return status;
    }
    ESP_LOGI(kTag, "Host mute ready on GPIO%d: %s", static_cast<int>(kMuteStatus),
             gpio_get_level(kMuteStatus) == kMutedLevel ? "muted" : "unmuted");
    return ESP_OK;
}

void HostMuteControl::OnEdge(void* context) { static_cast<HostMuteControl*>(context)->ApplyLevel(); }

void HostMuteControl::ApplyLevel() const {
    if (audio_ != nullptr) {
        audio_->SetHardwareMuted(gpio_get_level(kMuteStatus) == kMutedLevel);
    }
}

}  // namespace micropixel::platform::esp32_s3_box_3
