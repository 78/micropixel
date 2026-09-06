#include "platform/lvgl/display/scanout_stage_pool.hpp"

#include <cinttypes>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace micropixel::platform::lvgl {
namespace {

constexpr char kTag[] = "scanout_pool";

}  // namespace

ScanoutStagePool& ScanoutStagePool::Instance() {
    static ScanoutStagePool pool;
    return pool;
}

esp_err_t ScanoutStagePool::Initialize(uint32_t slot_bytes, uint32_t slot_count) {
    if (slot_bytes == 0U || slot_count == 0U || slot_count > kMaxSlots) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot_count_ != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t aligned = (slot_bytes + kSlotAlignment - 1U) / kSlotAlignment * kSlotAlignment;
    for (uint32_t index = 0U; index < slot_count; ++index) {
        slots_[index] = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kSlotAlignment, aligned, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (slots_[index] == nullptr) {
            for (uint32_t release = 0U; release < index; ++release) {
                heap_caps_free(slots_[release]);
                slots_[release] = nullptr;
            }
            ESP_LOGE(kTag, "could not allocate scanout stage %" PRIu32 "/%" PRIu32 " (%" PRIu32 " B)", index + 1U,
                     slot_count, aligned);
            return ESP_ERR_NO_MEM;
        }
    }
    slot_bytes_ = aligned;
    slot_count_ = slot_count;
    ESP_LOGI(kTag, "scanout stage pool ready: %" PRIu32 " x %" PRIu32 " B in PSRAM", slot_count, aligned);
    return ESP_OK;
}

uint8_t* ScanoutStagePool::Acquire(uint32_t bytes, uint32_t keep_free) {
    if (slot_count_ == 0U || bytes > slot_bytes_) {
        return nullptr;
    }
    uint8_t* stage = nullptr;
    bool declined = false;
    taskENTER_CRITICAL(&lock_);
    if (slot_count_ - InUseLocked() <= keep_free) {
        declined = true;
    } else {
        for (uint32_t index = 0U; index < slot_count_; ++index) {
            const uint32_t bit = 1U << index;
            if ((in_use_mask_ & bit) == 0U) {
                in_use_mask_ |= bit;
                stage = slots_[index];
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&lock_);
    if (stage == nullptr && !declined) {
        ESP_LOGW(kTag, "scanout stage pool exhausted: %" PRIu32 " slots in use", slot_count_);
    }
    return stage;
}

void ScanoutStagePool::Release(uint8_t* stage) {
    if (stage == nullptr) {
        return;
    }
    taskENTER_CRITICAL(&lock_);
    for (uint32_t index = 0U; index < slot_count_; ++index) {
        if (slots_[index] == stage) {
            in_use_mask_ &= ~(1U << index);
            break;
        }
    }
    taskEXIT_CRITICAL(&lock_);
}

bool ScanoutStagePool::Owns(const uint8_t* stage) const {
    if (stage == nullptr) {
        return false;
    }
    for (uint32_t index = 0U; index < slot_count_; ++index) {
        if (slots_[index] == stage) {
            return true;
        }
    }
    return false;
}

uint32_t ScanoutStagePool::InUseLocked() const {
    uint32_t count = 0U;
    for (uint32_t mask = in_use_mask_; mask != 0U; mask &= mask - 1U) {
        ++count;
    }
    return count;
}

}  // namespace micropixel::platform::lvgl
