#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "freertos/FreeRTOS.h"

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
    uint32_t bundle_size{};
};

struct CatalogSnapshot final {
    std::array<AppDescriptor, kMaxApps> apps{};
    uint32_t count{};
    uint32_t store_total_bytes{};
    uint32_t store_used_bytes{};
};

struct AppDiagnostic final {
    std::array<char, kAppIdCapacity> app_id{};
    std::array<char, 24U> phase{};
    std::array<char, 48U> code{};
    std::array<char, 256U> detail{};
    int32_t exit_code{};
    bool has_exit_code{};
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
    std::array<SequenceOperation, kMaxSequenceOperations> operations{};
    uint32_t operation_count{};
    uint8_t* package_data{};
    size_t package_size{};
    std::array<uint8_t, 32U> package_sha256{};
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

}  // namespace micropixel::firmware::control
