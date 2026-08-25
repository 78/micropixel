#include "remote_control/remote_control_agent.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "abi/micropixel_abi.h"
#include "cJSON.h"
#include "client/http3_async_client.h"
#include "client/http3_client.h"
#include "device/wifi.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "remote_control/reconnect_policy.hpp"
#include "runtime/bundle/bundle_format.h"
#include "sdkconfig.h"
#include "system_time.hpp"
#include "task_policy.hpp"

namespace micropixel::firmware::remote_control {
namespace {

#if CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS
constexpr bool kAllowUnverifiedTls = true;
#else
constexpr bool kAllowUnverifiedTls = false;
#endif

constexpr char kTag[] = "remote_control";
constexpr char kPartition[] = "sys_store";
constexpr char kNamespace[] = "control";
constexpr char kIdentityKey[] = "identity";
constexpr uint8_t kIdentityVersion = 1U;
constexpr uint32_t kTaskStackBytes = 16U * 1024U;
constexpr uint32_t kRequestTimeoutMs = 10000U;
constexpr uint32_t kControlReadTimeoutMs = 250U;
constexpr size_t kMaxConcurrentAuxiliaryRequests = 10U;
constexpr uint32_t kShutdownTimeoutMs = (kRequestTimeoutMs * 3U) + 5000U;
constexpr uint32_t kPairingTtlMs = 5U * 60U * 1000U;
constexpr TickType_t kOfflinePollTicks = pdMS_TO_TICKS(1000U);
constexpr size_t kGuestLogCapacity = 1024U;
constexpr size_t kGuestLogResponseCapacity = 48U;
constexpr size_t kGuestLogResponseJsonBudget = 60U * 1024U;
constexpr size_t kGuestLogMessageCapacity = MICROPIXEL_ABI_MAX_LOG_BYTES + 1U;
constexpr size_t kMaxEventBodyBytes = 64U * 1024U;
constexpr size_t kMaxPackageBytes = 8U * 1024U * 1024U;
constexpr size_t kMaxFirmwareBytes = 0x380000U;
constexpr size_t kTaskDiagnosticCapacity = 48U;
constexpr uint32_t kPackageDownloadTimeoutMs = 60000U;
constexpr uint32_t kDefaultCommandTimeoutMs = 60000U;
constexpr uint32_t kMinCommandTimeoutMs = 1000U;
constexpr uint32_t kMaxCommandTimeoutMs = 5U * 60U * 1000U;
constexpr TickType_t kFirmwareCheckIntervalTicks = pdMS_TO_TICKS(15U * 60U * 1000U);

const char* FirmwareUpdateStateText(host_ui::FirmwareUpdateState state) {
    switch (state) {
        case host_ui::FirmwareUpdateState::kChecking:
            return "checking";
        case host_ui::FirmwareUpdateState::kCurrent:
            return "current";
        case host_ui::FirmwareUpdateState::kAvailable:
            return "available";
        case host_ui::FirmwareUpdateState::kDownloading:
            return "downloading";
        case host_ui::FirmwareUpdateState::kVerifying:
            return "verifying";
        case host_ui::FirmwareUpdateState::kInstalling:
            return "installing";
        case host_ui::FirmwareUpdateState::kFailed:
            return "failed";
        case host_ui::FirmwareUpdateState::kUnknown:
            return "unknown";
    }
    return "unknown";
}

void AddFirmwareUpdateJson(cJSON* parent, const host_ui::RemoteControlModel& control) {
    cJSON* firmware_update = parent != nullptr ? cJSON_AddObjectToObject(parent, "firmwareUpdate") : nullptr;
    if (firmware_update == nullptr) {
        return;
    }
    (void)cJSON_AddBoolToObject(firmware_update, "available", control.firmware_update_available);
    (void)cJSON_AddBoolToObject(firmware_update, "installable", control.firmware_update_installable);
    (void)cJSON_AddStringToObject(firmware_update, "state", FirmwareUpdateStateText(control.firmware_update_state));
    (void)cJSON_AddStringToObject(firmware_update, "latestVersion", control.latest_firmware_version.data());
    (void)cJSON_AddNumberToObject(firmware_update, "sizeBytes", control.firmware_size_bytes);
    (void)cJSON_AddNumberToObject(firmware_update, "processedBytes", control.firmware_processed_bytes);
    (void)cJSON_AddNumberToObject(firmware_update, "progressPercent", control.firmware_progress_percent);
    (void)cJSON_AddStringToObject(firmware_update, "message", control.firmware_update_message.data());
}

const char* TaskStateText(eTaskState state) {
    switch (state) {
        case eRunning:
            return "running";
        case eReady:
            return "ready";
        case eBlocked:
            return "blocked";
        case eSuspended:
            return "suspended";
        case eDeleted:
            return "deleted";
        case eInvalid:
        default:
            return "invalid";
    }
}

bool DeadlineReached(TickType_t deadline_ticks) {
    return deadline_ticks != 0U && static_cast<int32_t>(xTaskGetTickCount() - deadline_ticks) >= 0;
}

struct GuestLogEntry final {
    uint64_t sequence{};
    uint64_t timestamp_us{};
    uint32_t level{};
    std::array<char, kRemoteControlAppIdCapacity> app_id{};
    std::array<char, kGuestLogMessageCapacity> message{};
};

struct IdentityRecord final {
    uint8_t version{};
    uint8_t reserved[3]{};
    uint32_t auth_epoch{};
    char device_id[host_ui::kRemoteControlDeviceIdCapacity]{};
    char credential[1024U]{};
};

static_assert(sizeof(IdentityRecord) == 1072U, "Remote Control identity record layout changed");

template <size_t Capacity>
void CopyText(std::array<char, Capacity>& destination, const char* source) {
    destination.fill('\0');
    if (source != nullptr) {
        const size_t length = ::strnlen(source, destination.size() - 1U);
        std::memcpy(destination.data(), source, length);
    }
}

bool ValidIdentityRecord(const IdentityRecord& record) {
    if (record.version != kIdentityVersion || record.auth_epoch == 0U || record.device_id[0] == '\0' ||
        record.credential[0] == '\0' ||
        ::strnlen(record.device_id, sizeof(record.device_id)) >= sizeof(record.device_id) ||
        ::strnlen(record.credential, sizeof(record.credential)) >= sizeof(record.credential)) {
        return false;
    }
    return std::all_of(record.reserved, record.reserved + sizeof(record.reserved),
                       [](uint8_t byte) { return byte == 0U; });
}

const char* JsonString(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : nullptr;
}

uint32_t JsonPositiveUint(const cJSON* object, const char* name, uint32_t fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) && item->valuedouble >= 1.0 && item->valuedouble <= UINT32_MAX
               ? static_cast<uint32_t>(item->valuedouble)
               : fallback;
}

uint64_t JsonNonNegativeUint64(const cJSON* object, const char* name, uint64_t fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= static_cast<double>(UINT64_MAX)
               ? static_cast<uint64_t>(item->valuedouble)
               : fallback;
}

bool JsonUint(const cJSON* object, const char* name, uint32_t maximum, uint32_t& value_out, uint32_t fallback = 0U) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == nullptr) {
        value_out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 || item->valuedouble > maximum ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value_out = static_cast<uint32_t>(item->valuedouble);
    return true;
}

bool ParseTouchPhase(const char* text, device::TouchPhase& phase_out) {
    if (text == nullptr) {
        return false;
    }
    if (std::strcmp(text, "down") == 0) {
        phase_out = device::TouchPhase::kDown;
    } else if (std::strcmp(text, "move") == 0) {
        phase_out = device::TouchPhase::kMove;
    } else if (std::strcmp(text, "up") == 0) {
        phase_out = device::TouchPhase::kUp;
    } else if (std::strcmp(text, "cancel") == 0) {
        phase_out = device::TouchPhase::kCancel;
    } else {
        return false;
    }
    return true;
}

bool ParseKeyCode(const char* text, device::KeyCode& code_out) {
    if (text == nullptr) {
        return false;
    }
    if (std::strcmp(text, "up") == 0) {
        code_out = device::KeyCode::kUp;
    } else if (std::strcmp(text, "down") == 0) {
        code_out = device::KeyCode::kDown;
    } else if (std::strcmp(text, "left") == 0) {
        code_out = device::KeyCode::kLeft;
    } else if (std::strcmp(text, "right") == 0) {
        code_out = device::KeyCode::kRight;
    } else if (std::strcmp(text, "confirm") == 0) {
        code_out = device::KeyCode::kConfirm;
    } else if (std::strcmp(text, "back") == 0) {
        code_out = device::KeyCode::kBack;
    } else if (std::strcmp(text, "menu") == 0) {
        code_out = device::KeyCode::kMenu;
    } else if (std::strcmp(text, "a") == 0) {
        code_out = device::KeyCode::kA;
    } else if (std::strcmp(text, "b") == 0) {
        code_out = device::KeyCode::kB;
    } else if (std::strcmp(text, "x") == 0) {
        code_out = device::KeyCode::kX;
    } else if (std::strcmp(text, "y") == 0) {
        code_out = device::KeyCode::kY;
    } else {
        return false;
    }
    return true;
}

bool ParseKeyPhase(const char* text, device::KeyPhase& phase_out) {
    if (text == nullptr) {
        return false;
    }
    if (std::strcmp(text, "down") == 0) {
        phase_out = device::KeyPhase::kDown;
    } else if (std::strcmp(text, "up") == 0) {
        phase_out = device::KeyPhase::kUp;
    } else if (std::strcmp(text, "repeat") == 0) {
        phase_out = device::KeyPhase::kRepeat;
    } else if (std::strcmp(text, "cancel") == 0) {
        phase_out = device::KeyPhase::kCancel;
    } else {
        return false;
    }
    return true;
}

bool ParseSha256(const char* text, std::array<uint8_t, 32U>& digest_out) {
    if (text == nullptr || std::strlen(text) != digest_out.size() * 2U) {
        return false;
    }
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    for (size_t index = 0U; index < digest_out.size(); ++index) {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest_out[index] = static_cast<uint8_t>((high << 4U) | low);
    }
    return true;
}

bool FirmwareVersionNewer(const char* candidate, const char* current) {
    unsigned candidate_major = 0U;
    unsigned candidate_minor = 0U;
    unsigned candidate_patch = 0U;
    unsigned current_major = 0U;
    unsigned current_minor = 0U;
    unsigned current_patch = 0U;
    if (candidate == nullptr || current == nullptr ||
        std::sscanf(candidate, "%u.%u.%u", &candidate_major, &candidate_minor, &candidate_patch) != 3 ||
        std::sscanf(current, "%u.%u.%u", &current_major, &current_minor, &current_patch) != 3) {
        return candidate != nullptr && current != nullptr && std::strcmp(candidate, current) != 0;
    }
    if (candidate_major != current_major) {
        return candidate_major > current_major;
    }
    if (candidate_minor != current_minor) {
        return candidate_minor > current_minor;
    }
    return candidate_patch > current_patch;
}

bool DecodeTrustedCa(std::vector<uint8_t>& certificate_out) {
    const char* encoded = CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64;
    const size_t encoded_size = std::strlen(encoded);
    if (encoded_size == 0U) {
        return false;
    }
    size_t required_size = 0U;
    (void)mbedtls_base64_decode(nullptr, 0U, &required_size, reinterpret_cast<const uint8_t*>(encoded), encoded_size);
    if (required_size == 0U) {
        return false;
    }
    certificate_out.resize(required_size);
    size_t decoded_size = 0U;
    const int result = mbedtls_base64_decode(certificate_out.data(), certificate_out.size(), &decoded_size,
                                             reinterpret_cast<const uint8_t*>(encoded), encoded_size);
    if (result != 0 || decoded_size != required_size) {
        certificate_out.clear();
        return false;
    }
    return true;
}

