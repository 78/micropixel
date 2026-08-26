#include "local_control/usb_local_control_agent.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "runtime/bundle/bundle_format.h"

namespace micropixel::firmware::local_control {
namespace {

constexpr char kTag[] = "usb_control";
constexpr std::string_view kPrefix = "MPX1 ";
constexpr size_t kMaximumPackageBytes = 8U * 1024U * 1024U;
constexpr size_t kMaximumChunkBytes = 3072U;
constexpr TickType_t kInstallTimeout = pdMS_TO_TICKS(120U * 1000U);
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
    if (app_id.empty() || app_id.size() >= remote_control::kRemoteControlAppIdCapacity) {
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

}  // namespace

UsbLocalControlAgent::UsbLocalControlAgent(device::LocalControlBackend& backend,
                                           remote_control::RemoteControlAgent& host_commands)
    : backend_(backend), host_commands_(host_commands) {
    response_queue_ = xQueueCreateStatic(kResponseQueueCapacity, sizeof(Response), response_queue_bytes_.data(),
                                         &response_queue_storage_);
}

UsbLocalControlAgent::~UsbLocalControlAgent() { Stop(); }

bool UsbLocalControlAgent::Start() {
    if (started_ || response_queue_ == nullptr) {
        return started_;
    }
    host_commands_.SetLocalHostResultSink(ReceiveHostResult, this);
    backend_.Bind(ReceiveCommand, ProvideResponse, this);
    started_ = true;
    ESP_LOGI(kTag, "USB local control ready; protocol=MPX1");
    return true;
}

void UsbLocalControlAgent::Stop() {
    if (!started_) {
        return;
    }
    backend_.Unbind(this);
    host_commands_.SetLocalHostResultSink(nullptr, nullptr);
    AbortInstall();
    started_ = false;
}

void UsbLocalControlAgent::ReceiveCommand(void* context, const char* command) {
    static_cast<UsbLocalControlAgent*>(context)->HandleCommand(command);
}

bool UsbLocalControlAgent::ProvideResponse(void* context, char* response, size_t capacity) {
    return static_cast<UsbLocalControlAgent*>(context)->PollResponse(response, capacity);
}

bool UsbLocalControlAgent::ReceiveHostResult(void* context, const remote_control::RemoteControlHostResult& result) {
    return static_cast<UsbLocalControlAgent*>(context)->HandleHostResult(result);
}

bool UsbLocalControlAgent::QueueResponse(uint32_t request_id, const char* status, const char* detail) {
    if (response_queue_ == nullptr || status == nullptr || detail == nullptr) {
        return false;
    }
    Response response{};
    const int length =
        std::snprintf(response.text.data(), response.text.size(), "MPX1 %" PRIu32 " %s %s", request_id, status, detail);
    if (length <= 0 || static_cast<size_t>(length) >= response.text.size()) {
        return false;
    }
    return xQueueSend(response_queue_, &response, 0U) == pdTRUE;
}

bool UsbLocalControlAgent::PollResponse(char* response, size_t capacity) {
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

bool UsbLocalControlAgent::HandleHostResult(const remote_control::RemoteControlHostResult& result) {
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

bool UsbLocalControlAgent::QueueHostCommand(uint32_t request_id, remote_control::RemoteControlHostCommandType type,
                                            std::string_view app_id) {
    remote_control::RemoteControlHostCommand command{};
    std::snprintf(command.command_id.data(), command.command_id.size(), "usb:%" PRIu32, request_id);
    command.type = type;
    command.deadline_ticks = xTaskGetTickCount() + kHostCommandTimeout;
    if (!app_id.empty()) {
        std::snprintf(command.app_id.data(), command.app_id.size(), "%.*s", static_cast<int>(app_id.size()),
                      app_id.data());
    }
    if (!host_commands_.QueueLocalHostCommand(command)) {
        (void)QueueResponse(request_id, "ERROR", "device_busy");
        return false;
    }
    return true;
}

void UsbLocalControlAgent::HandleAppList(uint32_t request_id, std::string_view arguments) {
    uint32_t offset = 0U;
    arguments = TrimLeft(arguments);
    if ((!arguments.empty() && !ParseUnsigned(TakeToken(arguments), offset)) || !TrimLeft(arguments).empty()) {
        (void)QueueResponse(request_id, "ERROR", "invalid_offset");
        return;
    }
    host_commands_.CopyLocalSnapshot(snapshot_workspace_);
    const remote_control::RemoteControlCatalogSnapshot& catalog = snapshot_workspace_.catalog;
    if (offset > catalog.count) {
        (void)QueueResponse(request_id, "ERROR", "invalid_offset");
        return;
    }
    const uint32_t end = std::min(catalog.count, offset + kAppsPerPage);
    std::array<char, kResponseCapacity> detail{};
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
        std::array<unsigned char, 132U> encoded_name{};
        size_t encoded_size = 0U;
        const size_t name_size = ::strnlen(app.display_name.data(), app.display_name.size());
        if (mbedtls_base64_encode(encoded_name.data(), encoded_name.size(), &encoded_size,
                                  reinterpret_cast<const unsigned char*>(app.display_name.data()), name_size) != 0) {
            (void)QueueResponse(request_id, "ERROR", "response_encoding_failed");
            return;
        }
        const bool active = std::strcmp(app.app_id.data(), snapshot_workspace_.active_app_id.data()) == 0;
        const char* lifecycle =
            active && snapshot_workspace_.lifecycle[0] != '\0' ? snapshot_workspace_.lifecycle.data() : "not_running";
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

void UsbLocalControlAgent::HandleInstallBegin(uint32_t request_id, std::string_view arguments) {
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
    (void)QueueResponse(request_id, "OK", "INSTALL_READY 3072");
}

void UsbLocalControlAgent::HandleInstallChunk(uint32_t request_id, std::string_view arguments) {
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
    std::array<char, 64U> detail{};
    std::snprintf(detail.data(), detail.size(), "INSTALL_CHUNK %zu", install_.received);
    (void)QueueResponse(request_id, "OK", detail.data());
}

void UsbLocalControlAgent::HandleInstallCommit(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty() || install_.data == nullptr || request_id != install_.request_id) {
        (void)QueueResponse(request_id, "ERROR", "no_install_session");
        return;
    }
    if (install_.received != install_.size) {
        (void)QueueResponse(request_id, "ERROR", "install_incomplete");
        return;
    }
    remote_control::RemoteControlHostCommand command{};
    std::snprintf(command.command_id.data(), command.command_id.size(), "usb:%" PRIu32, request_id);
    command.type = remote_control::RemoteControlHostCommandType::kInstallApp;
    command.deadline_ticks = xTaskGetTickCount() + kHostCommandTimeout;
    command.app_id = install_.app_id;
    command.package_data = install_.data;
    command.package_size = install_.size;
    command.package_sha256 = install_.sha256;
    if (!host_commands_.QueueLocalHostCommand(command)) {
        (void)QueueResponse(request_id, "ERROR", "device_busy");
        return;
    }
    install_ = {};
}

void UsbLocalControlAgent::HandleInstallAbort(uint32_t request_id, std::string_view arguments) {
    if (!TrimLeft(arguments).empty() || install_.data == nullptr || request_id != install_.request_id) {
        (void)QueueResponse(request_id, "ERROR", "no_install_session");
        return;
    }
    AbortInstall();
    (void)QueueResponse(request_id, "OK", "INSTALL_ABORTED");
}

void UsbLocalControlAgent::AbortInstall() {
    if (install_.data != nullptr) {
        heap_caps_free(install_.data);
    }
    install_ = {};
}

void UsbLocalControlAgent::ExpireInstallIfNeeded() {
    if (install_.data == nullptr ||
        static_cast<TickType_t>(xTaskGetTickCount() - install_.last_activity) < kInstallTimeout) {
        return;
    }
    const uint32_t request_id = install_.request_id;
    AbortInstall();
    (void)QueueResponse(request_id, "ERROR", "install_timeout");
}

void UsbLocalControlAgent::HandleCommand(const char* command) {
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
    } else if (operation == "APP_START") {
        const std::string_view app_id = TakeToken(remaining);
        if (!ValidAppId(app_id) || !TrimLeft(remaining).empty()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_app_id");
        } else {
            (void)QueueHostCommand(request_id, remote_control::RemoteControlHostCommandType::kStartApp, app_id);
        }
    } else if (operation == "APP_STOP") {
        const std::string_view app_id = TakeToken(remaining);
        if ((!app_id.empty() && !ValidAppId(app_id)) || !TrimLeft(remaining).empty()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_app_id");
        } else {
            (void)QueueHostCommand(request_id, remote_control::RemoteControlHostCommandType::kStopApp, app_id);
        }
    } else if (operation == "APP_UNINSTALL") {
        const std::string_view app_id = TakeToken(remaining);
        if (!ValidAppId(app_id) || !TrimLeft(remaining).empty()) {
            (void)QueueResponse(request_id, "ERROR", "invalid_app_id");
        } else {
            (void)QueueHostCommand(request_id, remote_control::RemoteControlHostCommandType::kUninstallApp, app_id);
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
