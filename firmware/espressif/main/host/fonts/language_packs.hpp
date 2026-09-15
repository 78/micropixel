// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <span>
#include <string_view>

#include "host/fonts/font_download.hpp"
#include "host/fonts/language_pack_catalog.hpp"
#include "runtime/bundle/app_store.hpp"

namespace micropixel::host::fonts {
// Process-lifetime Host owner. The network task only fills download_; installation
// and font activation run on the Host with no AppSession. Font bytes stay in NOR.
class LanguagePacks final {
   public:
    enum class Status : uint8_t {
        kIdle,
        kDownloading,
        kReady,
        kFailed,
        kCancelled,
        kNoSpace,
        kChecking,
        kAwaitingConfirmation,
        kCurrent
    };
    using CommitSetting = bool (*)(void*);
    using Prepare = bool (*)(std::span<const uint8_t>);
    using Commit = bool (*)(CommitSetting, void*);
    using Abort = void (*)();
    LanguagePacks(runtime::AppStore& store, Prepare prepare, Commit commit, Abort abort)
        : store_(store), prepare_(prepare), commit_(commit), abort_(abort) {}
    void BindWake(void (*wake)(void*), void* context) {
        wake_ = wake;
        wake_context_ = context;
    }
    [[nodiscard]] FontDownload& download() { return download_; }
    // Consumes the advisory result piggybacked on the ordinary App Store check.
    void SetUpdateVersion(std::string_view app_id, std::string_view current_version, std::string_view version);
    [[nodiscard]] const char* active_id() const { return active_id_.data(); }
    [[nodiscard]] const char* active_locale() const { return active_locale_.data(); }
    [[nodiscard]] bool update_available() const { return update_available_; }
    [[nodiscard]] const char* update_version() const { return update_version_.data(); }
    [[nodiscard]] const char* offered_version() const {
        return status_ == Status::kChecking ? update_version_.data() : download_.version.data();
    }
    [[nodiscard]] const char* active_version() const { return active_version_.data(); }
    [[nodiscard]] bool StartUpdate();
    [[nodiscard]] bool Restore(std::string_view locale);
    [[nodiscard]] bool Start(uint32_t index, bool refresh = false);
    [[nodiscard]] bool Confirm();
    void Cancel();
    [[nodiscard]] uint32_t download_size() const { return download_size_; }
    [[nodiscard]] uint32_t required_space() const { return required_space_; }
    [[nodiscard]] uint32_t free_space() const { return free_space_; }
    [[nodiscard]] Status status();
    [[nodiscard]] uint8_t progress() const { return download_.progress.load(); }
    [[nodiscard]] uint32_t selected() const { return selected_; }
    [[nodiscard]] bool Apply(CommitSetting setting, void* context);

   private:
    bool CheckSpace();
    bool Load(const char* id, std::string_view locale);
    bool PrepareSource(const micropixel_bundle_source_t& source, std::string_view locale);
    bool Cached(std::string_view locale);
    bool Matches(std::string_view locale) const;
    void Cleanup();
    void DiscardCandidate();
    runtime::AppStore& store_;
    Prepare prepare_{};
    Commit commit_{};
    Abort abort_{};
    void (*wake_)(void*){};
    void* wake_context_{};
    FontDownload download_{};
    std::array<char, 32U> active_locale_{};
    std::array<char, 32U> active_version_{};
    std::array<char, 32U> candidate_version_{};
    std::array<char, 32U> update_version_{};
    bool update_available_{};
    Status status_{Status::kIdle};
    uint32_t selected_{};
    uint32_t download_size_{};
    uint32_t required_space_{};
    uint32_t free_space_{};
    bool cached_{};
    bool updating_{};
    int64_t deadline_us_{};
    micropixel_bundle_font_mapping_t active_{};
    micropixel_bundle_font_mapping_t candidate_{};
    std::array<char, 65U> active_id_{};
    std::array<char, 65U> candidate_id_{};
    // Fixed workspaces belong to this PSRAM object, never the Host task stack.
    micropixel_bundle_metadata_t metadata_{};
    micropixel_bundle_source_t source_{};
    runtime::AppInstallRequest install_request_{};
    bundlefs_file_t file_{};
    std::array<bundlefs_file_info_t, BUNDLEFS_MAX_FILES> files_{};
};
}  // namespace micropixel::host::fonts