const char* GuestLogLevelText(uint32_t level) {
    switch (level) {
        case MICROPIXEL_LOG_DEBUG:
            return "debug";
        case MICROPIXEL_LOG_INFO:
            return "info";
        case MICROPIXEL_LOG_WARNING:
            return "warning";
        case MICROPIXEL_LOG_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

void AddMemoryStatistics(cJSON* parent, const char* name, uint32_t capabilities) {
    cJSON* memory = cJSON_AddObjectToObject(parent, name);
    if (memory == nullptr) {
        return;
    }
    (void)cJSON_AddNumberToObject(memory, "totalBytes", heap_caps_get_total_size(capabilities));
    (void)cJSON_AddNumberToObject(memory, "freeBytes", heap_caps_get_free_size(capabilities));
    (void)cJSON_AddNumberToObject(memory, "minimumFreeBytes", heap_caps_get_minimum_free_size(capabilities));
    (void)cJSON_AddNumberToObject(memory, "largestFreeBlockBytes", heap_caps_get_largest_free_block(capabilities));
}

std::vector<std::pair<std::string, std::string>> JsonHeaders(const char* credential) {
    return {{"authorization", std::string("Device ") + credential}, {"content-type", "application/json"}};
}

esp_http3::Http3Headers AsyncJsonHeaders(const char* credential) {
    esp_http3::Http3Headers headers;
    headers.emplace_back(esp_http3::Http3String("authorization"), esp_http3::Http3String("Device ") + credential);
    headers.emplace_back(esp_http3::Http3String("content-type"), esp_http3::Http3String("application/json"));
    return headers;
}

Http3Client& ClientFrom(void* client) { return *static_cast<Http3Client*>(client); }

}  // namespace

struct RemoteControlAgent::GuestLogBuffer final {
    std::array<GuestLogEntry, kGuestLogCapacity> entries{};
    std::array<char, 17U> session_id{};
    size_t start{};
    size_t count{};
    uint64_t next_sequence{1U};
};

static_assert(sizeof(GuestLogEntry) * kGuestLogCapacity < 2U * 1024U * 1024U,
              "Guest log ring must remain a bounded PSRAM allocation");

RemoteControlAgent::RemoteControlAgent(device::WifiBackend& wifi) : wifi_(wifi) {
    command_queue_ = xQueueCreateStatic(kCommandQueueCapacity, sizeof(Command), command_queue_bytes_.data(),
                                        &command_queue_storage_);
    host_command_queue_bytes_ = static_cast<uint8_t*>(heap_caps_calloc(
        kHostCommandQueueCapacity, sizeof(RemoteControlHostCommand), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    host_result_queue_bytes_ = static_cast<uint8_t*>(heap_caps_calloc(
        kHostResultQueueCapacity, sizeof(RemoteControlHostResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (host_command_queue_bytes_ == nullptr) {
        host_command_queue_bytes_ = static_cast<uint8_t*>(
            heap_caps_calloc(kHostCommandQueueCapacity, sizeof(RemoteControlHostCommand), MALLOC_CAP_8BIT));
    }
    if (host_result_queue_bytes_ == nullptr) {
        host_result_queue_bytes_ = static_cast<uint8_t*>(
            heap_caps_calloc(kHostResultQueueCapacity, sizeof(RemoteControlHostResult), MALLOC_CAP_8BIT));
    }
    if (host_command_queue_bytes_ != nullptr) {
        host_command_queue_ = xQueueCreateStatic(kHostCommandQueueCapacity, sizeof(RemoteControlHostCommand),
                                                 host_command_queue_bytes_, &host_command_queue_storage_);
    }
    if (host_result_queue_bytes_ != nullptr) {
        host_result_queue_ = xQueueCreateStatic(kHostResultQueueCapacity, sizeof(RemoteControlHostResult),
                                                host_result_queue_bytes_, &host_result_queue_storage_);
    }
    stopped_semaphore_ = xSemaphoreCreateBinaryStatic(&stopped_semaphore_storage_);
    guest_logs_ =
        static_cast<GuestLogBuffer*>(heap_caps_calloc(1U, sizeof(GuestLogBuffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (guest_logs_ != nullptr) {
        std::snprintf(guest_logs_->session_id.data(), guest_logs_->session_id.size(), "%08" PRIx32 "%08" PRIx32,
                      esp_random(), esp_random());
        guest_logs_->next_sequence = 1U;
        ESP_LOGI(kTag, "Guest log ring allocated in PSRAM: entries=%zu bytes=%zu", guest_logs_->entries.size(),
                 sizeof(GuestLogBuffer));
    } else {
        ESP_LOGW(kTag, "Guest log ring is unavailable");
    }
    CopyText(app_lifecycle_, "not_running");
    if (CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST[0] == '\0') {
        CopyText(model_.service, "Not configured");
        CopyText(model_.status_message, "Set MICROPIXEL_REMOTE_CONTROL_HOST to connect");
    } else {
        std::snprintf(model_.service.data(), model_.service.size(), "%s:%u", CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST,
                      static_cast<unsigned>(CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT));
        CopyText(model_.status_message, "Remote Control is disabled");
    }
}

RemoteControlAgent::~RemoteControlAgent() {
    Stop();
    ClearPendingResults();
    RemoteControlHostCommand pending_command{};
    while (host_command_queue_ != nullptr && xQueueReceive(host_command_queue_, &pending_command, 0U) == pdTRUE) {
        ReleaseHostCommand(pending_command);
    }
    RemoteControlHostResult pending{};
    while (host_result_queue_ != nullptr && xQueueReceive(host_result_queue_, &pending, 0U) == pdTRUE) {
        ReleaseArtifacts(pending);
    }
    if (guest_logs_ != nullptr) {
        heap_caps_free(guest_logs_);
        guest_logs_ = nullptr;
    }
    heap_caps_free(host_command_queue_bytes_);
    host_command_queue_bytes_ = nullptr;
    heap_caps_free(host_result_queue_bytes_);
    host_result_queue_bytes_ = nullptr;
}

bool RemoteControlAgent::Start(bool enabled) {
#if !CONFIG_MICROPIXEL_REMOTE_CONTROL_AGENT
    (void)enabled;
    SetConnectionState(host_ui::RemoteControlConnectionState::kDisabled, "Remote Control agent is not built");
    return false;
#else
    if (task_ != nullptr || command_queue_ == nullptr || stopped_semaphore_ == nullptr) {
        return task_ != nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.enabled = enabled;
        model_.connection_state = enabled ? host_ui::RemoteControlConnectionState::kWaitingForNetwork
                                          : host_ui::RemoteControlConnectionState::kDisabled;
    }
    (void)xSemaphoreTake(stopped_semaphore_, 0U);
    shutdown_requested_ = false;
    if (xTaskCreate(TaskEntry, "micropixel_remote", kTaskStackBytes, this, task_policy::kRemoteControlPriority,
                    &task_) != pdPASS) {
        task_ = nullptr;
        SetConnectionState(host_ui::RemoteControlConnectionState::kBackoff, "Unable to start Remote Control task");
        return false;
    }
    return true;
#endif
}

void RemoteControlAgent::Stop() {
    TaskHandle_t task = task_;
    if (task == nullptr) {
        return;
    }
    if (!QueueCommand(Command{.type = CommandType::kShutdown})) {
        ESP_LOGW(kTag, "shutdown command queue is full; discarding pending UI commands");
        (void)xQueueReset(command_queue_);
        (void)QueueCommand(Command{.type = CommandType::kShutdown});
    }
    if (xSemaphoreTake(stopped_semaphore_, pdMS_TO_TICKS(kShutdownTimeoutMs)) != pdTRUE) {
        ESP_LOGE(kTag, "Remote Control task did not stop in time; forcing deletion");
        vTaskDelete(task_);
    }
    task_ = nullptr;
}

bool RemoteControlAgent::SetEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.enabled = enabled;
        model_.connection_state = enabled ? host_ui::RemoteControlConnectionState::kWaitingForNetwork
                                          : host_ui::RemoteControlConnectionState::kDisabled;
        if (!enabled) {
            model_.pairing_code_pending = false;
            model_.pairing_code_available = false;
            model_.pairing_code.fill('\0');
            model_.pairing_expires_seconds = 0U;
            pairing_deadline_ticks_ = 0U;
        }
    }
    return QueueCommand(Command{.type = CommandType::kSetEnabled, .enabled = enabled});
}

bool RemoteControlAgent::RequestPairingCode() {
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (!model_.enabled || model_.pairing_code_pending || model_.pairing_code_available) {
        return false;
    }
    const bool queued = QueueCommand(Command{.type = CommandType::kRequestPairingCode});
    if (queued) {
        model_.pairing_code_pending = true;
        model_.pairing_code.fill('\0');
        model_.pairing_expires_seconds = 0U;
        CopyText(model_.status_message, "Requesting connection code");
    }
    ESP_LOGI(kTag, "pairing command submitted to agent queue: %s", queued ? "yes" : "no");
    return queued;
}

bool RemoteControlAgent::CancelPairingCode() {
    ClearPairingInSnapshot("Connection code cancelled");
    return QueueCommand(Command{.type = CommandType::kCancelPairingCode});
}

bool RemoteControlAgent::RequestFirmwareUpdate() {
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (!model_.firmware_update_installable ||
        model_.firmware_update_state == host_ui::FirmwareUpdateState::kDownloading ||
        model_.firmware_update_state == host_ui::FirmwareUpdateState::kVerifying ||
        model_.firmware_update_state == host_ui::FirmwareUpdateState::kInstalling) {
        return false;
    }
    const bool queued = QueueCommand(Command{.type = CommandType::kRequestFirmwareUpdate});
    if (queued) {
        CopyText(model_.firmware_update_message, "Update requested");
    }
    return queued;
}

host_ui::RemoteControlModel RemoteControlAgent::Snapshot() const {
    std::lock_guard<std::mutex> lock(model_mutex_);
    return model_;
}

void RemoteControlAgent::UpdateInstalledApps(const RemoteControlCatalogSnapshot& catalog) {
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    installed_apps_ = catalog;
    installed_apps_.count = std::min(installed_apps_.count, static_cast<uint32_t>(installed_apps_.apps.size()));
}

void RemoteControlAgent::UpdateAppLifecycle(const char* app_id, const char* lifecycle) {
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    CopyText(active_app_id_, app_id);
    CopyText(app_lifecycle_, lifecycle != nullptr ? lifecycle : "unknown");
}

void RemoteControlAgent::WriteGuestLog(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                                       uint64_t timestamp_us) {
    if (guest_logs_ == nullptr || (bytes == nullptr && length != 0U)) {
        return;
    }
    std::lock_guard<std::mutex> lock(log_mutex_);
    const size_t index = guest_logs_->count < guest_logs_->entries.size()
                             ? (guest_logs_->start + guest_logs_->count) % guest_logs_->entries.size()
                             : guest_logs_->start;
    if (guest_logs_->count < guest_logs_->entries.size()) {
        ++guest_logs_->count;
    } else {
        guest_logs_->start = (guest_logs_->start + 1U) % guest_logs_->entries.size();
    }
    GuestLogEntry& entry = guest_logs_->entries[index];
    entry = {};
    entry.sequence = guest_logs_->next_sequence++;
    entry.timestamp_us = timestamp_us;
    entry.level = level;
    std::snprintf(entry.app_id.data(), entry.app_id.size(), "%s", app_id != nullptr ? app_id : "");
    const size_t copied = std::min(length, entry.message.size() - 1U);
    if (copied != 0U) {
        std::memcpy(entry.message.data(), bytes, copied);
    }
    entry.message[copied] = '\0';
}

bool RemoteControlAgent::PollHostCommand(RemoteControlHostCommand& command, TickType_t timeout) {
    return host_command_queue_ != nullptr && xQueueReceive(host_command_queue_, &command, timeout) == pdTRUE;
}

bool RemoteControlAgent::PeekHostCommand(RemoteControlHostCommand& command) const {
    return host_command_queue_ != nullptr && xQueuePeek(host_command_queue_, &command, 0U) == pdTRUE;
}

bool RemoteControlAgent::SubmitHostResult(const RemoteControlHostResult& result) {
    if (host_result_queue_ != nullptr && xQueueSend(host_result_queue_, &result, 0U) == pdTRUE) {
        return true;
    }
    ReleaseArtifacts(result);
    return false;
}

void RemoteControlAgent::TaskEntry(void* context) {
    auto* agent = static_cast<RemoteControlAgent*>(context);
    if (agent != nullptr) {
        agent->TaskMain();
        (void)xSemaphoreGive(agent->stopped_semaphore_);
    }
    vTaskDelete(nullptr);
}

bool RemoteControlAgent::QueueCommand(const Command& command) {
    return command_queue_ != nullptr && xQueueSend(command_queue_, &command, 0U) == pdTRUE;
}

void RemoteControlAgent::SetConnectionState(host_ui::RemoteControlConnectionState state, const char* message) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    model_.connection_state = state;
    CopyText(model_.status_message, message);
}

void RemoteControlAgent::SetIdentityInSnapshot(const Identity& identity) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    model_.device_id = identity.device_id;
}

void RemoteControlAgent::ClearPairingInSnapshot(const char* message) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    model_.pairing_code.fill('\0');
    model_.pairing_code_pending = false;
    model_.pairing_code_available = false;
    model_.pairing_expires_seconds = 0U;
    pairing_deadline_ticks_ = 0U;
    if (message != nullptr) {
        CopyText(model_.status_message, message);
    }
}

void RemoteControlAgent::FinishPairingRequestWithoutCode(const char* message) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    model_.pairing_code.fill('\0');
    model_.pairing_code_pending = false;
    model_.pairing_code_available = false;
    model_.pairing_expires_seconds = 0U;
    pairing_deadline_ticks_ = 0U;
    if (message != nullptr) {
        CopyText(model_.status_message, message);
    }
}

void RemoteControlAgent::RefreshPairingDeadline() {
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (pairing_deadline_ticks_ == 0U) {
        return;
    }
    const int32_t remaining_ticks = static_cast<int32_t>(pairing_deadline_ticks_ - xTaskGetTickCount());
    if (remaining_ticks <= 0) {
        model_.pairing_code.fill('\0');
        model_.pairing_code_pending = false;
        model_.pairing_code_available = false;
        model_.pairing_expires_seconds = 0U;
        pairing_deadline_ticks_ = 0U;
        CopyText(model_.status_message, "Connection code expired");
        return;
    }
    const uint32_t remaining_ms = static_cast<uint32_t>(remaining_ticks) * portTICK_PERIOD_MS;
    model_.pairing_expires_seconds = (remaining_ms + 999U) / 1000U;
}

bool RemoteControlAgent::LoadIdentity(Identity& identity) const {
    nvs_handle_t handle{};
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    IdentityRecord record{};
    size_t size = sizeof(record);
    const esp_err_t error = nvs_get_blob(handle, kIdentityKey, &record, &size);
    nvs_close(handle);
    if (error != ESP_OK || size != sizeof(record) || !ValidIdentityRecord(record)) {
        return false;
    }
    CopyText(identity.device_id, record.device_id);
    CopyText(identity.credential, record.credential);
    identity.auth_epoch = record.auth_epoch;
    return true;
}

bool RemoteControlAgent::SaveIdentity(const Identity& identity) const {
    IdentityRecord record{.version = kIdentityVersion, .auth_epoch = identity.auth_epoch};
    std::snprintf(record.device_id, sizeof(record.device_id), "%s", identity.device_id.data());
    std::snprintf(record.credential, sizeof(record.credential), "%s", identity.credential.data());
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, kIdentityKey, &record, sizeof(record));
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to save Remote Control identity: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

bool RemoteControlAgent::ClearIdentity(Identity& identity) {
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_erase_key(handle, kIdentityKey);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to clear rejected Remote Control identity: %s", esp_err_to_name(error));
        return false;
    }

    identity = Identity{};
    SetIdentityInSnapshot(identity);
    ClearPairingInSnapshot("Device credential rejected; preparing a new identity");
    ESP_LOGW(kTag, "cleared rejected Remote Control identity; device will bootstrap again");
    return true;
}

bool RemoteControlAgent::Bootstrap(void* client, Identity& identity) {
    static constexpr uint8_t kEmptyBody[] = {'{', '}'};
    Http3Response response{};
    if (!ClientFrom(client).Post("/device/v1/bootstrap", {{"content-type", "application/json"}}, kEmptyBody,
                                 sizeof(kEmptyBody), response, kRequestTimeoutMs) ||
        response.status != 201) {
        ESP_LOGW(kTag, "device bootstrap failed: status=%d error=%s", response.status, response.error.c_str());
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
    if (root == nullptr) {
        return false;
    }
    const char* device_id = JsonString(root, "deviceId");
    const char* credential = JsonString(root, "deviceCredential");
    const uint32_t auth_epoch = JsonPositiveUint(root, "authEpoch", 1U);
    const bool valid = device_id != nullptr && credential != nullptr &&
                       std::strlen(device_id) < identity.device_id.size() &&
                       std::strlen(credential) < identity.credential.size();
    if (valid) {
        CopyText(identity.device_id, device_id);
        CopyText(identity.credential, credential);
        identity.auth_epoch = auth_epoch;
    }
    cJSON_Delete(root);
    if (!valid || !SaveIdentity(identity)) {
        return false;
    }
    SetIdentityInSnapshot(identity);
    ESP_LOGI(kTag, "bootstrapped Remote Control identity: %.8s...", identity.device_id.data());
    return true;
}

bool RemoteControlAgent::PostCommandResult(void* client, const Identity& identity, const char* command_id, bool ok,
                                           cJSON* result) {
    if (command_id == nullptr || std::strlen(command_id) >= 64U) {
        cJSON_Delete(result);
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        cJSON_Delete(result);
        return false;
    }
    (void)cJSON_AddStringToObject(root, "type", "command.result");
    (void)cJSON_AddStringToObject(root, "commandId", command_id);
    (void)cJSON_AddBoolToObject(root, "ok", ok);
    if (result != nullptr) {
        cJSON_AddItemToObject(root, "result", result);
    } else {
        (void)cJSON_AddNullToObject(root, "result");
    }
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return false;
    }
    const size_t body_size = std::strlen(body);
    if (body_size > kMaxEventBodyBytes) {
        cJSON_free(body);
        ESP_LOGW(kTag, "Remote Control result exceeds bounded event size: %zu", body_size);
        return false;
    }
    const auto* body_bytes = reinterpret_cast<const uint8_t*>(body);
    const bool posted = pending_result_count_ == 0U && SendCommandResultBody(client, identity, body_bytes, body_size);
    const bool queued = !posted && QueuePendingResult(body_bytes, body_size);
    cJSON_free(body);
    return posted || queued;
}

bool RemoteControlAgent::SendCommandResultBody(void* client, const Identity& identity, const uint8_t* body,
                                               size_t body_size) {
    if (client == nullptr || body == nullptr || body_size == 0U || body_size > kMaxEventBodyBytes) {
        return false;
    }
    const std::string path = std::string("/device/v1/devices/") + identity.device_id.data() + "/events";
    Http3Response response{};
    return ClientFrom(client).Post(path, JsonHeaders(identity.credential.data()), body, body_size, response,
                                   kRequestTimeoutMs) &&
           response.status == 202;
}

bool RemoteControlAgent::QueuePendingResult(const uint8_t* body, size_t body_size) {
    if (body == nullptr || body_size == 0U || body_size > kMaxEventBodyBytes ||
        pending_result_count_ >= pending_results_.size()) {
        ESP_LOGW(kTag, "Remote Control pending result queue is full");
        return false;
    }
    uint8_t* copy = static_cast<uint8_t*>(heap_caps_malloc(body_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        copy = static_cast<uint8_t*>(heap_caps_malloc(body_size, MALLOC_CAP_8BIT));
    }
    if (copy == nullptr) {
        ESP_LOGW(kTag, "Remote Control could not retain a pending result: bytes=%zu", body_size);
        return false;
    }
    std::memcpy(copy, body, body_size);
    const size_t index = (pending_result_start_ + pending_result_count_) % pending_results_.size();
    pending_results_[index] = PendingResultBody{.data = copy, .size = body_size};
    ++pending_result_count_;
    return true;
}

void RemoteControlAgent::FlushPendingResults(void* client, const Identity& identity) {
    while (pending_result_count_ != 0U) {
        PendingResultBody& pending = pending_results_[pending_result_start_];
        if (!SendCommandResultBody(client, identity, pending.data, pending.size)) {
            return;
        }
        heap_caps_free(pending.data);
        pending = {};
        pending_result_start_ = (pending_result_start_ + 1U) % pending_results_.size();
        --pending_result_count_;
    }
}

void RemoteControlAgent::ClearPendingResults() {
    while (pending_result_count_ != 0U) {
        PendingResultBody& pending = pending_results_[pending_result_start_];
        heap_caps_free(pending.data);
        pending = {};
        pending_result_start_ = (pending_result_start_ + 1U) % pending_results_.size();
        --pending_result_count_;
    }
}

bool RemoteControlAgent::PostUnsupportedCommandResult(void* client, const Identity& identity, const char* command_id) {
    cJSON* result = cJSON_CreateObject();
    if (result != nullptr) {
        (void)cJSON_AddStringToObject(result, "error", "not_implemented");
    }
    return PostCommandResult(client, identity, command_id, false, result);
}

bool RemoteControlAgent::PostRestartResult(void* client, const Identity& identity, const char* command_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON* result = root != nullptr ? cJSON_AddObjectToObject(root, "result") : nullptr;
    if (root == nullptr || result == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    (void)cJSON_AddStringToObject(root, "type", "command.result");
    (void)cJSON_AddStringToObject(root, "commandId", command_id);
    (void)cJSON_AddBoolToObject(root, "ok", true);
    (void)cJSON_AddStringToObject(result, "message", "device_restarting");
    (void)cJSON_AddNumberToObject(result, "delayMs", 750U);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return false;
    }
    const size_t body_size = std::strlen(body);
    const bool posted = SendCommandResultBody(client, identity, reinterpret_cast<const uint8_t*>(body), body_size);
    cJSON_free(body);
    if (!posted) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(750U));
    esp_restart();
}

bool RemoteControlAgent::PostFirmwareUpdateStatus(void* client, const Identity& identity) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    (void)cJSON_AddStringToObject(root, "type", "status");
    AddFirmwareUpdateJson(root, Snapshot());
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return false;
    }
    const size_t body_size = std::strlen(body);
    const bool posted = SendCommandResultBody(client, identity, reinterpret_cast<const uint8_t*>(body), body_size);
    cJSON_free(body);
    return posted;
}

