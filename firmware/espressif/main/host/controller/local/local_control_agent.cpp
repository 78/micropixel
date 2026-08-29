#include "host/controller/local/local_control_agent.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "runtime/bundle/bundle_format.h"

namespace micropixel::firmware::local_control {
namespace {

constexpr char kTag[] = "local_control";
constexpr std::string_view kPrefix = "MPX1 ";
constexpr size_t kMaximumPackageBytes = 8U * 1024U * 1024U;
constexpr size_t kMaximumChunkBytes = 3072U;
constexpr TickType_t kInstallTimeout = pdMS_TO_TICKS(120U * 1000U);
constexpr uint64_t kInstallTimeoutUs = 120ULL * 1000ULL * 1000ULL;
constexpr TickType_t kHostCommandTimeout = pdMS_TO_TICKS(5U * 60U * 1000U);
constexpr uint32_t kAppsPerPage = 4U;

std::string_view TrimLeft(std::string_view value) {
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1U);
    }
    return value;
}

std::string_view TakeToken(std::string_view& value) {
    value = TrimLeft(value);
    const size_t end = value.find(' ');
    if (end == std::string_view::npos) {
        const std::string_view token = value;
        value = {};
        return token;
    }
    const std::string_view token = value.substr(0U, end);
    value.remove_prefix(end + 1U);
    return token;
}

template <typename Integer>
bool ParseUnsigned(std::string_view token, Integer& value) {
    if (token.empty()) {
        return false;
    }
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    auto [parsed_end, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed_end == end;
}

bool ValidAppId(std::string_view app_id) {
    if (app_id.empty() || app_id.size() >= control::kAppIdCapacity) {
        return false;
    }
    return std::all_of(app_id.begin(), app_id.end(), [](char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
               byte == '_' || byte == '-' || byte == '.';
    });
}

bool ParseSha256(std::string_view text, std::array<uint8_t, 32U>& digest) {
    if (text.size() != digest.size() * 2U) {
        return false;
    }
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t index = 0U; index < digest.size(); ++index) {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[index] = static_cast<uint8_t>((high << 4U) | low);
    }
    return true;
}

bool JsonUnsigned(const cJSON* object, const char* name, uint32_t maximum, uint32_t& value, uint32_t fallback = 0U) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == nullptr) {
        value = fallback;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 || item->valuedouble > maximum ||
        item->valuedouble != static_cast<double>(static_cast<uint32_t>(item->valuedouble))) {
        return false;
    }
    value = static_cast<uint32_t>(item->valuedouble);
    return true;
}

const char* JsonText(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : nullptr;
}

bool ParseTouchPhase(const char* text, device::TouchPhase& phase) {
    if (text == nullptr) return false;
    if (std::strcmp(text, "down") == 0)
        phase = device::TouchPhase::kDown;
    else if (std::strcmp(text, "move") == 0)
        phase = device::TouchPhase::kMove;
    else if (std::strcmp(text, "up") == 0)
        phase = device::TouchPhase::kUp;
    else if (std::strcmp(text, "cancel") == 0)
        phase = device::TouchPhase::kCancel;
    else
        return false;
    return true;
}

bool ParseKeyPhase(const char* text, device::KeyPhase& phase) {
    if (text == nullptr) return false;
    if (std::strcmp(text, "down") == 0)
        phase = device::KeyPhase::kDown;
    else if (std::strcmp(text, "up") == 0)
        phase = device::KeyPhase::kUp;
    else if (std::strcmp(text, "repeat") == 0)
        phase = device::KeyPhase::kRepeat;
    else if (std::strcmp(text, "cancel") == 0)
        phase = device::KeyPhase::kCancel;
    else
        return false;
    return true;
}

bool ParseKeyCode(const char* text, device::KeyCode& code) {
    if (text == nullptr) return false;
    struct Mapping final {
        const char* text;
        device::KeyCode code;
    };
    constexpr std::array mappings{
        Mapping{"up", device::KeyCode::kUp},           Mapping{"down", device::KeyCode::kDown},
        Mapping{"left", device::KeyCode::kLeft},       Mapping{"right", device::KeyCode::kRight},
        Mapping{"confirm", device::KeyCode::kConfirm}, Mapping{"back", device::KeyCode::kBack},
        Mapping{"menu", device::KeyCode::kMenu},       Mapping{"south", device::KeyCode::kSouth},
        Mapping{"east", device::KeyCode::kEast},       Mapping{"west", device::KeyCode::kWest},
        Mapping{"north", device::KeyCode::kNorth},
    };
    for (const auto& mapping : mappings) {
        if (std::strcmp(text, mapping.text) == 0) {
            code = mapping.code;
            return true;
        }
    }
    return false;
}

}  // namespace

