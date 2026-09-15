// SPDX-License-Identifier: Apache-2.0
// Share the AppStore contract fixture; the real Bundle parser is exercised by
// test_bundle_reader with both CBIN and TTF components.
#define main app_store_fixture_main
#include "test_app_store.cpp"
#undef main
#include "host/fonts/language_packs.hpp"
using micropixel::host::fonts::LanguagePacks;
using State = micropixel::host::fonts::FontDownload::State;
namespace {
bool prepare_fails{}, setting_fails{};
unsigned activated{}, settings_saved{}, aborted{}, prepared{};
bool PrepareFont(std::span<const uint8_t>) {
    ++prepared;
    return !prepare_fails;
}
bool CommitFont(LanguagePacks::CommitSetting setting, void* context) {
    if (setting && !setting(context)) return false;
    ++activated;
    return true;
}
void AbortFont() { ++aborted; }
bool SaveSetting(void*) {
    if (setting_fails) return false;
    ++settings_saved;
    return true;
}
}  // namespace
extern "C" bool micropixel_bundle_open_component_font(const micropixel_bundle_source_t* source,
                                                      micropixel_bundle_metadata_t* metadata,
                                                      micropixel_bundle_font_mapping_t* mapping) {
    if (!micropixel_validate_component_package(source, metadata) || !micropixel_bundle_source_can_map(source))
        return false;
    if (!micropixel_bundle_source_map(source, 0U, metadata->bundle_size, &mapping->mapping)) return false;
    mapping->font.data = mapping->mapping.data;
    mapping->font.size = metadata->bundle_size;
    return true;
}
extern "C" void micropixel_close_font_mapping(micropixel_bundle_font_mapping_t* mapping) {
    micropixel_bundle_mapping_release(&mapping->mapping);
    *mapping = {};
}
void TestFontUpdates() {
    FakeStore system(kNorCapacity);
    const auto old_bytes = MakeBundle("fonts.zh-cn", 0x41U);
    const auto new_bytes = MakeBundle("fonts.zh-cn", 0x42U);
    system.Seed("fonts.zh-cn", old_bytes);
    micropixel::runtime::AppStore store(system);
    micropixel::runtime::InstalledAppCatalog catalog{};
    Check(store.LoadCatalog(catalog).has_value(), "mount update store");
    LanguagePacks packs(store, PrepareFont, CommitFont, AbortFont);
    Check(packs.Restore("zh-CN") && std::string_view(packs.active_version()) == "1.0.0",
          "restore current font version");
    const auto describe = [&](auto& job) {
        job.size = new_bytes.size();
        job.sha256 = Hash(new_bytes);
        std::strcpy(job.app_id.data(), "fonts.zh-cn");
        std::strcpy(job.version.data(), "1.1.0");
        job.state = State::kResolved;
    };
    const auto finish = [&] {
        auto& job = packs.download();
        job.bytes = static_cast<uint8_t*>(std::malloc(new_bytes.size()));
        std::memcpy(job.bytes, new_bytes.data(), new_bytes.size());
        job.state = State::kReady;
    };
    const auto before = prepared;
    packs.SetUpdateVersion("fonts.zh-cn", "1.0.0", "1.1.0");
    Check(packs.update_available() && std::string_view(packs.update_version()) == "1.1.0" && prepared == before &&
              system.files[0].data == old_bytes && packs.download().state == State::kIdle,
          "piggybacked result offers an update without preparing or issuing a font request");
    Check(packs.StartUpdate(), "update rechecks the release before confirmation");
    packs.download().state = State::kFailed;
    Check(packs.status() == LanguagePacks::Status::kFailed && packs.update_available() && !packs.Confirm(),
          "failed refresh preserves badge and never reports cached version as successful update");

    Check(packs.StartUpdate(), "retry download after failed refresh");
    describe(packs.download());
    Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && packs.Confirm(), "confirm failed transfer");
    packs.download().state = State::kFailed;
    Check(packs.status() == LanguagePacks::Status::kFailed && packs.update_available() &&
              system.files[0].data == old_bytes,
          "failed update transfer retains old bytes and must not activate cached font as an update");

    for (bool fail_prepare : {true, false}) {
        Check(packs.StartUpdate(), "retry font update");
        describe(packs.download());
        Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && packs.Confirm(),
              "confirm newer release");
        prepare_fails = fail_prepare;
        system.fail_next_commit = !fail_prepare;
        finish();
        Check(packs.status() == LanguagePacks::Status::kFailed && !system.writer_active &&
                  system.files[0].data == old_bytes,
              "preparation or commit failure aborts staged replacement and keeps committed old bytes");
        prepare_fails = false;
        LanguagePacks rebooted(store, PrepareFont, CommitFont, AbortFont);
        Check(rebooted.Restore("zh-CN") && std::string_view(rebooted.active_version()) == "1.0.0",
              "old font remains restorable after failed same-ID update");
    }
    Check(packs.StartUpdate(), "retry successful update");
    describe(packs.download());
    Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && packs.Confirm(),
          "confirm successful update");
    finish();
    const auto activations_before = activated;
    Check(packs.status() == LanguagePacks::Status::kReady && system.files[0].data == new_bytes &&
              activated == activations_before,
          "staged font is prepared and committed before activation");
    Check(packs.Apply(nullptr, nullptr) && !packs.update_available() &&
              std::string_view(packs.active_version()) == "1.1.0" && activated == activations_before + 1U &&
              system.file_count == 1U,
          "update activates new version, removes badge, and retains only one component");
    packs.SetUpdateVersion("fonts.zh-cn", "1.0.0", "2.0.0");
    Check(!packs.update_available(), "late pre-upgrade result cannot recreate badge");
    packs.SetUpdateVersion("fonts.zh-cn", "1.1.0", "1.1.0");
    Check(!packs.update_available() && !packs.StartUpdate(), "same version is not an update");

    LanguagePacks stale(store, PrepareFont, CommitFont, AbortFont);
    Check(stale.Restore("zh-CN"), "restore for stale offer");
    stale.SetUpdateVersion("fonts.zh-cn", "1.1.0", "2.0.0");
    Check(stale.update_available() && stale.StartUpdate(), "major font update is allowed");
    describe(stale.download());
    Check(stale.status() == LanguagePacks::Status::kCurrent && !stale.update_available() && !stale.Confirm(),
          "withdrawn release clears offer and cannot reinstall current version");
    stale.Cancel();
    Check(stale.status() == LanguagePacks::Status::kCancelled, "up-to-date panel closes to an idle-capable state");

    LanguagePacks switched(store, PrepareFont, CommitFont, AbortFont);
    Check(switched.Restore("zh-CN"), "restore before locale change");
    Check(switched.Start(0) && switched.Confirm() && switched.Apply(nullptr, nullptr),
          "switch before metadata response");
    switched.SetUpdateVersion("fonts.zh-cn", "1.1.0", "2.0.0");
    Check(!switched.update_available() && switched.download().state == State::kIdle,
          "late old-locale response cannot add an English font badge or issue a request");
}