bool RemoteControlAgent::PostSystemInformation(void* client, const Identity& identity, const char* command_id) {
    cJSON* result = cJSON_CreateObject();
    if (result == nullptr) {
        return false;
    }
    const esp_app_desc_t* description = esp_app_get_description();
    cJSON* firmware = cJSON_AddObjectToObject(result, "firmware");
    if (firmware != nullptr && description != nullptr) {
        (void)cJSON_AddStringToObject(firmware, "version", description->version);
        (void)cJSON_AddStringToObject(firmware, "buildDate", description->date);
        (void)cJSON_AddStringToObject(firmware, "buildTime", description->time);
        (void)cJSON_AddStringToObject(firmware, "idfVersion", description->idf_ver);
        char elf_sha[65]{};
        (void)esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
        (void)cJSON_AddStringToObject(firmware, "buildId", elf_sha);
    }
    const host_ui::RemoteControlModel control = Snapshot();
    AddFirmwareUpdateJson(result, control);

    cJSON* hardware = cJSON_AddObjectToObject(result, "hardware");
    if (hardware != nullptr) {
        esp_chip_info_t chip{};
        esp_chip_info(&chip);
        (void)cJSON_AddStringToObject(hardware, "chip", "ESP32-P4");
        (void)cJSON_AddNumberToObject(hardware, "revision", chip.revision);
        (void)cJSON_AddNumberToObject(hardware, "cores", chip.cores);
        (void)cJSON_AddNumberToObject(hardware, "cpuFrequencyMhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        uint32_t flash_bytes = 0U;
        if (esp_flash_get_size(nullptr, &flash_bytes) == ESP_OK) {
            (void)cJSON_AddNumberToObject(hardware, "flashBytes", flash_bytes);
        }
        cJSON* display = cJSON_AddObjectToObject(hardware, "display");
        if (display != nullptr) {
            (void)cJSON_AddStringToObject(display, "driver", "NV3051F");
            (void)cJSON_AddStringToObject(display, "interface", "MIPI-DSI 2-lane");
            (void)cJSON_AddNumberToObject(display, "widthPixels", 720U);
            (void)cJSON_AddNumberToObject(display, "heightPixels", 720U);
            (void)cJSON_AddStringToObject(display, "pixelFormat", "RGB888");
            (void)cJSON_AddNumberToObject(display, "refreshRateHz", 60U);
        }
    }

    cJSON* memory = cJSON_AddObjectToObject(result, "memory");
    if (memory != nullptr) {
        AddMemoryStatistics(memory, "internalSram", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        AddMemoryStatistics(memory, "psram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    RemoteControlCatalogSnapshot catalog{};
    std::array<char, kRemoteControlAppIdCapacity> active_app{};
    std::array<char, 24U> lifecycle{};
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        catalog = installed_apps_;
        active_app = active_app_id_;
        lifecycle = app_lifecycle_;
    }
    cJSON* storage = cJSON_AddObjectToObject(result, "storage");
    if (storage != nullptr) {
        cJSON* app_store = cJSON_AddObjectToObject(storage, "appStore");
        if (app_store != nullptr) {
            (void)cJSON_AddNumberToObject(app_store, "totalBytes", catalog.store_total_bytes);
            (void)cJSON_AddNumberToObject(app_store, "usedBytes", catalog.store_used_bytes);
            (void)cJSON_AddNumberToObject(app_store, "freeBytes",
                                          catalog.store_total_bytes >= catalog.store_used_bytes
                                              ? catalog.store_total_bytes - catalog.store_used_bytes
                                              : 0U);
        }
    }

    cJSON* runtime = cJSON_AddObjectToObject(result, "runtime");
    if (runtime != nullptr) {
        (void)cJSON_AddNumberToObject(runtime, "uptimeMs", esp_timer_get_time() / 1000);
        (void)cJSON_AddStringToObject(runtime, "currentApp", active_app.data());
        (void)cJSON_AddStringToObject(runtime, "appLifecycle", lifecycle.data());
    }

    cJSON* task_diagnostics = cJSON_AddObjectToObject(result, "taskDiagnostics");
#if configUSE_TRACE_FACILITY == 1
    std::array<TaskStatus_t, kTaskDiagnosticCapacity> task_status{};
    configRUN_TIME_COUNTER_TYPE total_runtime{};
    const UBaseType_t total_task_count = uxTaskGetNumberOfTasks();
    const UBaseType_t task_count =
        uxTaskGetSystemState(task_status.data(), static_cast<UBaseType_t>(task_status.size()), &total_runtime);
    if (task_diagnostics != nullptr) {
        (void)cJSON_AddBoolToObject(task_diagnostics, "available", true);
        (void)cJSON_AddNumberToObject(task_diagnostics, "taskCount", total_task_count);
        (void)cJSON_AddBoolToObject(task_diagnostics, "truncated", total_task_count > task_status.size());
        (void)cJSON_AddNumberToObject(task_diagnostics, "totalRuntimeCounter", total_runtime);
        cJSON* tasks = cJSON_AddArrayToObject(task_diagnostics, "tasks");
        const uint64_t current_total_runtime = static_cast<uint64_t>(total_runtime);
        const uint64_t total_delta = previous_total_runtime_ != 0U && current_total_runtime >= previous_total_runtime_
                                         ? current_total_runtime - previous_total_runtime_
                                         : 0U;
        std::array<TaskRuntimeSample, kTaskDiagnosticCapacity> current_samples{};
        for (UBaseType_t index = 0U; index < task_count; ++index) {
            const TaskStatus_t& task = task_status[index];
            const uint64_t current_runtime = static_cast<uint64_t>(task.ulRunTimeCounter);
            uint64_t previous_runtime = current_runtime;
            for (const TaskRuntimeSample& sample : previous_task_runtime_) {
                if (sample.handle == task.xHandle) {
                    previous_runtime = sample.runtime_counter;
                    break;
                }
            }
            const uint64_t runtime_delta =
                current_runtime >= previous_runtime ? current_runtime - previous_runtime : 0U;
            const double one_core_percent =
                total_delta == 0U ? 0.0
                                  : (static_cast<double>(runtime_delta) * 100.0) / static_cast<double>(total_delta);
            current_samples[index] = {.handle = task.xHandle, .runtime_counter = current_runtime};
            if (tasks == nullptr) {
                continue;
            }
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                continue;
            }
            (void)cJSON_AddStringToObject(item, "name", task.pcTaskName != nullptr ? task.pcTaskName : "unknown");
            (void)cJSON_AddNumberToObject(item, "taskNumber", task.xTaskNumber);
            (void)cJSON_AddStringToObject(item, "state", TaskStateText(task.eCurrentState));
            (void)cJSON_AddNumberToObject(item, "priority", task.uxCurrentPriority);
            (void)cJSON_AddNumberToObject(item, "basePriority", task.uxBasePriority);
            (void)cJSON_AddNumberToObject(item, "runtimeCounter", current_runtime);
            (void)cJSON_AddNumberToObject(item, "cpuPercent", one_core_percent);
            (void)cJSON_AddNumberToObject(item, "cpuCapacityPercent",
                                          one_core_percent / static_cast<double>(portNUM_PROCESSORS));
            (void)cJSON_AddNumberToObject(item, "stackHighWaterMarkBytes",
                                          static_cast<uint64_t>(task.usStackHighWaterMark) * sizeof(StackType_t));
#if (configUSE_CORE_AFFINITY == 1) && (configNUMBER_OF_CORES > 1)
            (void)cJSON_AddNumberToObject(item, "coreAffinityMask", task.uxCoreAffinityMask);
#endif
            cJSON_AddItemToArray(tasks, item);
        }
        previous_task_runtime_ = current_samples;
        previous_total_runtime_ = current_total_runtime;
    }
#else
    if (task_diagnostics != nullptr) {
        (void)cJSON_AddBoolToObject(task_diagnostics, "available", false);
        (void)cJSON_AddStringToObject(task_diagnostics, "reason", "freertos_trace_facility_disabled");
    }
#endif

    const device::WifiSnapshot wifi = wifi_.Snapshot();
    cJSON* network = cJSON_AddObjectToObject(result, "network");
    if (network != nullptr) {
        (void)cJSON_AddBoolToObject(network, "available", wifi.available);
        (void)cJSON_AddBoolToObject(network, "enabled", wifi.enabled);
        (void)cJSON_AddBoolToObject(network, "connected", wifi.connected);
        for (uint32_t index = 0U; index < wifi.saved_network_count; ++index) {
            if (wifi.saved_networks[index].connected) {
                (void)cJSON_AddStringToObject(network, "ssid", wifi.saved_networks[index].ssid.data());
                (void)cJSON_AddNumberToObject(network, "rssi", wifi.saved_networks[index].rssi);
                break;
            }
        }
        uint8_t station_mac[6]{};
        if (esp_wifi_get_mac(WIFI_IF_STA, station_mac) == ESP_OK) {
            char mac_text[18]{};
            std::snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X", station_mac[0], station_mac[1],
                          station_mac[2], station_mac[3], station_mac[4], station_mac[5]);
            (void)cJSON_AddStringToObject(network, "macAddress", mac_text);
        }
        esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info{};
        if (station != nullptr && esp_netif_get_ip_info(station, &ip_info) == ESP_OK) {
            char ip_address[16]{};
            char gateway[16]{};
            char netmask[16]{};
            std::snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip_info.ip));
            std::snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&ip_info.gw));
            std::snprintf(netmask, sizeof(netmask), IPSTR, IP2STR(&ip_info.netmask));
            (void)cJSON_AddStringToObject(network, "ipAddress", ip_address);
            (void)cJSON_AddStringToObject(network, "gateway", gateway);
            (void)cJSON_AddStringToObject(network, "netmask", netmask);
            const char* hostname = nullptr;
            if (esp_netif_get_hostname(station, &hostname) == ESP_OK && hostname != nullptr) {
                (void)cJSON_AddStringToObject(network, "hostname", hostname);
            }
            esp_netif_dns_info_t dns{};
            if (esp_netif_get_dns_info(station, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
                dns.ip.type == ESP_IPADDR_TYPE_V4) {
                char dns_address[16]{};
                std::snprintf(dns_address, sizeof(dns_address), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
                (void)cJSON_AddStringToObject(network, "dns", dns_address);
            }
        }
    }
    return PostCommandResult(client, identity, command_id, true, result);
}

