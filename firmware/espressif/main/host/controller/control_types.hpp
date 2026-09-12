#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "abi/micropixel_abi.h"
#include "device/contracts/input.hpp"
#include "freertos/FreeRTOS.h"
#include "runtime/bundle/app_requirements.h"

namespace micropixel::firmware::control {

constexpr size_t kMaxApps = 50U;
constexpr size_t kAppIdCapacity = 65U;
constexpr size_t kDisplayNameCapacity = 65U;
constexpr size_t kCommandIdCapacity = 64U;
constexpr size_t kMaxSequenceOperations = 16U;
constexpr size_t kMaxResultArtifacts = 4U;

struct AppDescriptor final {
    std::array<char, kAppIdCapacity> app_id{};
    std::array<char, kDisplayNameCapacity> display_name{};
    std::array<char, 32U> version{};
    uint32_t bundle_size{};
    std::array<uint8_t, 32U> sha256{};
};

struct CatalogSnapshot final {
    std::array<AppDescriptor, kMaxApps> apps{};
    uint32_t count{};
    uint64_t store_total_bytes{};
    uint64_t store_used_bytes{};
};

struct AppDiagnostic final {
    std::array<char, kAppIdCapacity> app_id{};
    std::array<char, 24U> phase{};
    std::array<char, 48U> code{};
    std::array<char, 256U> detail{};
    int32_t exit_code{};
    bool has_exit_code{};
};

struct StoreAppUpdate final {
    std::array<uint8_t, 32U> baseline_sha256{};
    std::array<char, kAppIdCapacity> app_id{};
    std::array<char, 32U> version{};
    std::array<char, 24U> state{};
};

struct StoreSnapshot final {
    micropixel_app_environment_t environment{};
    uint32_t idle_ms{};
    bool busy{true};
};

struct HostSnapshot final {
    CatalogSnapshot catalog{};
    std::array<char, kAppIdCapacity> active_app_id{};
    std::array<char, 24U> lifecycle{};
    AppDiagnostic last_app_diagnostic{};
    bool has_last_app_diagnostic{};
};

enum class HostCommandType : uint8_t {
    kCaptureScreen,
    kStartApp,
    kStopApp,
    kInstallApp,
    kUninstallApp,
    kInputSequence,
    kFirmwareStatus,
    kFirmwareUpdate,
};

enum class ControlSource : uint8_t {
    kRemote,
    kLocal,
};

// Transient Host-only state used by App Hall while a package is transferred
// and committed. It is deliberately separate from the installed catalog: an
// app can be visible here before its Bundle has entered App Store.
struct InstallActivity final {
    std::array<char, kCommandIdCapacity> command_id{};
    std::array<char, kAppIdCapacity> app_id{};
    ControlSource source{ControlSource::kRemote};
    uint32_t generation{};
    uint8_t progress_percent{};
    bool active{};
};

enum class SequenceOperationType : uint8_t {
    kTouch,
    kKey,
    kCaptureScreen,
};

struct SequenceOperation final {
    SequenceOperationType type{SequenceOperationType::kTouch};
    uint32_t delay_ms{};
    device::TouchSample touch{};
    device::KeySample key{};
    std::array<char, 33U> capture_id{};
};

struct HostCommand final {
    std::array<char, kCommandIdCapacity> command_id{};
    ControlSource source{ControlSource::kRemote};
    HostCommandType type{HostCommandType::kCaptureScreen};
    TickType_t deadline_ticks{};
    std::array<char, kAppIdCapacity> app_id{};
    micropixel_system_launch_arguments_response_t launch_arguments{};
    std::array<SequenceOperation, kMaxSequenceOperations> operations{};
    uint32_t operation_count{};
    uint8_t* package_data{};
    size_t package_size{};
    std::array<uint8_t, 32U> package_sha256{};
    std::array<uint8_t, 32U> baseline_sha256{};
    std::array<char, 32U> store_version{};
    bool store_verified{};
    bool automatic{};
};

using ArtifactRelease = void (*)(uint8_t*);

struct Artifact final {
    std::array<char, 33U> capture_id{};
    uint8_t* data{};
    size_t size{};
    uint32_t width{};
    uint32_t height{};
    ArtifactRelease release{};
};

struct HostResult final {
    std::array<char, kCommandIdCapacity> command_id{};
    ControlSource source{ControlSource::kRemote};
    std::array<char, 96U> message{};
    std::array<Artifact, kMaxResultArtifacts> artifacts{};
    AppDiagnostic diagnostic{};
    uint32_t artifact_count{};
    bool has_diagnostic{};
    bool ok{};
};

// Lower-case hex of a SHA-256 digest, NUL terminated (shared by the local and
// remote control agents' App list responses).
inline void FormatSha256Hex(const std::array<uint8_t, 32U>& digest, std::array<char, 65U>& text_out) {
    for (size_t index = 0U; index < digest.size(); ++index) {
        std::snprintf(text_out.data() + index * 2U, 3U, "%02x", digest[index]);
    }
}

}  // namespace micropixel::firmware::control
