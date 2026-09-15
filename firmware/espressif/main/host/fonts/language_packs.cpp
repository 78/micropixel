// SPDX-License-Identifier: Apache-2.0
#include "host/fonts/language_packs.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "runtime/bundle/memory_bundle_source.h"
#include "runtime/bundlefs/bundle_store_source.hpp"

namespace micropixel::host::fonts {
namespace {
constexpr char kTag[] = "language_fonts";
using State = FontDownload::State;
}  // namespace

bool LanguagePacks::Matches(std::string_view locale) const {
    if (metadata_.package_type != MICROPIXEL_BUNDLE_PACKAGE_COMPONENT ||
        metadata_.component_type != MICROPIXEL_BUNDLE_COMPONENT_FONT ||
        metadata_.font_format != MICROPIXEL_BUNDLE_FORMAT_STATIC_TTF)
        return false;
    for (uint32_t i = 0U; i < metadata_.language_count; ++i)
        if (locale == reinterpret_cast<const char*>(metadata_.languages[i])) return true;
    return false;
}

bool LanguagePacks::Load(const char* id, std::string_view locale) {
    auto& system = store_.system_store();
    source_ = {};
    if (!system.mappable() || system.Open(id, file_) != BUNDLEFS_OK ||
        !runtime::MakeBundleSource(system, file_, source_) || !PrepareSource(source_, locale))
        return false;
    if (id != candidate_id_.data()) std::snprintf(candidate_id_.data(), candidate_id_.size(), "%s", id);
    ESP_LOGI(kTag, "prepared %s: %lu TTF bytes mapped from NOR", id, static_cast<unsigned long>(candidate_.font.size));
    return true;
}

bool LanguagePacks::PrepareSource(const micropixel_bundle_source_t& source, std::string_view locale) {
    if (!micropixel_bundle_open_component_font(&source, &metadata_, &candidate_)) return false;
    if (!Matches(locale) || !prepare_({candidate_.font.data, candidate_.font.size})) {
        DiscardCandidate();
        return false;
    }
    std::snprintf(candidate_version_.data(), candidate_version_.size(), "%s", metadata_.package_version);
    return true;
}

void LanguagePacks::SetUpdateVersion(std::string_view app_id, std::string_view current_version,
                                     std::string_view version) {
    // Ignore responses captured before a language switch or successful upgrade.
    if (app_id.empty() || app_id != active_id_.data() || current_version != active_version_.data()) return;
    update_version_.fill(0U);
    if (version.size() < update_version_.size() && !version.empty())
        std::memcpy(update_version_.data(), version.data(), version.size());
    update_available_ = micropixel_app_version_update(active_version_.data(), update_version_.data(), false);
    if (!update_available_) update_version_.fill(0U);
}

bool LanguagePacks::StartUpdate() {
    if (!update_available_) return false;
    for (size_t i = 0; i < kPacks.size(); ++i) {
        if (active_locale_.data() == std::string_view(kPacks[i].locale) && Start(i, true)) {
            updating_ = true;
            return true;
        }
    }
    return false;
}

bool LanguagePacks::Cached(std::string_view locale) {
    if (locale == "en") return true;
    auto& system = store_.system_store();
    uint32_t count{};
    if (!system.mappable() || system.Mount() != BUNDLEFS_OK ||
        system.List(files_.data(), files_.size(), count) != BUNDLEFS_OK)
        return false;
    for (uint32_t i = 0U; i < count; ++i) {
        source_ = {};
        if (system.Open(files_[i].name, file_) == BUNDLEFS_OK && runtime::MakeBundleSource(system, file_, source_) &&
            micropixel_read_bundle_metadata(&source_, &metadata_) && Matches(locale)) {
            std::snprintf(candidate_id_.data(), candidate_id_.size(), "%s", files_[i].name);
            return true;
        }
    }
    return false;
}

bool LanguagePacks::Restore(std::string_view locale) {
    if (status_ != Status::kIdle || active_.font.data || !Cached(locale)) return false;
    if (!(locale == "en" ? prepare_({}) : Load(candidate_id_.data(), locale))) return false;
    if (!commit_(nullptr, nullptr)) {
        DiscardCandidate();
        return false;
    }
    active_ = candidate_;
    candidate_ = {};
    active_id_ = candidate_id_;
    active_version_ = candidate_version_;
    std::snprintf(active_locale_.data(), active_locale_.size(), "%.*s", static_cast<int>(locale.size()), locale.data());
    candidate_id_.fill(0U);
    return true;
}

void LanguagePacks::DiscardCandidate() {
    abort_();
    micropixel_close_font_mapping(&candidate_);
    candidate_id_.fill(0U);
    candidate_version_.fill(0U);
}

bool LanguagePacks::CheckSpace() {
    bundlefs_store_info_t info{};
    if (store_.system_store().GetStoreInfo(info) != BUNDLEFS_OK || !store_.system_store().mappable() ||
        !info.data_block_size)
        return cached_;
    free_space_ = static_cast<uint32_t>(info.free_bytes);
    required_space_ = cached_ ? 0U : download_size_ + info.data_block_size;
    if (required_space_ > free_space_) {
        status_ = Status::kNoSpace;
        return false;
    }
    return true;
}

bool LanguagePacks::Start(uint32_t index, bool refresh) {
    const auto current = status();
    if (index >= kPacks.size() || current == Status::kDownloading || current == Status::kChecking) return false;
    DiscardCandidate();
    selected_ = index;
    updating_ = false;
    download_size_ = required_space_ = free_space_ = 0U;
    download_.progress.store(0U);
    download_.cancelled.store(false);
    cached_ = (!refresh || index == 0U) && Cached(kPacks[index].locale);
    if (cached_) {
        status_ = Status::kAwaitingConfirmation;
        (void)CheckSpace();
        return true;
    }
    std::snprintf(download_.locale.data(), download_.locale.size(), "%s", kPacks[index].locale);
    download_.state.store(State::kRequested, std::memory_order_release);
    deadline_us_ = esp_timer_get_time() + 30000000LL;
    status_ = Status::kChecking;
    if (wake_) wake_(wake_context_);
    return true;
}

bool LanguagePacks::Confirm() {
    if (status_ != Status::kAwaitingConfirmation) return false;
    if (!CheckSpace()) {
        if (status_ != Status::kNoSpace) status_ = Status::kFailed;
        return false;
    }
    if (cached_) {
        const auto locale = std::string_view(kPacks[selected_].locale);
        if (!(locale == "en" ? prepare_({}) : Load(candidate_id_.data(), locale))) {
            status_ = Status::kFailed;
            return false;
        }
        status_ = Status::kReady;
        download_.progress.store(95U);
    } else {
        download_.state.store(State::kDownloadRequested, std::memory_order_release);
        deadline_us_ = esp_timer_get_time() + 30000000LL;
        status_ = Status::kDownloading;
        if (wake_) wake_(wake_context_);
    }
    return true;
}

void LanguagePacks::Cancel() {
    download_.cancelled.store(true);
    auto expected = State::kRequested;
    if (download_.state.compare_exchange_strong(expected, State::kIdle)) status_ = Status::kCancelled;
    expected = State::kDownloadRequested;
    if (download_.state.compare_exchange_strong(expected, State::kIdle)) status_ = Status::kCancelled;
    const auto state = download_.state.load(std::memory_order_acquire);
    if (state == State::kReady || state == State::kFailed || state == State::kResolved) {
        heap_caps_free(download_.bytes);
        download_.bytes = nullptr;
        download_.state.store(State::kIdle, std::memory_order_release);
        status_ = Status::kCancelled;
    }
    if (status_ == Status::kReady || status_ == Status::kAwaitingConfirmation || status_ == Status::kNoSpace ||
        status_ == Status::kCurrent) {
        DiscardCandidate();
        status_ = Status::kCancelled;
    }
}

LanguagePacks::Status LanguagePacks::status() {
    if (status_ != Status::kDownloading && status_ != Status::kChecking) return status_;
    const auto state = download_.state.load(std::memory_order_acquire);
    if ((state == State::kRequested || state == State::kDownloadRequested) && esp_timer_get_time() > deadline_us_) {
        Cancel();
        if (status_ == Status::kCancelled) status_ = Status::kFailed;
        return status_;
    }
    if (status_ == Status::kChecking && (state == State::kResolved || state == State::kFailed)) {
        download_.state.store(State::kIdle, std::memory_order_release);
        if (download_.cancelled.load()) {
            status_ = Status::kCancelled;
            return status_;
        }
        if (updating_) {
            if (state == State::kFailed) {
                status_ = Status::kFailed;
                return status_;
            }
            if (std::string_view(download_.app_id.data()) != active_id_.data()) {
                status_ = Status::kFailed;
                return status_;
            }
            if (!micropixel_app_version_update(active_version_.data(), download_.version.data(), false)) {
                update_available_ = false;
                update_version_.fill(0U);
                status_ = Status::kCurrent;
                return status_;
            }
        }
        download_size_ = state == State::kResolved ? static_cast<uint32_t>(download_.size) : 0U;
        cached_ = false;
        auto& system = store_.system_store();
        uint32_t count{};
        if (state == State::kResolved && system.List(files_.data(), files_.size(), count) == BUNDLEFS_OK) {
            for (uint32_t i = 0U; i < count; ++i)
                if (std::string_view(files_[i].name) == download_.app_id.data() &&
                    std::memcmp(files_[i].sha256, download_.sha256.data(), download_.sha256.size()) == 0) {
                    cached_ = true;
                    std::snprintf(candidate_id_.data(), candidate_id_.size(), "%s", files_[i].name);
                }
        } else if (state == State::kFailed)
            cached_ = Cached(kPacks[selected_].locale);
        if (cached_) download_size_ = 0U;
        status_ = state == State::kResolved || cached_ ? Status::kAwaitingConfirmation : Status::kFailed;
        if (status_ == Status::kAwaitingConfirmation && !CheckSpace() && status_ != Status::kNoSpace)
            status_ = Status::kFailed;
        ESP_LOGI(kTag, "font preflight status=%u download=%lu required=%lu free=%lu, Host minimum free stack: %u",
                 static_cast<unsigned>(status_), static_cast<unsigned long>(download_size_),
                 static_cast<unsigned long>(required_space_), static_cast<unsigned long>(free_space_),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        return status_;
    }
    if (state != State::kReady && state != State::kFailed) return status_;
    bool success = false;
    bool no_space = false;
    if (!download_.cancelled.load() && state == State::kReady) {
        source_ = {};
        if (micropixel_memory_bundle_source(download_.bytes, download_.size, &source_) &&
            micropixel_read_bundle_metadata(&source_, &metadata_) && Matches(kPacks[selected_].locale)) {
            install_request_ =
                runtime::AppInstallRequest{.data = download_.bytes,
                                           .size = download_.size,
                                           .expected_app_id = download_.app_id.data(),
                                           .expected_sha256 = download_.sha256,
                                           .trusted_component_signature = true,
                                           .expected_version = download_.version.data(),
                                           .progress =
                                               [](void* context, uint8_t value) {
                                                   auto& job = *static_cast<FontDownload*>(context);
                                                   job.progress.store(70U + value * 25U / 100U);
                                               },
                                           .progress_context = &download_,
                                           .prepare_component =
                                               [](void* context, const micropixel_bundle_source_t& source) {
                                                   auto& packs = *static_cast<LanguagePacks*>(context);
                                                   return packs.PrepareSource(source, kPacks[packs.selected_].locale);
                                               },
                                           .prepare_context = this};
            download_.progress.store(70U);
            const auto installed = store_.Install(install_request_);
            no_space = !installed && installed.error() == runtime::AppStoreError::kNoSpace;
            success = installed && (installed->changed || Load(download_.app_id.data(), kPacks[selected_].locale));
            if (success)
                std::snprintf(candidate_id_.data(), candidate_id_.size(), "%s", download_.app_id.data());
            else
                DiscardCandidate();
        }
    }
    heap_caps_free(download_.bytes);
    download_.bytes = nullptr;
    download_.state.store(State::kIdle, std::memory_order_release);
    status_ = download_.cancelled.load() ? Status::kCancelled
              : success                  ? Status::kReady
              : no_space                 ? Status::kNoSpace
                                         : Status::kFailed;
    ESP_LOGI(kTag, "font preparation status=%u, Host minimum free stack: %u", static_cast<unsigned>(status_),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return status_;
}

bool LanguagePacks::Apply(CommitSetting setting, void* context) {
    if (status_ != Status::kReady || !commit_(setting, context)) {
        DiscardCandidate();
        status_ = Status::kFailed;
        return false;
    }
    micropixel_close_font_mapping(&active_);
    active_ = candidate_;
    candidate_ = {};
    active_id_ = candidate_id_;
    active_version_ = candidate_version_;
    std::snprintf(active_locale_.data(), active_locale_.size(), "%s", kPacks[selected_].locale);
    update_available_ = false;
    update_version_.fill(0U);
    candidate_id_.fill(0U);
    status_ = Status::kIdle;
    download_.progress.store(100U);
    Cleanup();
    ESP_LOGI(kTag, "language committed, Host minimum free stack: %u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
}

void LanguagePacks::Cleanup() {
    auto& system = store_.system_store();
    uint32_t count{};
    if (system.List(files_.data(), files_.size(), count) == BUNDLEFS_OK) {
        for (uint32_t i = 0U; i < count; ++i) {
            if (std::string_view(files_[i].name) == active_id_.data()) continue;
            source_ = {};
            if (system.Open(files_[i].name, file_) == BUNDLEFS_OK &&
                runtime::MakeBundleSource(system, file_, source_) &&
                micropixel_read_bundle_metadata(&source_, &metadata_) &&
                metadata_.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT &&
                metadata_.component_type == MICROPIXEL_BUNDLE_COMPONENT_FONT)
                (void)store_.UninstallComponent(files_[i].name, active_id_.data());
        }
    }
    // Migration from the pre-component implementation: remove only its exact files.
    for (const auto& pack : kPacks) {
        std::array<char, 48U> name{};
        std::snprintf(name.data(), name.size(), ".font.%s", pack.locale);
        (void)system.Remove(name.data());
        if (&store_.app_store() != &system) (void)store_.app_store().Remove(name.data());
    }
}
}  // namespace micropixel::host::fonts