bool RemoteControlAgent::PostInstalledApps(void* client, const Identity& identity, const char* command_id) {
    RemoteControlCatalogSnapshot catalog{};
    std::array<char, kRemoteControlAppIdCapacity> active_app{};
    std::array<char, 24U> lifecycle{};
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        catalog = installed_apps_;
        active_app = active_app_id_;
        lifecycle = app_lifecycle_;
    }
    cJSON* result = cJSON_CreateObject();
    cJSON* apps = result != nullptr ? cJSON_AddArrayToObject(result, "apps") : nullptr;
    if (result == nullptr || apps == nullptr) {
        cJSON_Delete(result);
        return false;
    }
    for (uint32_t index = 0U; index < catalog.count; ++index) {
        const RemoteControlAppDescriptor& app = catalog.apps[index];
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            continue;
        }
        (void)cJSON_AddStringToObject(item, "appId", app.app_id.data());
        (void)cJSON_AddStringToObject(item, "displayName", app.display_name.data());
        (void)cJSON_AddNumberToObject(item, "bundleSizeBytes", app.bundle_size);
        (void)cJSON_AddStringToObject(item, "source", "app_store");
        const bool active = active_app[0] != '\0' && active_app == app.app_id;
        (void)cJSON_AddBoolToObject(item, "active", active);
        (void)cJSON_AddStringToObject(item, "lifecycle", active ? lifecycle.data() : "not_running");
        cJSON_AddItemToArray(apps, item);
    }
    (void)cJSON_AddNumberToObject(result, "count", catalog.count);
    return PostCommandResult(client, identity, command_id, true, result);
}

cJSON* RemoteControlAgent::CreateGuestLogPayload(uint64_t after_sequence, size_t maximum_entries,
                                                 uint64_t& next_cursor_out, bool& has_entries_out) const {
    cJSON* result = cJSON_CreateObject();
    cJSON* entries = result != nullptr ? cJSON_AddArrayToObject(result, "entries") : nullptr;
    if (result == nullptr || entries == nullptr) {
        cJSON_Delete(result);
        return nullptr;
    }
    uint64_t next_cursor = after_sequence;
    bool truncated = false;
    bool has_more = false;
    size_t appended = 0U;
    size_t estimated_json_bytes = 256U;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (guest_logs_ != nullptr) {
            (void)cJSON_AddStringToObject(result, "sessionId", guest_logs_->session_id.data());
            if (guest_logs_->count != 0U) {
                const uint64_t oldest_sequence = guest_logs_->entries[guest_logs_->start].sequence;
                truncated = oldest_sequence > after_sequence && oldest_sequence - after_sequence > 1U;
            }
            for (size_t offset = 0U; offset < guest_logs_->count; ++offset) {
                const GuestLogEntry& entry =
                    guest_logs_->entries[(guest_logs_->start + offset) % guest_logs_->entries.size()];
                if (entry.sequence <= after_sequence) {
                    continue;
                }
                if (appended >= maximum_entries) {
                    has_more = true;
                    break;
                }
                const size_t entry_json_upper_bound =
                    (std::strlen(entry.app_id.data()) + std::strlen(entry.message.data())) * 6U + 256U;
                if (appended != 0U && estimated_json_bytes + entry_json_upper_bound > kGuestLogResponseJsonBudget) {
                    has_more = true;
                    break;
                }
                cJSON* item = cJSON_CreateObject();
                if (item == nullptr) {
                    continue;
                }
                (void)cJSON_AddNumberToObject(item, "sequence", static_cast<double>(entry.sequence));
                (void)cJSON_AddNumberToObject(item, "timestampUs", static_cast<double>(entry.timestamp_us));
                (void)cJSON_AddStringToObject(item, "level", GuestLogLevelText(entry.level));
                (void)cJSON_AddStringToObject(item, "appId", entry.app_id.data());
                (void)cJSON_AddStringToObject(item, "message", entry.message.data());
                cJSON_AddItemToArray(entries, item);
                next_cursor = entry.sequence;
                ++appended;
                estimated_json_bytes += entry_json_upper_bound;
            }
        }
    }
    (void)cJSON_AddNumberToObject(result, "nextCursor", static_cast<double>(next_cursor));
    (void)cJSON_AddBoolToObject(result, "truncated", truncated);
    (void)cJSON_AddBoolToObject(result, "hasMore", has_more);
    next_cursor_out = next_cursor;
    has_entries_out = appended != 0U;
    return result;
}