int main() {
    FakeStore system(kNorCapacity), nand(kNorCapacity, false, 2U);
    micropixel::runtime::AppStore store(system, &nand);
    micropixel::runtime::InstalledAppCatalog catalog{};
    Check(store.LoadCatalog(catalog).has_value(), "mount both stores");
    LanguagePacks packs(store, PrepareFont, CommitFont, AbortFont);
    const auto bytes = MakeBundle("fonts.zh-cn", 0x41U);
    const auto describe = [&](auto& job) {
        job.size = bytes.size();
        job.sha256 = Hash(bytes);
        std::strcpy(job.app_id.data(), "fonts.zh-cn");
        std::strcpy(job.version.data(), "1.0.0");
        job.state = State::kResolved;
    };
    const auto finish_download = [&](auto& job) {
        job.bytes = static_cast<uint8_t*>(std::malloc(bytes.size()));
        std::memcpy(job.bytes, bytes.data(), bytes.size());
        job.state = State::kReady;
    };
    auto& job = packs.download();
    Check(packs.Restore("en"), "English boots offline");
    Check(!packs.Start(99), "invalid locale rejected");
    Check(packs.Start(1) && !packs.Start(2) && !packs.Confirm(), "pending discovery cannot confirm or overlap");
    packs.Cancel();
    Check(packs.status() == LanguagePacks::Status::kCancelled && job.state == State::kIdle,
          "cancel before QUIC task claims request");
    Check(packs.Start(1), "retry cancelled discovery");
    describe(job);
    Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && !job.bytes && system.file_count == 0U,
          "discovery neither downloads nor installs a font");
    Check(packs.download_size() == bytes.size() && packs.required_space() > bytes.size(),
          "preview includes required atomic-installation headroom");
    packs.Cancel();
    Check(!packs.Confirm() && system.file_count == 0U, "closing preview cannot switch or install");
    Check(packs.Start(1), "request discovery again");
    describe(job);
    Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && packs.Confirm() &&
              job.state == State::kDownloadRequested && !packs.Confirm(),
          "confirmation requests download exactly once");
    finish_download(job);
    Check(packs.status() == LanguagePacks::Status::kReady && system.file_count == 1U && nand.file_count == 0U,
          "verified font installed to NOR before activation");
    Check(packs.progress() == 95U, "installation reports byte-write progress");
    setting_fails = true;
    Check(!packs.Apply(SaveSetting, nullptr) && activated == 1U && settings_saved == 0U,
          "NVS failure preserves active bank");
    setting_fails = false;
    const auto prepared_before_preview = prepared;
    Check(packs.Start(1) && packs.status() == LanguagePacks::Status::kAwaitingConfirmation,
          "cached font still requires confirmation");
    Check(prepared == prepared_before_preview, "cached preview must not prepare or replace Guest font instances");
    Check(packs.Confirm() && packs.Apply(SaveSetting, nullptr) && settings_saved == 1U,
          "confirmed font and locale commit together");
    Check(packs.Start(1, true), "current language queries updates");
    describe(job);
    Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && packs.download_size() == 0U,
          "matching installed digest skips font download");
    packs.Cancel();
    prepare_fails = true;
    Check(packs.Start(0) && packs.status() == LanguagePacks::Status::kAwaitingConfirmation,
          "English preview does not allocate fonts");
    Check(!packs.Confirm() && packs.status() == LanguagePacks::Status::kFailed,
          "font allocation failure at confirmation cannot report ready");
    packs.Cancel();
    prepare_fails = false;
    Check(packs.Start(0) && packs.Confirm() && packs.Apply(SaveSetting, nullptr),
          "confirmed switch to built-in English without reboot");
    Check(system.file_count == 0U && nand.file_count == 0U, "old font removed only after activation");
    Check(packs.Start(1), "request before completed-transfer cancellation");
    describe(job);
    Check(packs.status() == LanguagePacks::Status::kAwaitingConfirmation && packs.Confirm(), "confirm transfer");
    finish_download(job);
    packs.Cancel();
    Check(!job.bytes && job.state == State::kIdle && packs.status() == LanguagePacks::Status::kCancelled,
          "cancel after transfer releases response without installing");
    FakeStore full_system(MICROPIXEL_BUNDLEFS_METADATA_SIZE + MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT);
    micropixel::runtime::AppStore full_store(full_system);
    Check(full_store.LoadCatalog(catalog).has_value(), "mount full system store");
    LanguagePacks full(full_store, PrepareFont, CommitFont, AbortFont);
    Check(full.Restore("en") && full.Start(1), "request discovery with insufficient NOR space");
    describe(full.download());
    const auto saved_before_failure = settings_saved;
    Check(full.status() == LanguagePacks::Status::kNoSpace && !full.download().bytes && full_system.file_count == 0U,
          "preflight rejects insufficient space before downloading");
    Check(!full.Confirm() && !full.Apply(SaveSetting, nullptr) && settings_saved == saved_before_failure,
          "space failure cannot change active language");
    FakeStore shrinking(MICROPIXEL_BUNDLEFS_METADATA_SIZE + 2U * MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT);
    micropixel::runtime::AppStore shrinking_store(shrinking);
    Check(shrinking_store.LoadCatalog(catalog).has_value(), "mount shrinking store");
    LanguagePacks race(shrinking_store, PrepareFont, CommitFont, AbortFont);
    Check(race.Start(1), "start preview before storage change");
    describe(race.download());
    Check(race.status() == LanguagePacks::Status::kAwaitingConfirmation, "preview originally fits");
    shrinking.Seed("factory", MakeBundle("factory", 0x44U));
    Check(!race.Confirm() && race.status() == LanguagePacks::Status::kNoSpace,
          "confirmation rechecks current capacity");
    Check(packs.Start(3), "query a missing language component");
    job.state = State::kFailed;
    const auto saved_before_query_failure = settings_saved;
    Check(packs.status() == LanguagePacks::Status::kFailed && !packs.Confirm() &&
              settings_saved == saved_before_query_failure,
          "failed discovery cannot confirm or change language");
    FakeStore non_mappable(kNorCapacity, false);
    non_mappable.Seed("fonts.zh-cn", bytes);
    micropixel::runtime::AppStore invalid_store(non_mappable);
    LanguagePacks invalid(invalid_store, PrepareFont, CommitFont, AbortFont);
    Check(!invalid.Restore("zh-CN"), "NAND font must never be copied into PSRAM as a fallback");
    TestFontUpdates();
    std::printf("Language component lifecycle: %u checks passed\n", checks);
}