LocalControlAgent::LocalControlAgent(device::LocalControl& transport, control::ControlDispatcher& controls,
                                     control::GuestLogBuffer& guest_logs, const device::BoardInfo& board_info,
                                     device::Wifi& wifi)
    : transport_(transport), controls_(controls), guest_logs_(guest_logs), board_info_(board_info), wifi_(wifi) {
    response_queue_bytes_ = static_cast<uint8_t*>(
        heap_caps_calloc(kResponseQueueCapacity, sizeof(Response), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    void* command_workspace_storage =
        heap_caps_calloc(1U, sizeof(CommandWorkspace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (command_workspace_storage != nullptr) {
        command_workspace_ = std::construct_at(static_cast<CommandWorkspace*>(command_workspace_storage));
    }
    void* snapshot_storage = heap_caps_calloc(1U, sizeof(control::HostSnapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snapshot_storage != nullptr) {
        snapshot_workspace_ = std::construct_at(static_cast<control::HostSnapshot*>(snapshot_storage));
    }
    response_workspace_mutex_ = xSemaphoreCreateMutexStatic(&response_workspace_mutex_storage_);
    if (response_queue_bytes_ != nullptr && command_workspace_ != nullptr && snapshot_workspace_ != nullptr &&
        response_workspace_mutex_ != nullptr) {
        response_queue_ = xQueueCreateStatic(kResponseQueueCapacity, sizeof(Response), response_queue_bytes_,
                                             &response_queue_storage_);
        ESP_LOGI(kTag, "local control workspaces allocated in PSRAM: queue=%zu command=%zu snapshot=%zu bytes",
                 sizeof(Response) * kResponseQueueCapacity, sizeof(*command_workspace_), sizeof(*snapshot_workspace_));
    } else {
        ESP_LOGE(kTag, "local control requires %zu bytes of PSRAM workspaces",
                 sizeof(Response) * kResponseQueueCapacity + sizeof(CommandWorkspace) + sizeof(control::HostSnapshot));
    }
}

LocalControlAgent::~LocalControlAgent() {
    Stop();
    if (install_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(install_timer_, portMAX_DELAY);
        (void)esp_timer_delete(install_timer_);
        install_timer_ = nullptr;
    }
    if (restart_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(restart_timer_, portMAX_DELAY);
        (void)esp_timer_delete(restart_timer_);
        restart_timer_ = nullptr;
    }
    if (snapshot_workspace_ != nullptr) {
        std::destroy_at(snapshot_workspace_);
        heap_caps_free(snapshot_workspace_);
        snapshot_workspace_ = nullptr;
    }
    if (command_workspace_ != nullptr) {
        std::destroy_at(command_workspace_);
        heap_caps_free(command_workspace_);
        command_workspace_ = nullptr;
    }
    heap_caps_free(response_queue_bytes_);
    response_queue_bytes_ = nullptr;
}

bool LocalControlAgent::Start() {
    if (started_ || response_queue_ == nullptr) {
        return started_;
    }
    if (install_timer_ == nullptr) {
        esp_timer_create_args_t timer_arguments{};
        timer_arguments.callback = InstallTimeoutElapsed;
        timer_arguments.arg = this;
        timer_arguments.dispatch_method = ESP_TIMER_TASK;
        timer_arguments.name = "local_install";
        timer_arguments.skip_unhandled_events = true;
        if (esp_timer_create(&timer_arguments, &install_timer_) != ESP_OK) {
            ESP_LOGE(kTag, "local install timeout timer is unavailable");
            return false;
        }
    }
    controls_.SetLocalResultSink(ReceiveHostResult, this);
    transport_.Bind(ReceiveCommand, ProvideResponse, this);
    started_ = true;
    ESP_LOGI(kTag, "local control ready; protocol=MPX1");
    return true;
}

void LocalControlAgent::Stop() {
    if (!started_) {
        return;
    }
    transport_.Unbind(this);
    controls_.SetLocalResultSink(nullptr, nullptr);
    AbortInstall();
    started_ = false;
}

void LocalControlAgent::ReceiveCommand(void* context, const char* command) {
    static_cast<LocalControlAgent*>(context)->HandleCommand(command);
}

bool LocalControlAgent::ProvideResponse(void* context, char* response, size_t capacity) {
    return static_cast<LocalControlAgent*>(context)->PollResponse(response, capacity);
}

bool LocalControlAgent::ReceiveHostResult(void* context, const control::HostResult& result) {
    return static_cast<LocalControlAgent*>(context)->HandleHostResult(result);
}

void LocalControlAgent::InstallTimeoutElapsed(void* context) {
    auto* agent = static_cast<LocalControlAgent*>(context);
    agent->install_timeout_due_.store(true, std::memory_order_release);
    agent->transport_.NotifyResponseReady();
}

void LocalControlAgent::RestartTimerElapsed(void* context) {
    (void)context;
    esp_restart();
}

bool LocalControlAgent::QueueResponse(uint32_t request_id, const char* status, const char* detail) {
    if (response_queue_ == nullptr || response_workspace_mutex_ == nullptr || command_workspace_ == nullptr ||
        status == nullptr || detail == nullptr || xSemaphoreTake(response_workspace_mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    Response& response = command_workspace_->response;
    response = {};
    const int length =
        std::snprintf(response.text.data(), response.text.size(), "MPX1 %" PRIu32 " %s %s", request_id, status, detail);
    if (length <= 0 || static_cast<size_t>(length) >= response.text.size()) {
        (void)xSemaphoreGive(response_workspace_mutex_);
        return false;
    }
    const bool queued = xQueueSend(response_queue_, &response, 0U) == pdTRUE;
    (void)xSemaphoreGive(response_workspace_mutex_);
    if (!queued) {
        return false;
    }
    transport_.NotifyResponseReady();
    return true;
}

bool LocalControlAgent::PollResponse(char* response, size_t capacity) {
    ExpireInstallIfNeeded();
    if (response == nullptr || capacity == 0U || response_queue_ == nullptr) {
        return false;
    }
    Response queued{};
    if (xQueueReceive(response_queue_, &queued, 0U) != pdTRUE) {
        return false;
    }
    std::snprintf(response, capacity, "%s", queued.text.data());
    return true;
}

bool LocalControlAgent::HandleHostResult(const control::HostResult& result) {
    if (result.source != control::ControlSource::kLocal) {
        return false;
    }
    uint32_t request_id = 0U;
    const std::string_view command_id(result.command_id.data());
    if (!command_id.starts_with("usb:") || !ParseUnsigned(command_id.substr(4U), request_id)) {
        return false;
    }
    if (!result.ok) {
        return QueueResponse(request_id, "ERROR", result.message.data());
    }
    std::array<char, 160U> detail{};
    std::snprintf(detail.data(), detail.size(), "RESULT %s", result.message.data());
    return QueueResponse(request_id, "OK", detail.data());
}

bool LocalControlAgent::QueueHostCommand(uint32_t request_id, control::HostCommandType type, std::string_view app_id) {
    control::HostCommand command{};
    std::snprintf(command.command_id.data(), command.command_id.size(), "usb:%" PRIu32, request_id);
    command.source = control::ControlSource::kLocal;
    command.type = type;
    command.deadline_ticks = xTaskGetTickCount() + kHostCommandTimeout;
    if (!app_id.empty()) {
        std::snprintf(command.app_id.data(), command.app_id.size(), "%.*s", static_cast<int>(app_id.size()),
                      app_id.data());
    }
    if (!controls_.QueueLocalCommand(command)) {
        (void)QueueResponse(request_id, "ERROR", "device_busy");
        return false;
    }
    return true;
}

void LocalControlAgent::HandleAppList(uint32_t request_id, std::string_view arguments) {
    uint32_t offset = 0U;
    arguments = TrimLeft(arguments);
    if ((!arguments.empty() && !ParseUnsigned(TakeToken(arguments), offset)) || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_offset");
        return;
    }
    if (snapshot_workspace_ == nullptr) {
        (void)QueueResponse(request_id, "ERROR", "out_of_memory");
        return;
    }
    controls_.CopySnapshot(*snapshot_workspace_);
    const control::CatalogSnapshot& catalog = snapshot_workspace_->catalog;
    if (offset > catalog.count) {
        (void)QueueResponse(request_id, "ERROR", "invalid_offset");
        return;
    }
    const uint32_t end = std::min(catalog.count, offset + kAppsPerPage);
    auto& detail = command_workspace_->detail;
    detail = {};
    const int prefix_length =
        std::snprintf(detail.data(), detail.size(), "APP_LIST %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
                      end, catalog.count, catalog.store_used_bytes, catalog.store_total_bytes, end - offset);
    if (prefix_length <= 0 || static_cast<size_t>(prefix_length) >= detail.size()) {
        (void)QueueResponse(request_id, "ERROR", "response_too_large");
        return;
    }
    size_t used = static_cast<size_t>(prefix_length);
    for (uint32_t index = offset; index < end; ++index) {
        const auto& app = catalog.apps[index];
        auto& encoded_name = command_workspace_->encoded;
        encoded_name = {};
        size_t encoded_size = 0U;
        const size_t name_size = ::strnlen(app.display_name.data(), app.display_name.size());
        if (mbedtls_base64_encode(encoded_name.data(), encoded_name.size(), &encoded_size,
                                  reinterpret_cast<const unsigned char*>(app.display_name.data()), name_size) != 0) {
            (void)QueueResponse(request_id, "ERROR", "response_encoding_failed");
            return;
        }
        const bool active = std::strcmp(app.app_id.data(), snapshot_workspace_->active_app_id.data()) == 0;
        const char* lifecycle =
            active && snapshot_workspace_->lifecycle[0] != '\0' ? snapshot_workspace_->lifecycle.data() : "not_running";
        const int entry_length = std::snprintf(detail.data() + used, detail.size() - used, " %s,%" PRIu32 ",%.*s,%u,%s",
                                               app.app_id.data(), app.bundle_size, static_cast<int>(encoded_size),
                                               encoded_name.data(), active ? 1U : 0U, lifecycle);
        if (entry_length <= 0 || static_cast<size_t>(entry_length) >= detail.size() - used) {
            (void)QueueResponse(request_id, "ERROR", "response_too_large");
            return;
        }
        used += static_cast<size_t>(entry_length);
    }
    (void)QueueResponse(request_id, "OK", detail.data());
}

void LocalControlAgent::HandleAppLastError(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_arguments");
        return;
    }
    if (snapshot_workspace_ == nullptr) {
        (void)QueueResponse(request_id, "ERROR", "out_of_memory");
        return;
    }
    controls_.CopySnapshot(*snapshot_workspace_);
    if (!snapshot_workspace_->has_last_app_diagnostic) {
        (void)QueueResponse(request_id, "OK", "APP_ERROR NONE");
        return;
    }
    const control::AppDiagnostic& diagnostic = snapshot_workspace_->last_app_diagnostic;
    auto& encoded_detail = command_workspace_->encoded;
    encoded_detail = {};
    size_t encoded_size = 0U;
    const size_t detail_size = ::strnlen(diagnostic.detail.data(), diagnostic.detail.size());
    if (mbedtls_base64_encode(encoded_detail.data(), encoded_detail.size(), &encoded_size,
                              reinterpret_cast<const unsigned char*>(diagnostic.detail.data()), detail_size) != 0) {
        (void)QueueResponse(request_id, "ERROR", "response_encoding_failed");
        return;
    }
    auto& detail = command_workspace_->detail;
    detail = {};
    const int length =
        std::snprintf(detail.data(), detail.size(), "APP_ERROR %s %s %s %u %" PRId32 " %.*s", diagnostic.app_id.data(),
                      diagnostic.phase.data(), diagnostic.code.data(), diagnostic.has_exit_code ? 1U : 0U,
                      diagnostic.exit_code, static_cast<int>(encoded_size), encoded_detail.data());
    if (length <= 0 || static_cast<size_t>(length) >= detail.size()) {
        (void)QueueResponse(request_id, "ERROR", "response_too_large");
        return;
    }
    (void)QueueResponse(request_id, "OK", detail.data());
}

void LocalControlAgent::HandleLogRead(uint32_t request_id, std::string_view arguments) {
    const std::string_view session = TakeToken(arguments);
    const std::string_view sequence_text = TakeToken(arguments);
    uint64_t after_sequence = 0U;
    if (session.empty() || !ParseUnsigned(sequence_text, after_sequence) || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_cursor");
        return;
    }
    std::array<char, control::kCommandIdCapacity> session_text{};
    if (session != "-") {
        if (session.size() >= session_text.size()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_cursor");
            return;
        }
        std::snprintf(session_text.data(), session_text.size(), "%.*s", static_cast<int>(session.size()),
                      session.data());
    }
    const auto page = guest_logs_.ReadOne(session == "-" ? nullptr : session_text.data(), after_sequence);
    const char* current_session = page.session_id[0] != '\0' ? page.session_id.data() : "-";
    if (!page.has_entry) {
        std::array<char, 160U> detail{};
        std::snprintf(detail.data(), detail.size(), "LOG_PAGE %s %" PRIu64 " %u %u 0", current_session,
                      page.next_sequence, page.truncated ? 1U : 0U, page.has_more ? 1U : 0U);
        (void)QueueResponse(request_id, "OK", detail.data());
        return;
    }
    auto& encoded_message = command_workspace_->encoded;
    encoded_message = {};
    size_t encoded_size = 0U;
    const size_t message_size = ::strnlen(page.entry.message.data(), page.entry.message.size());
    if (mbedtls_base64_encode(encoded_message.data(), encoded_message.size(), &encoded_size,
                              reinterpret_cast<const unsigned char*>(page.entry.message.data()), message_size) != 0) {
        (void)QueueResponse(request_id, "ERROR", "response_encoding_failed");
        return;
    }
    auto& detail = command_workspace_->detail;
    detail = {};
    const int length = std::snprintf(
        detail.data(), detail.size(), "LOG_PAGE %s %" PRIu64 " %u %u 1 %" PRIu64 " %" PRIu64 " %" PRIu32 " %s %.*s",
        current_session, page.next_sequence, page.truncated ? 1U : 0U, page.has_more ? 1U : 0U, page.entry.sequence,
        page.entry.timestamp_us, page.entry.level, page.entry.app_id.data(), static_cast<int>(encoded_size),
        encoded_message.data());
    if (length <= 0 || static_cast<size_t>(length) >= detail.size()) {
        (void)QueueResponse(request_id, "ERROR", "response_too_large");
        return;
    }
    (void)QueueResponse(request_id, "OK", detail.data());
}

void LocalControlAgent::HandleInputSequence(uint32_t request_id, std::string_view arguments) {
    const std::string_view encoded = TakeToken(arguments);
    if (encoded.empty() || encoded.size() > 4096U || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_input_sequence");
        return;
    }
    constexpr size_t kDecodedCapacity = 3072U;
    auto* decoded =
        static_cast<unsigned char*>(heap_caps_malloc(kDecodedCapacity + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (decoded == nullptr) {
        (void)QueueResponse(request_id, "ERROR", "out_of_memory");
        return;
    }
    size_t decoded_size = 0U;
    const int decode_status =
        mbedtls_base64_decode(decoded, kDecodedCapacity, &decoded_size,
                              reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (decode_status != 0 || decoded_size == 0U || decoded_size > kDecodedCapacity) {
        heap_caps_free(decoded);
        (void)QueueResponse(request_id, "ERROR", "invalid_input_sequence");
        return;
    }
    decoded[decoded_size] = '\0';
    cJSON* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(decoded), decoded_size);
    heap_caps_free(decoded);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        (void)QueueResponse(request_id, "ERROR", "invalid_input_sequence");
        return;
    }
    const int count = cJSON_GetArraySize(root);
    control::HostCommand command{};
    std::snprintf(command.command_id.data(), command.command_id.size(), "usb:%" PRIu32, request_id);
    command.source = control::ControlSource::kLocal;
    command.type = control::HostCommandType::kInputSequence;
    command.deadline_ticks = xTaskGetTickCount() + kHostCommandTimeout;
    bool valid = count > 0 && count <= static_cast<int>(command.operations.size());
    uint32_t total_delay_ms = 0U;
    for (int index = 0; valid && index < count; ++index) {
        const cJSON* source = cJSON_GetArrayItem(root, index);
        auto& operation = command.operations[static_cast<size_t>(index)];
        uint32_t delay_ms = 0U;
        const char* type = cJSON_IsObject(source) ? JsonText(source, "type") : nullptr;
        valid =
            type != nullptr && JsonUnsigned(source, "delayMs", 5000U, delay_ms) && total_delay_ms <= 60000U - delay_ms;
        total_delay_ms += delay_ms;
        operation.delay_ms = delay_ms;
        if (!valid) break;
        if (std::strcmp(type, "touch") == 0) {
            uint32_t id = 0U, x = 0U, y = 0U, pressure = 0U;
            valid = ParseTouchPhase(JsonText(source, "phase"), operation.touch.phase) &&
                    JsonUnsigned(source, "id", UINT32_MAX, id) && JsonUnsigned(source, "x", INT32_MAX, x) &&
                    JsonUnsigned(source, "y", INT32_MAX, y) &&
                    JsonUnsigned(source, "pressurePerMille", 1000U, pressure);
            operation.type = control::SequenceOperationType::kTouch;
            operation.touch.id = id;
            operation.touch.x = static_cast<int32_t>(x);
            operation.touch.y = static_cast<int32_t>(y);
            operation.touch.pressure_per_mille = static_cast<uint16_t>(pressure);
        } else if (std::strcmp(type, "key") == 0) {
            uint32_t repeat_count = 0U;
            valid = ParseKeyCode(JsonText(source, "code"), operation.key.code) &&
                    ParseKeyPhase(JsonText(source, "phase"), operation.key.phase) &&
                    JsonUnsigned(source, "repeatCount", 1000U, repeat_count) &&
                    ((operation.key.phase == device::KeyPhase::kRepeat) == (repeat_count != 0U));
            operation.type = control::SequenceOperationType::kKey;
            operation.key.repeat_count = repeat_count;
        } else {
            valid = false;
        }
    }
    cJSON_Delete(root);
    if (!valid) {
        (void)QueueResponse(request_id, "ERROR", "invalid_input_sequence");
        return;
    }
    command.operation_count = static_cast<uint32_t>(count);
    if (!controls_.QueueLocalCommand(command)) {
        (void)QueueResponse(request_id, "ERROR", "device_busy");
    }
}

void LocalControlAgent::HandleDeviceStatus(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty() || snapshot_workspace_ == nullptr) {
        (void)QueueResponse(request_id, "ERROR",
                            snapshot_workspace_ == nullptr ? "out_of_memory" : "invalid_arguments");
        return;
    }
    controls_.CopySnapshot(*snapshot_workspace_);
    const esp_app_desc_t* firmware = esp_app_get_description();
    const device::BoardInfo board_info = board_info_;
    const device::WifiSnapshot wifi = wifi_.Snapshot();
    auto encode = [](const char* source, auto& output, size_t& encoded_size) {
        const char* text = source != nullptr ? source : "";
        return mbedtls_base64_encode(output.data(), output.size(), &encoded_size,
                                     reinterpret_cast<const unsigned char*>(text), std::strlen(text)) == 0;
    };
    std::array<unsigned char, 96U> version{}, board{}, chip{};
    size_t version_size = 0U, board_size = 0U, chip_size = 0U;
    if (!encode(firmware != nullptr ? firmware->version : "unknown", version, version_size) ||
        !encode(board_info.board, board, board_size) || !encode(board_info.host_chip, chip, chip_size)) {
        (void)QueueResponse(request_id, "ERROR", "response_encoding_failed");
        return;
    }
    auto& detail = command_workspace_->detail;
    detail = {};
    const char* active_app =
        snapshot_workspace_->active_app_id[0] != '\0' ? snapshot_workspace_->active_app_id.data() : "-";
    const int length = std::snprintf(
        detail.data(), detail.size(),
        "DEVICE_STATUS %.*s %.*s %.*s %" PRIu64 " %s %s %" PRIu32 " %" PRIu32 " %" PRIu32 " %zu %zu %" PRIu32
        " %" PRIu32 " %u %u %u",
        static_cast<int>(version_size), version.data(), static_cast<int>(board_size), board.data(),
        static_cast<int>(chip_size), chip.data(), static_cast<uint64_t>(esp_timer_get_time() / 1000), active_app,
        snapshot_workspace_->lifecycle.data(), snapshot_workspace_->catalog.count,
        snapshot_workspace_->catalog.store_used_bytes, snapshot_workspace_->catalog.store_total_bytes,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), board_info.display.width_pixels,
        board_info.display.height_pixels, wifi.available ? 1U : 0U, wifi.enabled ? 1U : 0U, wifi.connected ? 1U : 0U);
    if (length <= 0 || static_cast<size_t>(length) >= detail.size()) {
        (void)QueueResponse(request_id, "ERROR", "response_too_large");
        return;
    }
    (void)QueueResponse(request_id, "OK", detail.data());
}

void LocalControlAgent::HandleTaskDiagnostics(uint32_t request_id, std::string_view arguments) {
    uint32_t offset = 0U;
    arguments = TrimLeft(arguments);
    if ((!arguments.empty() && !ParseUnsigned(TakeToken(arguments), offset)) || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_offset");
        return;
    }
#if configUSE_TRACE_FACILITY == 1
    constexpr UBaseType_t kTaskCapacity = 48U;
    auto* tasks = static_cast<TaskStatus_t*>(
        heap_caps_calloc(kTaskCapacity, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tasks == nullptr) {
        (void)QueueResponse(request_id, "ERROR", "out_of_memory");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total_runtime{};
    const UBaseType_t total_count = uxTaskGetNumberOfTasks();
    const UBaseType_t count = uxTaskGetSystemState(tasks, kTaskCapacity, &total_runtime);
    if (offset > count) {
        heap_caps_free(tasks);
        (void)QueueResponse(request_id, "ERROR", "invalid_offset");
        return;
    }
    const uint32_t next_offset = offset < count ? offset + 1U : offset;
    auto& detail = command_workspace_->detail;
    detail = {};
    int length =
        std::snprintf(detail.data(), detail.size(), "TASK_PAGE 1 %" PRIu32 " %u %u %u %" PRIu64 " %u", next_offset,
                      static_cast<unsigned>(count), static_cast<unsigned>(total_count), total_count > count ? 1U : 0U,
                      static_cast<uint64_t>(total_runtime), offset < count ? 1U : 0U);
    if (length > 0 && offset < count) {
        const TaskStatus_t& task = tasks[offset];
        std::array<unsigned char, 40U> encoded_name{};
        size_t encoded_size = 0U;
        const char* name = task.pcTaskName != nullptr ? task.pcTaskName : "unknown";
        if (mbedtls_base64_encode(encoded_name.data(), encoded_name.size(), &encoded_size,
                                  reinterpret_cast<const unsigned char*>(name), std::strlen(name)) != 0) {
            length = -1;
        } else {
            length +=
                std::snprintf(detail.data() + length, detail.size() - static_cast<size_t>(length),
                              " %.*s,%u,%u,%u,%u,%" PRIu64 ",%" PRIu32, static_cast<int>(encoded_size),
                              encoded_name.data(), static_cast<unsigned>(task.xTaskNumber),
                              static_cast<unsigned>(task.eCurrentState), static_cast<unsigned>(task.uxCurrentPriority),
                              static_cast<unsigned>(task.uxBasePriority), static_cast<uint64_t>(task.ulRunTimeCounter),
                              static_cast<uint32_t>(task.usStackHighWaterMark) * sizeof(StackType_t));
        }
    }
    heap_caps_free(tasks);
    if (length <= 0 || static_cast<size_t>(length) >= detail.size()) {
        (void)QueueResponse(request_id, "ERROR", "response_encoding_failed");
        return;
    }
    (void)QueueResponse(request_id, "OK", detail.data());
#else
    (void)offset;
    (void)QueueResponse(request_id, "OK", "TASK_PAGE 0 0 0 0 0 0 0");
#endif
}

void LocalControlAgent::HandleDeviceReboot(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_arguments");
        return;
    }
    if (restart_timer_ == nullptr) {
        esp_timer_create_args_t timer_arguments{};
        timer_arguments.callback = RestartTimerElapsed;
        timer_arguments.arg = this;
        timer_arguments.dispatch_method = ESP_TIMER_TASK;
        timer_arguments.name = "local_restart";
        timer_arguments.skip_unhandled_events = true;
        if (esp_timer_create(&timer_arguments, &restart_timer_) != ESP_OK) {
            (void)QueueResponse(request_id, "ERROR", "restart_unavailable");
            return;
        }
    }
    if (!QueueResponse(request_id, "OK", "RESULT rebooting")) {
        return;
    }
    if (esp_timer_start_once(restart_timer_, 500U * 1000U) != ESP_OK) {
        ESP_LOGE(kTag, "local reboot timer could not be armed");
    }
}

void LocalControlAgent::HandleInstallBegin(uint32_t request_id, std::string_view arguments) {
    if (install_.data != nullptr) {
        (void)QueueResponse(request_id, "ERROR", "install_busy");
        return;
    }
    const std::string_view app_id = TakeToken(arguments);
    const std::string_view size_text = TakeToken(arguments);
    const std::string_view sha256_text = TakeToken(arguments);
    size_t size = 0U;
    std::array<uint8_t, 32U> sha256{};
    if (!ValidAppId(app_id) || !ParseUnsigned(size_text, size) || size < sizeof(micropixel_bundle_header_t) ||
        size > kMaximumPackageBytes || (size % MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT) != 0U ||
        !ParseSha256(sha256_text, sha256) || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_install_request");
        return;
    }
    uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        (void)QueueResponse(request_id, "ERROR", "out_of_memory");
        return;
    }
    install_ = {};
    install_.data = data;
    install_.size = size;
    install_.request_id = request_id;
    install_.last_activity = xTaskGetTickCount();
    install_.sha256 = sha256;
    std::snprintf(install_.app_id.data(), install_.app_id.size(), "%.*s", static_cast<int>(app_id.size()),
                  app_id.data());
    if (!ArmInstallTimeout(kInstallTimeoutUs)) {
        AbortInstall();
        (void)QueueResponse(request_id, "ERROR", "install_timer_failed");
        return;
    }
    std::array<char, control::kCommandIdCapacity> command_id{};
    std::snprintf(command_id.data(), command_id.size(), "usb:%" PRIu32, request_id);
    if (!controls_.BeginInstallActivity(control::ControlSource::kLocal, command_id.data(), install_.app_id.data())) {
        AbortInstall();
        (void)QueueResponse(request_id, "ERROR", "install_busy");
        return;
    }
    (void)QueueResponse(request_id, "OK", "INSTALL_READY 3072");
}

void LocalControlAgent::HandleInstallChunk(uint32_t request_id, std::string_view arguments) {
    const std::string_view offset_text = TakeToken(arguments);
    const std::string_view encoded = TakeToken(arguments);
    size_t offset = 0U;
    if (install_.data == nullptr || request_id != install_.request_id) {
        (void)QueueResponse(request_id, "ERROR", "no_install_session");
        return;
    }
    if (!ParseUnsigned(offset_text, offset) || offset != install_.received || encoded.empty() ||
        encoded.size() > 4096U || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_install_chunk");
        return;
    }
    size_t decoded_size = 0U;
    const size_t remaining = install_.size - install_.received;
    const int status =
        mbedtls_base64_decode(install_.data + install_.received, std::min(remaining, kMaximumChunkBytes), &decoded_size,
                              reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (status != 0 || decoded_size == 0U || decoded_size > remaining) {
        (void)QueueResponse(request_id, "ERROR", "invalid_install_chunk");
        return;
    }
    install_.received += decoded_size;
    install_.last_activity = xTaskGetTickCount();
    std::array<char, control::kCommandIdCapacity> command_id{};
    std::snprintf(command_id.data(), command_id.size(), "usb:%" PRIu32, request_id);
    controls_.UpdateInstallProgress(
        control::ControlSource::kLocal, command_id.data(),
        static_cast<uint8_t>(std::min<size_t>(99U, install_.received * 99U / install_.size)));
    if (!ArmInstallTimeout(kInstallTimeoutUs)) {
        AbortInstall();
        (void)QueueResponse(request_id, "ERROR", "install_timer_failed");
        return;
    }
    std::array<char, 64U> detail{};
    std::snprintf(detail.data(), detail.size(), "INSTALL_CHUNK %zu", install_.received);
    (void)QueueResponse(request_id, "OK", detail.data());
}

void LocalControlAgent::HandleInstallCommit(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty() || install_.data == nullptr || request_id != install_.request_id) {
        (void)QueueResponse(request_id, "ERROR", "no_install_session");
        return;
    }
    if (install_.received != install_.size) {
        (void)QueueResponse(request_id, "ERROR", "install_incomplete");
        return;
    }
    control::HostCommand command{};
    std::snprintf(command.command_id.data(), command.command_id.size(), "usb:%" PRIu32, request_id);
    command.source = control::ControlSource::kLocal;
    command.type = control::HostCommandType::kInstallApp;
    command.deadline_ticks = xTaskGetTickCount() + kHostCommandTimeout;
    command.app_id = install_.app_id;
    command.package_data = install_.data;
    command.package_size = install_.size;
    command.package_sha256 = install_.sha256;
    if (!controls_.QueueLocalCommand(command)) {
        (void)QueueResponse(request_id, "ERROR", "device_busy");
        return;
    }
    controls_.UpdateInstallProgress(control::ControlSource::kLocal, command.command_id.data(), 99U);
    DisarmInstallTimeout();
    install_ = {};
}

void LocalControlAgent::HandleInstallAbort(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty() || install_.data == nullptr || request_id != install_.request_id) {
        (void)QueueResponse(request_id, "ERROR", "no_install_session");
        return;
    }
    AbortInstall();
    (void)QueueResponse(request_id, "OK", "INSTALL_ABORTED");
}

bool LocalControlAgent::ArmInstallTimeout(uint64_t delay_us) {
    if (install_timer_ == nullptr || delay_us == 0U) {
        return false;
    }
    install_timeout_due_.store(false, std::memory_order_release);
    const esp_err_t stop_status = esp_timer_stop(install_timer_);
    if (stop_status != ESP_OK && stop_status != ESP_ERR_INVALID_STATE) {
        return false;
    }
    return esp_timer_start_once(install_timer_, delay_us) == ESP_OK;
}

void LocalControlAgent::DisarmInstallTimeout() {
    install_timeout_due_.store(false, std::memory_order_release);
    if (install_timer_ != nullptr) {
        (void)esp_timer_stop(install_timer_);
    }
}

void LocalControlAgent::AbortInstall() {
    DisarmInstallTimeout();
    if (install_.request_id != 0U) {
        std::array<char, control::kCommandIdCapacity> command_id{};
        std::snprintf(command_id.data(), command_id.size(), "usb:%" PRIu32, install_.request_id);
        controls_.EndInstallActivity(control::ControlSource::kLocal, command_id.data());
    }
    if (install_.data != nullptr) {
        heap_caps_free(install_.data);
    }
    install_ = {};
}

void LocalControlAgent::ExpireInstallIfNeeded() {
    if (!install_timeout_due_.exchange(false, std::memory_order_acq_rel) || install_.data == nullptr) {
        return;
    }
    const TickType_t elapsed = static_cast<TickType_t>(xTaskGetTickCount() - install_.last_activity);
    if (elapsed < kInstallTimeout) {
        const TickType_t remaining = kInstallTimeout - elapsed;
        const uint64_t remaining_us = static_cast<uint64_t>(remaining) * portTICK_PERIOD_MS * 1000ULL;
        if (ArmInstallTimeout(remaining_us)) {
            return;
        }
        ESP_LOGE(kTag, "local install timeout timer could not be rearmed");
    }
    const uint32_t request_id = install_.request_id;
    AbortInstall();
    (void)QueueResponse(request_id, "ERROR", elapsed < kInstallTimeout ? "install_timer_failed" : "install_timeout");
}

void LocalControlAgent::HandleCommand(const char* command) {
    if (command == nullptr) {
        return;
    }
    std::string_view remaining(command);
    if (!remaining.starts_with(kPrefix)) {
        return;
    }
    remaining.remove_prefix(kPrefix.size());
    uint32_t request_id = 0U;
    const std::string_view request_id_text = TakeToken(remaining);
    const std::string_view operation = TakeToken(remaining);
    if (!ParseUnsigned(request_id_text, request_id) || request_id == 0U || operation.empty()) {
        return;
    }
    if (operation == "HELLO" && TrimLeft(remaining).empty()) {
        (void)QueueResponse(request_id, "OK", "HELLO 1 3072 8388608");
    } else if (operation == "APP_LIST") {
        HandleAppList(request_id, remaining);
    } else if (operation == "APP_LAST_ERROR") {
        HandleAppLastError(request_id, remaining);
    } else if (operation == "LOG_READ") {
        HandleLogRead(request_id, remaining);
    } else if (operation == "INPUT_SEQUENCE") {
        HandleInputSequence(request_id, remaining);
    } else if (operation == "FIRMWARE_STATUS" && TrimLeft(remaining).empty()) {
        (void)QueueHostCommand(request_id, control::HostCommandType::kFirmwareStatus);
    } else if (operation == "FIRMWARE_UPDATE" && TrimLeft(remaining).empty()) {
        (void)QueueHostCommand(request_id, control::HostCommandType::kFirmwareUpdate);
    } else if (operation == "DEVICE_STATUS") {
        HandleDeviceStatus(request_id, remaining);
    } else if (operation == "DEVICE_TASKS") {
        HandleTaskDiagnostics(request_id, remaining);
    } else if (operation == "DEVICE_REBOOT") {
        HandleDeviceReboot(request_id, remaining);
    } else if (operation == "APP_START") {
        const std::string_view app_id = TakeToken(remaining);
        if (!ValidAppId(app_id) || !TrimLeft(remaining).empty()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_app_id");
        } else {
            (void)QueueHostCommand(request_id, control::HostCommandType::kStartApp, app_id);
        }
    } else if (operation == "APP_STOP") {
        const std::string_view app_id = TakeToken(remaining);
        if ((!app_id.empty() && !ValidAppId(app_id)) || !TrimLeft(remaining).empty()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_app_id");
        } else {
            (void)QueueHostCommand(request_id, control::HostCommandType::kStopApp, app_id);
        }
    } else if (operation == "APP_UNINSTALL") {
        const std::string_view app_id = TakeToken(remaining);
        if (!ValidAppId(app_id) || !TrimLeft(remaining).empty()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_app_id");
        } else {
            (void)QueueHostCommand(request_id, control::HostCommandType::kUninstallApp, app_id);
        }
    } else if (operation == "APP_INSTALL_BEGIN") {
        HandleInstallBegin(request_id, remaining);
    } else if (operation == "APP_INSTALL_CHUNK") {
        HandleInstallChunk(request_id, remaining);
    } else if (operation == "APP_INSTALL_COMMIT") {
        HandleInstallCommit(request_id, remaining);
    } else if (operation == "APP_INSTALL_ABORT") {
        HandleInstallAbort(request_id, remaining);
    } else {
        (void)QueueResponse(request_id, "ERROR", "unsupported_command");
    }
}

}  // namespace micropixel::firmware::local_control