bool RemoteControlAgent::PostGuestLogs(void* client, const Identity& identity, const char* command_id,
                                       uint64_t after_sequence) {
    uint64_t next_cursor = after_sequence;
    bool has_entries = false;
    cJSON* result = CreateGuestLogPayload(after_sequence, kGuestLogResponseCapacity, next_cursor, has_entries);
    (void)next_cursor;
    (void)has_entries;
    if (result == nullptr) {
        return false;
    }
    return PostCommandResult(client, identity, command_id, true, result);
}

bool RemoteControlAgent::QueueHostCommand(void* client, const Identity& identity, const cJSON* root, const char* name,
                                          const char* command_id, uint32_t timeout_ms) {
    auto reject = [&](const char* error) {
        cJSON* result = cJSON_CreateObject();
        if (result != nullptr) {
            (void)cJSON_AddStringToObject(result, "error", error);
        }
        return PostCommandResult(client, identity, command_id, false, result);
    };
    if (name == nullptr || command_id == nullptr || std::strlen(command_id) >= kRemoteControlCommandIdCapacity) {
        return reject("invalid_command");
    }

    RemoteControlHostCommand command{};
    CopyText(command.command_id, command_id);
    command.deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsObject(params)) {
        params = nullptr;
    }

    if (std::strcmp(name, "capture_screen") == 0 || std::strcmp(name, "screen.capture") == 0) {
        command.type = RemoteControlHostCommandType::kCaptureScreen;
    } else if (std::strcmp(name, "app.start") == 0) {
        const char* app_id = params != nullptr ? JsonString(params, "appId") : nullptr;
        if (app_id == nullptr || app_id[0] == '\0' || std::strlen(app_id) >= command.app_id.size()) {
            return reject("invalid_app_id");
        }
        command.type = RemoteControlHostCommandType::kStartApp;
        CopyText(command.app_id, app_id);
    } else if (std::strcmp(name, "app.stop") == 0) {
        command.type = RemoteControlHostCommandType::kStopApp;
        const char* app_id = params != nullptr ? JsonString(params, "appId") : nullptr;
        if (app_id != nullptr) {
            if (std::strlen(app_id) >= command.app_id.size()) {
                return reject("invalid_app_id");
            }
            CopyText(command.app_id, app_id);
        }
    } else if (std::strcmp(name, "app.install") == 0) {
        const char* app_id = params != nullptr ? JsonString(params, "appId") : nullptr;
        const char* path = params != nullptr ? JsonString(params, "url") : nullptr;
        const char* sha256 = params != nullptr ? JsonString(params, "sha256") : nullptr;
        uint32_t package_size = 0U;
        const std::string expected_prefix =
            std::string("/device/v1/devices/") + identity.device_id.data() + "/packages/";
        if (app_id == nullptr || app_id[0] == '\0' || std::strlen(app_id) >= command.app_id.size() || path == nullptr ||
            std::strncmp(path, expected_prefix.c_str(), expected_prefix.size()) != 0 ||
            std::strchr(path + expected_prefix.size(), '/') != nullptr ||
            !JsonUint(params, "sizeBytes", kMaxPackageBytes, package_size) || package_size == 0U ||
            (package_size % MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT) != 0U || !ParseSha256(sha256, command.package_sha256)) {
            return reject("invalid_install_request");
        }
        command.type = RemoteControlHostCommandType::kInstallApp;
        CopyText(command.app_id, app_id);
        command.package_size = package_size;
        if (!DownloadPackage(client, identity, path, command.package_size, command.package_data)) {
            return reject("package_download_failed");
        }
    } else if (std::strcmp(name, "app.uninstall") == 0) {
        const char* app_id = params != nullptr ? JsonString(params, "appId") : nullptr;
        if (app_id == nullptr || app_id[0] == '\0' || std::strlen(app_id) >= command.app_id.size()) {
            return reject("invalid_app_id");
        }
        command.type = RemoteControlHostCommandType::kUninstallApp;
        CopyText(command.app_id, app_id);
    } else if (std::strcmp(name, "input.sequence") == 0) {
        const cJSON* operations = params != nullptr ? cJSON_GetObjectItemCaseSensitive(params, "operations") : nullptr;
        const int operation_count = cJSON_IsArray(operations) ? cJSON_GetArraySize(operations) : 0;
        if (operation_count <= 0 || operation_count > static_cast<int>(command.operations.size())) {
            return reject("invalid_operation_count");
        }
        uint32_t total_delay_ms = 0U;
        uint32_t capture_count = 0U;
        for (int index = 0; index < operation_count; ++index) {
            const cJSON* source = cJSON_GetArrayItem(operations, index);
            const char* type = cJSON_IsObject(source) ? JsonString(source, "type") : nullptr;
            auto& operation = command.operations[static_cast<size_t>(index)];
            if (!JsonUint(source, "delayMs", 5000U, operation.delay_ms) ||
                total_delay_ms > 10000U - operation.delay_ms) {
                return reject("invalid_sequence_delay");
            }
            total_delay_ms += operation.delay_ms;
            if (type != nullptr && std::strcmp(type, "touch") == 0) {
                uint32_t id = 0U;
                uint32_t x = 0U;
                uint32_t y = 0U;
                uint32_t pressure = 0U;
                if (!ParseTouchPhase(JsonString(source, "phase"), operation.touch.phase) ||
                    !JsonUint(source, "id", UINT32_MAX, id) || !JsonUint(source, "x", INT32_MAX, x) ||
                    !JsonUint(source, "y", INT32_MAX, y) || !JsonUint(source, "pressurePerMille", 1000U, pressure)) {
                    return reject("invalid_touch_operation");
                }
                operation.type = RemoteControlSequenceOperationType::kTouch;
                operation.touch.id = id;
                operation.touch.x = static_cast<int32_t>(x);
                operation.touch.y = static_cast<int32_t>(y);
                operation.touch.pressure_per_mille = static_cast<uint16_t>(pressure);
            } else if (type != nullptr && std::strcmp(type, "key") == 0) {
                uint32_t repeat_count = 0U;
                if (!ParseKeyCode(JsonString(source, "code"), operation.key.code) ||
                    !ParseKeyPhase(JsonString(source, "phase"), operation.key.phase) ||
                    !JsonUint(source, "repeatCount", 1000U, repeat_count) ||
                    ((operation.key.phase == device::KeyPhase::kRepeat) != (repeat_count != 0U))) {
                    return reject("invalid_key_operation");
                }
                operation.type = RemoteControlSequenceOperationType::kKey;
                operation.key.repeat_count = repeat_count;
            } else if (type != nullptr && std::strcmp(type, "screenshot") == 0) {
                const char* capture_id = JsonString(source, "id");
                if (capture_id == nullptr || capture_id[0] == '\0' ||
                    std::strlen(capture_id) >= operation.capture_id.size() ||
                    ++capture_count > kRemoteControlMaxResultArtifacts) {
                    return reject("invalid_screenshot_operation");
                }
                operation.type = RemoteControlSequenceOperationType::kCaptureScreen;
                CopyText(operation.capture_id, capture_id);
            } else {
                return reject("unsupported_sequence_operation");
            }
        }
        command.type = RemoteControlHostCommandType::kInputSequence;
        command.operation_count = static_cast<uint32_t>(operation_count);
    } else {
        return reject("not_implemented");
    }

    if (DeadlineReached(command.deadline_ticks)) {
        ReleaseHostCommand(command);
        return reject("command_expired");
    }
    if (host_command_queue_ == nullptr || xQueueSend(host_command_queue_, &command, 0U) != pdTRUE) {
        ReleaseHostCommand(command);
        return reject("device_busy");
    }
    ESP_LOGI(kTag, "Queued Host command: type=%u", static_cast<unsigned>(command.type));
    return true;
}

void RemoteControlAgent::ReleaseArtifacts(const RemoteControlHostResult& result) {
    const uint32_t count = std::min(result.artifact_count, static_cast<uint32_t>(result.artifacts.size()));
    for (uint32_t index = 0U; index < count; ++index) {
        const RemoteControlArtifact& artifact = result.artifacts[index];
        if (artifact.data != nullptr && artifact.release != nullptr) {
            artifact.release(artifact.data);
        }
    }
}

void RemoteControlAgent::ReleaseHostCommand(const RemoteControlHostCommand& command) {
    if (command.package_data != nullptr) {
        heap_caps_free(command.package_data);
    }
}

bool RemoteControlAgent::DownloadPackage(void* client, const Identity& identity, const char* path, size_t size,
                                         uint8_t*& data_out, bool report_firmware_progress,
                                         const FirmwareStatusPublisher& publish_status) {
    data_out = nullptr;
    if (client == nullptr || path == nullptr || size == 0U || size > kMaxPackageBytes) {
        return false;
    }
    Http3Request request{};
    request.method = "GET";
    request.path = path;
    constexpr char kPublicFirmwarePrefix[] = "/firmware/releases/";
    const bool public_firmware = std::strncmp(path, kPublicFirmwarePrefix, sizeof(kPublicFirmwarePrefix) - 1U) == 0;
    if (!public_firmware) {
        request.headers = {{"authorization", std::string("Device ") + identity.credential.data()}};
    }
    std::unique_ptr<Http3Stream> stream = ClientFrom(client).Open(request);
    if (!stream || stream->GetStatus(kRequestTimeoutMs) != 200) {
        return false;
    }
    uint8_t* bytes = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bytes == nullptr) {
        return false;
    }
    size_t received = 0U;
    uint32_t last_logged_percent = 0U;
    while (received < size) {
        constexpr size_t kReadChunkBytes = 16U * 1024U;
        const size_t chunk = std::min(kReadChunkBytes, size - received);
        const int count = stream->Read(bytes + received, chunk, kPackageDownloadTimeoutMs);
        if (count <= 0) {
            heap_caps_free(bytes);
            return false;
        }
        received += static_cast<size_t>(count);
        if (report_firmware_progress) {
            const uint32_t download_percent = static_cast<uint32_t>(received * 100U / size);
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                model_.firmware_processed_bytes = static_cast<uint32_t>(received);
                model_.firmware_progress_percent = static_cast<uint8_t>(download_percent * 70U / 100U);
                std::snprintf(model_.firmware_update_message.data(), model_.firmware_update_message.size(),
                              "Downloading firmware: %" PRIu32 "%%", download_percent);
            }
            if (download_percent == 100U || download_percent >= last_logged_percent + 5U) {
                last_logged_percent = download_percent;
                ESP_LOGI(kTag, "OTA download: %" PRIu32 "%% (%zu/%zu bytes)", download_percent, received, size);
                if (publish_status) {
                    publish_status();
                }
            }
        }
    }
    uint8_t extra = 0U;
    if (stream->Read(&extra, sizeof(extra), kRequestTimeoutMs) != 0) {
        heap_caps_free(bytes);
        return false;
    }
    data_out = bytes;
    return true;
}

