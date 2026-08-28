#include "platform/boards/metalio-claw4/audio/i2s_audio_sink.hpp"

#include <cstring>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/common/i2c_executor.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "claw4_audio_sink";
constexpr uint8_t kOutputPort0 = 0x02U;
constexpr uint8_t kOutputPort1 = 0x03U;
constexpr uint8_t kConfigPort0 = 0x06U;
constexpr uint8_t kConfigPort1 = 0x07U;
constexpr uint8_t kPaSwitchMask = 1U << 1U;
constexpr uint8_t kBtPowerMask = 1U << 6U;
constexpr uint8_t kPaEnableMask = 1U << 0U;
constexpr uint32_t kSampleRate = 16000U;
constexpr uint32_t kWakeupMs = 64U;

esp_err_t InitializeBtAudioMode() {
    uart_config_t config{};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_2, 512, 512, 0, nullptr, 0), kTag, "install audio UART");
    esp_err_t status = uart_param_config(UART_NUM_2, &config);
    if (status == ESP_OK) {
        status = uart_set_pin(UART_NUM_2, GPIO_NUM_26, GPIO_NUM_27, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (status != ESP_OK) {
        (void)uart_driver_delete(UART_NUM_2);
        return status;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    constexpr char kReceivePath[] = "AT+RX=2\r\n";
    constexpr char kModeOne[] = "AT+MODE=1\r\n";
    if (uart_write_bytes(UART_NUM_2, kReceivePath, sizeof(kReceivePath) - 1U) < 0) {
        return ESP_FAIL;
    }
    (void)uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(700));
    if (uart_write_bytes(UART_NUM_2, kModeOne, sizeof(kModeOne) - 1U) < 0) {
        return ESP_FAIL;
    }
    (void)uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(700));
    uint8_t response[160]{};
    const int response_size = uart_read_bytes(UART_NUM_2, response, sizeof(response) - 1U, pdMS_TO_TICKS(200));
    if (response_size > 0) {
        response[response_size] = '\0';
        for (int index = 0; index < response_size; ++index) {
            if (response[index] == '\r' || response[index] == '\n') {
                response[index] = ' ';
            }
        }
        ESP_LOGI(kTag, "module response: %s", reinterpret_cast<const char*>(response));
    } else {
        ESP_LOGW(kTag, "module returned no UART response");
    }
    return ESP_OK;
}

}  // namespace

void I2sAudioSink::Configure(i2c_master_dev_handle_t io_expander, common::I2cExecutor& i2c_executor) {
    io_expander_ = io_expander;
    i2c_executor_ = &i2c_executor;
}

esp_err_t I2sAudioSink::UpdateRegister(uint8_t address, uint8_t set_mask, uint8_t clear_mask) {
    if (i2c_executor_ == nullptr || io_expander_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    struct Request final {
        i2c_master_dev_handle_t device;
        uint8_t address;
        uint8_t set_mask;
        uint8_t clear_mask;
    } request{io_expander_, address, set_mask, clear_mask};
    return i2c_executor_->Invoke(
        common::I2cExecutor::Priority::kHigh,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            uint8_t value = 0U;
            esp_err_t status = i2c_master_transmit_receive(requested.device, &requested.address,
                                                           sizeof(requested.address), &value, sizeof(value), 1000);
            if (status != ESP_OK) {
                return status;
            }
            const uint8_t transaction[] = {requested.address,
                                           static_cast<uint8_t>((value | requested.set_mask) & ~requested.clear_mask)};
            return i2c_master_transmit(requested.device, transaction, sizeof(transaction), 1000);
        },
        &request);
}

esp_err_t I2sAudioSink::Initialize() {
    ESP_RETURN_ON_ERROR(UpdateRegister(kOutputPort0, kPaSwitchMask | kBtPowerMask, 0U), kTag, "set audio rail latches");
    ESP_RETURN_ON_ERROR(UpdateRegister(kOutputPort1, 0U, kPaEnableMask), kTag, "mute audio amplifier");
    ESP_RETURN_ON_ERROR(UpdateRegister(kConfigPort0, 0U, kPaSwitchMask | kBtPowerMask), kTag,
                        "configure audio rail outputs");
    ESP_RETURN_ON_ERROR(UpdateRegister(kConfigPort1, 0U, kPaEnableMask), kTag, "configure amplifier output");
    ESP_RETURN_ON_ERROR(InitializeBtAudioMode(), kTag, "initialize Bluetooth audio module");

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    channel_config.dma_desc_num = 6U;
    channel_config.dma_frame_num = 240U;
    channel_config.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &tx_, nullptr), kTag, "create I2S channel");
    i2s_std_config_t standard_config{};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    standard_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    standard_config.gpio_cfg.bclk = GPIO_NUM_12;
    standard_config.gpio_cfg.ws = GPIO_NUM_10;
    standard_config.gpio_cfg.dout = GPIO_NUM_9;
    standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    return i2s_channel_init_std_mode(tx_, &standard_config);
}

esp_err_t I2sAudioSink::WriteSilence(int32_t* frames, uint32_t frame_count) {
    std::memset(frames, 0, static_cast<size_t>(frame_count) * 2U * sizeof(*frames));
    return Write(frames, frame_count);
}

esp_err_t I2sAudioSink::Start(int32_t* scratch_frames, uint32_t frame_count) {
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_), kTag, "enable idle-gated I2S output");
    esp_err_t status = WriteSilence(scratch_frames, frame_count);
    if (status == ESP_OK) {
        status = UpdateRegister(kOutputPort1, kPaEnableMask, 0U);
    }
    const uint32_t wakeup_chunks = (kWakeupMs * kSampleRate + frame_count * 1000U - 1U) / (frame_count * 1000U);
    for (uint32_t chunk = 0U; status == ESP_OK && chunk < wakeup_chunks; ++chunk) {
        status = WriteSilence(scratch_frames, frame_count);
    }
    if (status != ESP_OK) {
        (void)UpdateRegister(kOutputPort1, 0U, kPaEnableMask);
        (void)i2s_channel_disable(tx_);
    }
    return status;
}

esp_err_t I2sAudioSink::Write(const int32_t* frames, uint32_t frame_count) {
    if (tx_ == nullptr || frames == nullptr || frame_count == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t bytes = static_cast<size_t>(frame_count) * 2U * sizeof(*frames);
    size_t bytes_written = 0U;
    const esp_err_t status = i2s_channel_write(tx_, frames, bytes, &bytes_written, pdMS_TO_TICKS(500));
    return status == ESP_OK && bytes_written == bytes ? ESP_OK : (status == ESP_OK ? ESP_FAIL : status);
}

esp_err_t I2sAudioSink::Stop() {
    const esp_err_t amplifier_status = UpdateRegister(kOutputPort1, 0U, kPaEnableMask);
    const esp_err_t i2s_status = tx_ == nullptr ? ESP_ERR_INVALID_STATE : i2s_channel_disable(tx_);
    return amplifier_status != ESP_OK ? amplifier_status : i2s_status;
}

void I2sAudioSink::Shutdown() {
    (void)UpdateRegister(kOutputPort1, 0U, kPaEnableMask);
    if (tx_ != nullptr) {
        (void)i2s_channel_disable(tx_);
        (void)i2s_del_channel(tx_);
        tx_ = nullptr;
    }
    (void)uart_driver_delete(UART_NUM_2);
}

}  // namespace micropixel::platform::metalio_claw4