bool RemoteControlAgent::RefreshFirmwareRelease(void* client) {
    const esp_app_desc_t* current = esp_app_get_description();
    if (client == nullptr || current == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_update_state = host_ui::FirmwareUpdateState::kChecking;
        model_.firmware_processed_bytes = 0U;
        model_.firmware_progress_percent = 0U;
        CopyText(model_.firmware_update_message, "Checking for updates");
    }
    Http3Request request{};
    request.method = "GET";
    request.path = std::string("/firmware/releases/latest?currentVersion=") + current->version;
    std::unique_ptr<Http3Stream> stream = ClientFrom(client).Open(request);
    if (!stream || stream->GetStatus(kRequestTimeoutMs) != 200) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_update_state = host_ui::FirmwareUpdateState::kUnknown;
        model_.firmware_update_available = false;
        model_.firmware_update_installable = false;
        CopyText(model_.firmware_update_message, "Update service unavailable");
        return false;
    }
    std::array<uint8_t, 4096U> response_bytes{};
    size_t received = 0U;
    for (;;) {
        if (received == response_bytes.size()) {
            return false;
        }
        const int count =
            stream->Read(response_bytes.data() + received, response_bytes.size() - received, kRequestTimeoutMs);
        if (count < 0) {
            return false;
        }
        if (count == 0) {
            break;
        }
        received += static_cast<size_t>(count);
    }
    cJSON* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(response_bytes.data()), received);
    const char* version = root != nullptr ? JsonString(root, "version") : nullptr;
    const char* path = root != nullptr ? JsonString(root, "downloadUrl") : nullptr;
    const char* sha256 = root != nullptr ? JsonString(root, "sha256") : nullptr;
    uint32_t size = 0U;
    std::array<uint8_t, 32U> digest{};
    const cJSON* available_item = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "updateAvailable") : nullptr;
    const cJSON* installable_item = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "installable") : nullptr;
    const bool available = cJSON_IsTrue(available_item);
    const bool installable = cJSON_IsTrue(installable_item);
    constexpr char kExpectedPrefix[] = "/firmware/releases/";
    const bool valid = version != nullptr && std::strlen(version) < host_ui::kFirmwareVersionTextCapacity &&
                       path != nullptr && std::strlen(path) < firmware_download_path_.size() &&
                       std::strncmp(path, kExpectedPrefix, sizeof(kExpectedPrefix) - 1U) == 0 &&
                       std::strchr(path + sizeof(kExpectedPrefix) - 1U, '/') == nullptr &&
                       JsonUint(root, "sizeBytes", kMaxFirmwareBytes, size) && size != 0U &&
                       ParseSha256(sha256, digest);
    if (valid) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        CopyText(model_.latest_firmware_version, version);
        model_.firmware_size_bytes = size;
        model_.firmware_update_available = available && FirmwareVersionNewer(version, current->version);
        model_.firmware_update_installable =
            installable && (model_.firmware_update_available || std::strcmp(version, current->version) == 0);
        model_.firmware_update_state = model_.firmware_update_available ? host_ui::FirmwareUpdateState::kAvailable
                                                                        : host_ui::FirmwareUpdateState::kCurrent;
        model_.firmware_processed_bytes = 0U;
        model_.firmware_progress_percent = model_.firmware_update_available ? 0U : 100U;
        CopyText(model_.firmware_update_message,
                 model_.firmware_update_available ? "Firmware update available" : "Firmware is up to date");
        CopyText(firmware_download_path_, path);
        firmware_sha256_ = digest;
        firmware_size_ = size;
    }
    cJSON_Delete(root);
    if (!valid) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_update_state = host_ui::FirmwareUpdateState::kFailed;
        model_.firmware_update_available = false;
        model_.firmware_update_installable = false;
        CopyText(model_.firmware_update_message, "Invalid firmware release metadata");
    }
    return valid;
}

bool RemoteControlAgent::ApplyFirmwareUpdate(void* client, const Identity& identity, const cJSON* params,
                                             const char* command_id, const FirmwareStatusPublisher& publish_status) {
    auto finish = [&](bool ok, const char* message) {
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            model_.firmware_update_state =
                ok ? host_ui::FirmwareUpdateState::kInstalling : host_ui::FirmwareUpdateState::kFailed;
            CopyText(model_.firmware_update_message, message);
        }
        if (publish_status) {
            publish_status();
        } else if (!PostFirmwareUpdateStatus(client, identity)) {
            ESP_LOGW(kTag, "OTA completion state event could not be delivered");
        }
        if (command_id == nullptr) {
            return true;
        }
        cJSON* result = cJSON_CreateObject();
        if (result != nullptr) {
            (void)cJSON_AddStringToObject(result, ok ? "message" : "error", message);
        }
        return PostCommandResult(client, identity, command_id, ok, result);
    };

    const char* version = cJSON_IsObject(params) ? JsonString(params, "version") : nullptr;
    const char* path = cJSON_IsObject(params) ? JsonString(params, "url") : nullptr;
    const char* sha256 = cJSON_IsObject(params) ? JsonString(params, "sha256") : nullptr;
    uint32_t size = 0U;
    std::array<uint8_t, 32U> expected_digest{};
    constexpr char kPublicFirmwarePrefix[] = "/firmware/releases/";
    const std::string device_firmware_prefix =
        std::string("/device/v1/devices/") + identity.device_id.data() + "/firmware/";
    const bool public_path = path != nullptr &&
                             std::strncmp(path, kPublicFirmwarePrefix, sizeof(kPublicFirmwarePrefix) - 1U) == 0 &&
                             std::strchr(path + sizeof(kPublicFirmwarePrefix) - 1U, '/') == nullptr;
    const bool device_path = path != nullptr &&
                             std::strncmp(path, device_firmware_prefix.c_str(), device_firmware_prefix.size()) == 0 &&
                             std::strchr(path + device_firmware_prefix.size(), '/') == nullptr;
    if (version == nullptr || std::strlen(version) >= host_ui::kFirmwareVersionTextCapacity || path == nullptr ||
        (!public_path && !device_path) || !JsonUint(params, "sizeBytes", kMaxFirmwareBytes, size) || size == 0U ||
        !ParseSha256(sha256, expected_digest)) {
        return finish(false, "firmware_image_invalid");
    }

    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_update_state = host_ui::FirmwareUpdateState::kDownloading;
        model_.firmware_processed_bytes = 0U;
        model_.firmware_progress_percent = 0U;
        CopyText(model_.firmware_update_message, "Downloading firmware");
    }
    ESP_LOGI(kTag, "OTA download started: version=%s bytes=%" PRIu32, version, size);
    if (publish_status) {
        publish_status();
    }
    uint8_t* image = nullptr;
    if (!DownloadPackage(client, identity, path, size, image, true, publish_status)) {
        return finish(false, "firmware_download_failed");
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_update_state = host_ui::FirmwareUpdateState::kVerifying;
        model_.firmware_progress_percent = 75U;
        CopyText(model_.firmware_update_message, "Verifying firmware");
    }
    ESP_LOGI(kTag, "OTA download complete; verifying SHA-256");
    if (publish_status) {
        publish_status();
    }
    std::array<uint8_t, 32U> actual_digest{};
    size_t digest_size = 0U;
    const bool hash_ok = psa_crypto_init() == PSA_SUCCESS &&
                         psa_hash_compute(PSA_ALG_SHA_256, image, size, actual_digest.data(), actual_digest.size(),
                                          &digest_size) == PSA_SUCCESS &&
                         digest_size == actual_digest.size() && actual_digest == expected_digest;
    if (!hash_ok) {
        heap_caps_free(image);
        return finish(false, "firmware_hash_mismatch");
    }

    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_update_state = host_ui::FirmwareUpdateState::kInstalling;
        model_.firmware_progress_percent = 80U;
        CopyText(model_.firmware_update_message, "Installing firmware");
    }
    ESP_LOGI(kTag, "OTA image verified; writing inactive partition");
    if (publish_status) {
        publish_status();
    }

    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t handle = 0U;
    esp_err_t error = partition != nullptr ? esp_ota_begin(partition, size, &handle) : ESP_ERR_NOT_FOUND;
    constexpr size_t kOtaWriteChunkBytes = 16U * 1024U;
    uint32_t last_write_logged_percent = 0U;
    for (size_t offset = 0U; error == ESP_OK && offset < size; offset += kOtaWriteChunkBytes) {
        const size_t chunk = std::min(kOtaWriteChunkBytes, static_cast<size_t>(size) - offset);
        error = esp_ota_write(handle, image + offset, chunk);
        if (error == ESP_OK) {
            const size_t written = offset + chunk;
            const uint32_t write_percent = static_cast<uint32_t>(written * 100U / size);
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                model_.firmware_progress_percent = static_cast<uint8_t>(80U + write_percent * 19U / 100U);
                std::snprintf(model_.firmware_update_message.data(), model_.firmware_update_message.size(),
                              "Installing firmware: %" PRIu32 "%%", write_percent);
            }
            if (write_percent == 100U || write_percent >= last_write_logged_percent + 10U) {
                last_write_logged_percent = write_percent;
                ESP_LOGI(kTag, "OTA flash: %" PRIu32 "%% (%zu/%" PRIu32 " bytes)", write_percent, written, size);
                if (publish_status) {
                    publish_status();
                }
            }
        }
    }
    heap_caps_free(image);
    if (error == ESP_OK) {
        error = esp_ota_end(handle);
    } else if (handle != 0U) {
        (void)esp_ota_abort(handle);
    }
    esp_app_desc_t installed{};
    if (error == ESP_OK) {
        error = esp_ota_get_partition_description(partition, &installed);
    }
    if (error == ESP_OK && std::strcmp(installed.version, version) != 0) {
        ESP_LOGW(kTag, "OTA release version differs from image version: release=%s image=%s", version,
                 installed.version);
    }
    if (error == ESP_OK) {
        error = esp_ota_set_boot_partition(partition);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "firmware OTA failed: %s", esp_err_to_name(error));
        return finish(false, error == ESP_ERR_OTA_VALIDATE_FAILED || error == ESP_ERR_INVALID_VERSION
                                 ? "firmware_image_invalid"
                                 : "firmware_flash_failed");
    }
    if (!finish(true, "firmware_update_started")) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.firmware_progress_percent = 100U;
        CopyText(model_.firmware_update_message, "Restarting to finish update");
    }
    ESP_LOGI(kTag, "OTA image installed; restarting into image version=%s (release=%s)", installed.version, version);
    if (publish_status) {
        publish_status();
    }
    vTaskDelay(pdMS_TO_TICKS(750U));
    esp_restart();
}

bool RemoteControlAgent::UploadArtifact(void* client, const Identity& identity, const RemoteControlArtifact& artifact,
                                        cJSON* artifacts) {
    if (artifact.data == nullptr || artifact.size == 0U || artifact.release == nullptr || artifacts == nullptr) {
        return false;
    }
    const std::string path = std::string("/device/v1/devices/") + identity.device_id.data() + "/artifacts";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"authorization", std::string("Device ") + identity.credential.data()}, {"content-type", "image/jpeg"}};
    Http3Response response{};
    if (!ClientFrom(client).Post(path, headers, artifact.data, artifact.size, response, kRequestTimeoutMs) ||
        response.status != 201) {
        return false;
    }
    cJSON* upload = cJSON_ParseWithLength(response.body.data(), response.body.size());
    const char* url = upload != nullptr ? JsonString(upload, "url") : nullptr;
    if (url == nullptr) {
        cJSON_Delete(upload);
        return false;
    }
    cJSON* item = cJSON_CreateObject();
    if (item != nullptr) {
        (void)cJSON_AddStringToObject(item, "id", artifact.capture_id.data());
        (void)cJSON_AddStringToObject(item, "url", url);
        (void)cJSON_AddNumberToObject(item, "width", artifact.width);
        (void)cJSON_AddNumberToObject(item, "height", artifact.height);
        (void)cJSON_AddNumberToObject(item, "sizeBytes", static_cast<double>(artifact.size));
        cJSON_AddItemToArray(artifacts, item);
    }
    cJSON_Delete(upload);
    return item != nullptr;
}

void RemoteControlAgent::DrainHostResults(void* client, const Identity& identity) {
    RemoteControlHostResult host_result{};
    while (pending_result_count_ < pending_results_.size() && host_result_queue_ != nullptr &&
           xQueueReceive(host_result_queue_, &host_result, 0U) == pdTRUE) {
        cJSON* result = cJSON_CreateObject();
        cJSON* artifacts = result != nullptr ? cJSON_AddArrayToObject(result, "screenshots") : nullptr;
        bool ok = host_result.ok && result != nullptr && artifacts != nullptr;
        if (result != nullptr && host_result.message[0] != '\0') {
            (void)cJSON_AddStringToObject(result, "message", host_result.message.data());
        }
        const uint32_t count =
            std::min(host_result.artifact_count, static_cast<uint32_t>(host_result.artifacts.size()));
        ESP_LOGI(kTag, "Draining Host result: ok=%u artifacts=%" PRIu32, host_result.ok ? 1U : 0U, count);
        for (uint32_t index = 0U; index < count; ++index) {
            if (!UploadArtifact(client, identity, host_result.artifacts[index], artifacts)) {
                ESP_LOGW(kTag, "Screen artifact upload failed: bytes=%zu", host_result.artifacts[index].size);
                ok = false;
            }
        }
        if (!ok && result != nullptr) {
            (void)cJSON_AddStringToObject(result, "error",
                                          host_result.ok ? "artifact_upload_failed" : host_result.message.data());
        }
        ReleaseArtifacts(host_result);
        if (!PostCommandResult(client, identity, host_result.command_id.data(), ok, result)) {
            break;
        }
    }
}

void RemoteControlAgent::HandleControlBytes(void* client, const Identity& identity, const uint8_t* bytes, size_t size,
                                            const FirmwareStatusPublisher& publish_status) {
    for (size_t index = 0U; index < size; ++index) {
        const char character = static_cast<char>(bytes[index]);
        if (character == '\n') {
            if (!control_line_overflow_ && control_line_size_ != 0U) {
                control_line_[control_line_size_] = '\0';
                HandleControlLine(client, identity, control_line_.data(), publish_status);
            }
            control_line_size_ = 0U;
            control_line_overflow_ = false;
            continue;
        }
        if (control_line_overflow_) {
            continue;
        }
        if (control_line_size_ + 1U >= control_line_.size()) {
            control_line_overflow_ = true;
            continue;
        }
        control_line_[control_line_size_++] = character;
    }
}

bool RemoteControlAgent::RememberCommandId(const char* command_id) {
    if (command_id == nullptr || command_id[0] == '\0' || std::strlen(command_id) >= kRemoteControlCommandIdCapacity) {
        return false;
    }
    for (size_t offset = 0U; offset < recent_command_count_; ++offset) {
        const size_t index = (recent_command_start_ + offset) % recent_command_ids_.size();
        if (std::strcmp(recent_command_ids_[index].data(), command_id) == 0) {
            return false;
        }
    }

    size_t index = 0U;
    if (recent_command_count_ < recent_command_ids_.size()) {
        index = (recent_command_start_ + recent_command_count_) % recent_command_ids_.size();
        ++recent_command_count_;
    } else {
        index = recent_command_start_;
        recent_command_start_ = (recent_command_start_ + 1U) % recent_command_ids_.size();
    }
    CopyText(recent_command_ids_[index], command_id);
    return true;
}

void RemoteControlAgent::HandleControlLine(void* client, const Identity& identity, const char* line,
                                           const FirmwareStatusPublisher& publish_status) {
    cJSON* root = cJSON_Parse(line);
    if (root == nullptr) {
        return;
    }
    const char* type = JsonString(root, "type");
    if (type != nullptr && std::strcmp(type, "command") == 0) {
        const char* command_id = JsonString(root, "commandId");
        const char* name = JsonString(root, "name");
        const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
        uint32_t timeout_ms = 0U;
        if (command_id == nullptr || command_id[0] == '\0' ||
            std::strlen(command_id) >= kRemoteControlCommandIdCapacity) {
            ESP_LOGW(kTag, "Ignored control frame with invalid command id");
        } else if (!RememberCommandId(command_id)) {
            ESP_LOGI(kTag, "Ignored duplicate command after control stream reconnect");
        } else if (!JsonUint(root, "timeoutMs", kMaxCommandTimeoutMs, timeout_ms, kDefaultCommandTimeoutMs) ||
                   timeout_ms < kMinCommandTimeoutMs) {
            cJSON* result = cJSON_CreateObject();
            if (result != nullptr) {
                (void)cJSON_AddStringToObject(result, "error", "invalid_command_timeout");
            }
            (void)PostCommandResult(client, identity, command_id, false, result);
        } else if (name != nullptr &&
                   (std::strcmp(name, "device.get_info") == 0 || std::strcmp(name, "get_system_info") == 0)) {
            (void)PostSystemInformation(client, identity, command_id);
        } else if (name != nullptr && (std::strcmp(name, "app.list") == 0 || std::strcmp(name, "list_apps") == 0)) {
            (void)PostInstalledApps(client, identity, command_id);
        } else if (name != nullptr && std::strcmp(name, "device.ota") == 0) {
            (void)ApplyFirmwareUpdate(client, identity, params, command_id, publish_status);
        } else if (name != nullptr && std::strcmp(name, "device.reboot") == 0) {
            (void)PostRestartResult(client, identity, command_id);
        } else if (name != nullptr && (std::strcmp(name, "logs.snapshot") == 0 || std::strcmp(name, "logs.get") == 0 ||
                                       std::strcmp(name, "export_logs") == 0)) {
            const uint64_t after_sequence =
                cJSON_IsObject(params) ? JsonNonNegativeUint64(params, "afterSequence", 0U) : 0U;
            (void)PostGuestLogs(client, identity, command_id, after_sequence);
        } else if (name != nullptr &&
                   (std::strcmp(name, "capture_screen") == 0 || std::strcmp(name, "screen.capture") == 0 ||
                    std::strcmp(name, "app.start") == 0 || std::strcmp(name, "app.stop") == 0 ||
                    std::strcmp(name, "app.install") == 0 || std::strcmp(name, "app.uninstall") == 0 ||
                    std::strcmp(name, "input.sequence") == 0)) {
            (void)QueueHostCommand(client, identity, root, name, command_id, timeout_ms);
        } else {
            (void)PostUnsupportedCommandResult(client, identity, command_id);
        }
    }
    cJSON_Delete(root);
}

void RemoteControlAgent::TaskMain() {
    Identity identity{};
    if (LoadIdentity(identity)) {
        SetIdentityInSnapshot(identity);
    }

    std::unique_ptr<Http3AsyncClient> async_client;
    std::unique_ptr<Http3Client> client;
    std::unique_ptr<Http3Stream> control_stream;
    Http3AsyncRequestHandle status_request{};
    Http3AsyncRequestHandle pairing_request{};
    Http3AsyncRequestHandle pairing_cancel_request{};
    std::array<uint8_t, 1024U> read_buffer{};
    TickType_t next_firmware_check_ticks = 0U;
    ReconnectBackoff reconnect_backoff;

    auto close_transport = [&]() {
        const bool retry_pairing = pairing_request.valid() && Snapshot().pairing_code_pending;
        if (async_client) {
            async_client->Stop();
        }
        status_request = {};
        pairing_request = {};
        pairing_cancel_request = {};
        if (control_stream) {
            control_stream->Close();
            control_stream.reset();
        }
        client.reset();
        if (async_client) {
            async_client->Disconnect();
            async_client.reset();
        }
        control_line_size_ = 0U;
        control_line_overflow_ = false;
        pairing_requested_ = pairing_requested_ || retry_pairing;
        next_firmware_check_ticks = 0U;
    };

    auto start_status_request = [&]() {
        if (!async_client) {
            return false;
        }

        std::array<char, kRemoteControlAppIdCapacity> active_app{};
        std::array<char, 24U> lifecycle{};
        {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            active_app = active_app_id_;
            lifecycle = app_lifecycle_;
        }
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) {
            return false;
        }
        (void)cJSON_AddStringToObject(root, "type", "status");
        (void)cJSON_AddNumberToObject(root, "protocolVersion", 1);
        (void)cJSON_AddStringToObject(root, "currentApp", active_app[0] != '\0' ? active_app.data() : "System Shell");
        (void)cJSON_AddStringToObject(root, "appLifecycle", lifecycle.data());
        (void)cJSON_AddStringToObject(root, "network", "Wi-Fi");
        AddFirmwareUpdateJson(root, Snapshot());
        char* body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (body == nullptr) {
            return false;
        }

        Http3AsyncRequest request{};
        request.method = "POST";
        request.path = std::string("/device/v1/devices/") + identity.device_id.data() + "/events";
        request.headers = AsyncJsonHeaders(identity.credential.data());
        const auto* body_bytes = reinterpret_cast<const uint8_t*>(body);
        request.body.assign(body_bytes, body_bytes + std::strlen(body));
        request.timeout_ms = kRequestTimeoutMs;
        request.max_response_body_size = 1024U;
        status_request = async_client->Submit(std::move(request));
        cJSON_free(body);
        return status_request.valid();
    };

    auto poll_async_completions = [&]() {
        if (!async_client) {
            return;
        }
        Http3AsyncResult result{};
        while (async_client->PollCompletion(result)) {
            if (result.handle.value == status_request.value) {
                if (result.outcome != Http3AsyncOutcome::kSucceeded || result.status != 202) {
                    ESP_LOGW(kTag, "Remote Control status event failed asynchronously: status=%d error=%s",
                             result.status, result.error.c_str());
                }
                status_request = {};
                continue;
            }
            if (result.handle.value == pairing_request.value) {
                const bool transport_ok = result.outcome == Http3AsyncOutcome::kSucceeded && result.status == 201;
                cJSON* root = transport_ok ? cJSON_ParseWithLength(reinterpret_cast<const char*>(result.body.data()),
                                                                   result.body.size())
                                           : nullptr;
                const char* code = root != nullptr ? JsonString(root, "code") : nullptr;
                const bool valid = code != nullptr && std::strlen(code) < model_.pairing_code.size();
                if (valid) {
                    std::lock_guard<std::mutex> lock(model_mutex_);
                    CopyText(model_.pairing_code, code);
                    model_.pairing_code_pending = false;
                    model_.pairing_code_available = true;
                    model_.pairing_expires_seconds = kPairingTtlMs / 1000U;
                    pairing_deadline_ticks_ = xTaskGetTickCount() + pdMS_TO_TICKS(kPairingTtlMs);
                    CopyText(model_.status_message, "Enter this code in the Control Console");
                } else {
                    FinishPairingRequestWithoutCode("Unable to create connection code");
                }
                if (root != nullptr) {
                    cJSON_Delete(root);
                }
                ESP_LOGI(kTag, "pairing async request completed: status=%d valid=%s", result.status,
                         valid ? "yes" : "no");
                pairing_request = {};
                continue;
            }
            if (result.handle.value == pairing_cancel_request.value) {
                if (result.outcome != Http3AsyncOutcome::kSucceeded || (result.status != 204 && result.status != 404)) {
                    ESP_LOGW(kTag, "pairing cancellation failed asynchronously: status=%d error=%s", result.status,
                             result.error.c_str());
                }
                pairing_cancel_request = {};
            }
        }
    };

    auto publish_firmware_status = [&]() {
        poll_async_completions();
        if (!status_request.valid()) {
            (void)start_status_request();
        }
    };

    auto process_command = [&](const Command& command) {
        switch (command.type) {
            case CommandType::kSetEnabled:
                if (command.enabled) {
                    reconnect_backoff.Reset();
                } else {
                    close_transport();
                    pairing_requested_ = false;
                    pairing_cancel_requested_ = false;
                    ClearPairingInSnapshot("Remote Control is disabled");
                }
                break;
            case CommandType::kRequestPairingCode:
                ESP_LOGI(kTag, "pairing command dequeued by agent");
                pairing_requested_ = true;
                pairing_cancel_requested_ = false;
                break;
            case CommandType::kCancelPairingCode:
                pairing_requested_ = false;
                pairing_cancel_requested_ = true;
                break;
            case CommandType::kRequestFirmwareUpdate:
                firmware_update_requested_ = true;
                break;
            case CommandType::kShutdown:
                shutdown_requested_ = true;
                break;
        }
    };

    auto wait_before_retry = [&](host_ui::RemoteControlConnectionState state, const char* reason) {
        const uint32_t delay_ms = reconnect_backoff.NextDelayMs(esp_random());
        std::array<char, 128U> message{};
        std::snprintf(message.data(), message.size(), "%s; retrying in %" PRIu32 " ms", reason, delay_ms);
        SetConnectionState(state, message.data());
        ESP_LOGW(kTag, "%s; retrying in %" PRIu32 " ms", reason, delay_ms);

        const TickType_t deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
        while (!shutdown_requested_) {
            const TickType_t now_ticks = xTaskGetTickCount();
            const int32_t remaining_ticks = static_cast<int32_t>(deadline_ticks - now_ticks);
            if (remaining_ticks <= 0) {
                break;
            }
            Command command{};
            if (xQueueReceive(command_queue_, &command, static_cast<TickType_t>(remaining_ticks)) == pdTRUE) {
                process_command(command);
                if (!Snapshot().enabled) {
                    break;
                }
            }
        }
    };

    auto perform_requested_firmware_update = [&]() {
        if (!firmware_update_requested_ || !client) {
            return;
        }
        firmware_update_requested_ = false;
        host_ui::RemoteControlModel update = Snapshot();
        if (update.firmware_update_installable && firmware_download_path_[0] != '\0' && firmware_size_ != 0U) {
            std::array<char, 65U> sha256{};
            for (size_t index = 0U; index < firmware_sha256_.size(); ++index) {
                std::snprintf(sha256.data() + index * 2U, 3U, "%02x", firmware_sha256_[index]);
            }
            cJSON* params = cJSON_CreateObject();
            if (params != nullptr) {
                (void)cJSON_AddStringToObject(params, "version", update.latest_firmware_version.data());
                (void)cJSON_AddStringToObject(params, "url", firmware_download_path_.data());
                (void)cJSON_AddStringToObject(params, "sha256", sha256.data());
                (void)cJSON_AddNumberToObject(params, "sizeBytes", static_cast<double>(firmware_size_));
                (void)ApplyFirmwareUpdate(client.get(), identity, params, nullptr, publish_firmware_status);
                cJSON_Delete(params);
            }
        } else {
            std::lock_guard<std::mutex> lock(model_mutex_);
            model_.firmware_update_state = host_ui::FirmwareUpdateState::kFailed;
            CopyText(model_.firmware_update_message, "No firmware update is available");
        }
    };

    while (!shutdown_requested_) {
        Command command{};
        while (xQueueReceive(command_queue_, &command, 0U) == pdTRUE) {
            process_command(command);
        }
        if (shutdown_requested_) {
            break;
        }

        RefreshPairingDeadline();
        const host_ui::RemoteControlModel snapshot = Snapshot();
        if (CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST[0] == '\0') {
            if (snapshot.enabled) {
                SetConnectionState(host_ui::RemoteControlConnectionState::kBackoff,
                                   "Control service is not configured");
            }
            vTaskDelay(kOfflinePollTicks);
            continue;
        }
        if (!kAllowUnverifiedTls && CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64[0] == '\0') {
            if (snapshot.enabled) {
                SetConnectionState(host_ui::RemoteControlConnectionState::kAuthenticationError,
                                   "Control CA certificate is not configured");
            }
            vTaskDelay(kOfflinePollTicks);
            continue;
        }
        const device::WifiSnapshot wifi = wifi_.Snapshot();
        if (!wifi.connected) {
            close_transport();
            if (snapshot.enabled) {
                SetConnectionState(host_ui::RemoteControlConnectionState::kWaitingForNetwork, "Waiting for Wi-Fi");
            }
            vTaskDelay(kOfflinePollTicks);
            continue;
        }
        const std::time_t current_time = std::time(nullptr);
        if (!kAllowUnverifiedTls && !system_time::IsTrustedUtcTime(current_time)) {
            close_transport();
            if (snapshot.enabled) {
                SetConnectionState(host_ui::RemoteControlConnectionState::kBackoff, "Waiting for trusted network time");
            }
            vTaskDelay(kOfflinePollTicks);
            continue;
        }

        if (!client) {
            Http3AsyncClientConfig config{};
            config.hostname = CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST;
            config.port = static_cast<uint16_t>(CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT);
            config.connect_timeout_ms = kRequestTimeoutMs;
            config.request_timeout_ms = kRequestTimeoutMs;
            config.idle_timeout_ms = 60000U;
            config.receive_buffer_size = 16U * 1024U;
            config.max_concurrent_requests = kMaxConcurrentAuxiliaryRequests;
            config.max_request_body_size = 1024U;
            config.default_max_response_body_size = 64U * 1024U;
            config.allow_unverified_peer = kAllowUnverifiedTls;
            if (CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64[0] != '\0' &&
                !DecodeTrustedCa(config.trusted_ca_der)) {
                if (snapshot.enabled) {
                    SetConnectionState(host_ui::RemoteControlConnectionState::kAuthenticationError,
                                       "Control CA certificate is invalid");
                }
                vTaskDelay(kOfflinePollTicks);
                continue;
            }
            if (snapshot.enabled) {
                SetConnectionState(host_ui::RemoteControlConnectionState::kConnecting, "Connecting to Control service");
            }
            async_client = std::make_unique<Http3AsyncClient>(config);
            client = std::make_unique<Http3Client>(*async_client);
        }

        if (!snapshot.enabled) {
            const TickType_t now_ticks = xTaskGetTickCount();
            if (next_firmware_check_ticks == 0U || static_cast<int32_t>(now_ticks - next_firmware_check_ticks) >= 0) {
                (void)RefreshFirmwareRelease(client.get());
                next_firmware_check_ticks = xTaskGetTickCount() + kFirmwareCheckIntervalTicks;
            }
            perform_requested_firmware_update();
            SetConnectionState(host_ui::RemoteControlConnectionState::kDisabled, "Remote Control is disabled");
            if (xQueueReceive(command_queue_, &command, kOfflinePollTicks) == pdTRUE) {
                process_command(command);
            }
            continue;
        }

        if (identity.device_id[0] == '\0' && !Bootstrap(client.get(), identity)) {
            const std::string error = client->GetLastError();
            close_transport();
            if (error.find("TLS") != std::string::npos) {
                wait_before_retry(host_ui::RemoteControlConnectionState::kAuthenticationError,
                                  "Control service TLS verification failed");
            } else {
                wait_before_retry(host_ui::RemoteControlConnectionState::kBackoff, "Device bootstrap failed");
            }
            continue;
        }

        if (!control_stream) {
            Http3Request request{};
            request.method = "GET";
            request.path = std::string("/device/v1/devices/") + identity.device_id.data() + "/control";
            request.headers = JsonHeaders(identity.credential.data());
            control_stream = async_client->Open(request);
            const int status = control_stream ? control_stream->GetStatus(kRequestTimeoutMs) : -1;
            if (status != 200) {
                close_transport();
                ESP_LOGW(kTag, "control stream request failed: status=%d", status);
                if (ShouldResetIdentityForControlStatus(status)) {
                    if (ClearIdentity(identity)) {
                        wait_before_retry(host_ui::RemoteControlConnectionState::kAuthenticationError,
                                          "Device credential rejected; identity cleared");
                    } else {
                        wait_before_retry(host_ui::RemoteControlConnectionState::kAuthenticationError,
                                          "Device credential rejected; identity clear failed");
                    }
                } else {
                    wait_before_retry(host_ui::RemoteControlConnectionState::kBackoff, "Control stream failed");
                }
                continue;
            }
            SetConnectionState(host_ui::RemoteControlConnectionState::kConnected, "Connected to Control service");
            if (!async_client->Start()) {
                close_transport();
                wait_before_retry(host_ui::RemoteControlConnectionState::kBackoff,
                                  "Event-driven HTTP/3 requests failed to start");
                continue;
            }
            (void)start_status_request();
            next_firmware_check_ticks = 0U;
        }

        poll_async_completions();

        if (pairing_cancel_requested_) {
            if (pairing_request.valid()) {
                (void)async_client->Cancel(pairing_request);
                pairing_request = {};
            }
            Http3AsyncRequest request{};
            request.method = "DELETE";
            request.path = std::string("/device/v1/devices/") + identity.device_id.data() + "/pairings/current";
            request.headers = AsyncJsonHeaders(identity.credential.data());
            request.timeout_ms = kRequestTimeoutMs;
            request.max_response_body_size = 1024U;
            pairing_cancel_request = async_client->Submit(std::move(request));
            if (!pairing_cancel_request.valid()) {
                ESP_LOGW(kTag, "pairing cancellation could not enter the async request queue");
            }
            pairing_cancel_requested_ = false;
        }
        if (pairing_requested_) {
            ESP_LOGI(kTag, "pairing request dispatch reached HTTP stage");
            if (!pairing_request.valid()) {
                static constexpr uint8_t kEmptyBody[] = {'{', '}'};
                Http3AsyncRequest request{};
                request.method = "POST";
                request.path = std::string("/device/v1/devices/") + identity.device_id.data() + "/pairings";
                request.headers = AsyncJsonHeaders(identity.credential.data());
                request.body.assign(kEmptyBody, kEmptyBody + sizeof(kEmptyBody));
                request.timeout_ms = kRequestTimeoutMs;
                request.max_response_body_size = 2048U;
                pairing_request = async_client->Submit(std::move(request));
                ESP_LOGI(kTag, "pairing request submitted asynchronously: %s", pairing_request.valid() ? "yes" : "no");
                if (!pairing_request.valid()) {
                    FinishPairingRequestWithoutCode("Unable to create connection code");
                }
            }
            pairing_requested_ = false;
        }

        const TickType_t now_ticks = xTaskGetTickCount();
        if (next_firmware_check_ticks == 0U || static_cast<int32_t>(now_ticks - next_firmware_check_ticks) >= 0) {
            (void)RefreshFirmwareRelease(client.get());
            next_firmware_check_ticks = xTaskGetTickCount() + kFirmwareCheckIntervalTicks;
        }
        perform_requested_firmware_update();

        FlushPendingResults(client.get(), identity);
        DrainHostResults(client.get(), identity);

        const int64_t control_read_started_us = esp_timer_get_time();
        const int bytes_read = control_stream->Read(read_buffer.data(), read_buffer.size(), kControlReadTimeoutMs);
        const int64_t control_read_elapsed_ms = (esp_timer_get_time() - control_read_started_us) / 1000;
        if (control_read_elapsed_ms > static_cast<int64_t>(kControlReadTimeoutMs + 100U)) {
            ESP_LOGW(kTag, "control stream Read exceeded timeout: configured=%" PRIu32 " ms elapsed=%" PRId64 " ms",
                     kControlReadTimeoutMs, control_read_elapsed_ms);
        }
        if (bytes_read > 0) {
            reconnect_backoff.Reset();
            HandleControlBytes(client.get(), identity, read_buffer.data(), static_cast<size_t>(bytes_read),
                               publish_firmware_status);
        } else if (bytes_read == 0 || control_stream->GetError() != "Read timeout") {
            close_transport();
            wait_before_retry(host_ui::RemoteControlConnectionState::kBackoff, "Control stream closed");
        }
    }

    close_transport();
    SetConnectionState(host_ui::RemoteControlConnectionState::kDisabled, "Remote Control task stopped");
}

}  // namespace micropixel::firmware::remote_control
