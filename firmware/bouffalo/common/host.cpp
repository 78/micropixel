// SPDX-License-Identifier: Apache-2.0
#include "audio_output.hpp"
#include "console.h"
#include "desktop.hpp"
#include "device/device_services.hpp"
#include "device/text.hpp"
#include "graphics.hpp"
#include "input.hpp"
#include "mpx_board.h"
#include "sd_app_storage.hpp"
#include "storage.hpp"
extern "C" {
#include "cJSON.h"
}
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>

#include "device/device_registry.hpp"
#include "esp_heap_caps.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/defaults/unavailable_services.hpp"
#include "runtime/app_runtime.hpp"
#include "runtime/bundle/app_store.hpp"
#include "runtime/bundlefs/bundlefs.hpp"
#include "runtime/guest_log_sink.hpp"
#include "work/background_executor.hpp"
extern "C" {
#include "bflb_mtimer.h"
#include "bflb_sec_trng.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "sd_storage.h"

bool micropixel_psram_check(size_t* tested_bytes);
}
namespace {
namespace r = micropixel::runtime;
namespace d = micropixel::device;
namespace device = micropixel::device;
using mpx_port::AudioOutput;
using mpx_port::DesktopHit;
using mpx_port::DesktopHomeHit;
using mpx_port::DesktopItem;
using mpx_port::FlashStorage;
using mpx_port::Graphics;
using mpx_port::InitializeKv;
using mpx_port::Input;
using mpx_port::kDesktopHome;
using mpx_port::kDesktopNext;
using mpx_port::kDesktopPrevious;
using mpx_port::SdAppStorage;
using mpx_port::SetDesktopGuestOverlay;
class Random final : public d::Random {
   public:
    int32_t GetU32(uint32_t& value) override {
        // LHAL registers this peripheral as "sec_trng". Unknown names assert
        // inside bflb_device_get_by_name(), before a null check can run.
        auto* device = bflb_device_get_by_name(BFLB_NAME_SEC_TRNG);
        if (!device) return MICROPIXEL_STATUS_INTERNAL;
        return bflb_trng_readlen(reinterpret_cast<uint8_t*>(&value), sizeof(value)) == 0 ? MICROPIXEL_STATUS_OK
                                                                                         : MICROPIXEL_STATUS_INTERNAL;
    }
};
class Logs final : public r::GuestLogSink {
    struct Record {
        uint32_t seq, level;
        uint64_t time;
        char id[65];
        uint8_t bytes[384];
        size_t n;
    };
    BouffaloMutex mutex_;
    Record records_[16]{};
    uint32_t seq_{};

   public:
    void WriteGuestLog(const char* id, uint32_t level, const uint8_t* b, size_t n, uint64_t ts) override {
        std::lock_guard l(mutex_);
        auto& v = records_[seq_ % 16];
        v.seq = ++seq_;
        v.time = ts;
        v.level = level;
        snprintf(v.id, sizeof(v.id), "%s", id);
        v.n = n > sizeof(v.bytes) ? sizeof(v.bytes) : n;
        memcpy(v.bytes, b, v.n);
    }
    void Page(uint32_t request, uint32_t sequence);
};
static Graphics graphics;
static Input input;
static FlashStorage flash;
static Random random_source;
static d::DeviceRegistry registry;
static micropixel::platform::defaults::UnavailableBattery battery;
static micropixel::platform::defaults::UnavailableAudio unavailable_audio;
static AudioOutput audio_output(MPX_AUDIO_NAME);
static micropixel::platform::audio::AudioEngine audio;
static Logs logs;
static std::optional<r::AppRuntime> runtime;
static std::optional<r::BundleFs> bundlefs;
static std::optional<r::AppStore> store;
static SdAppStorage sd_apps;
static std::optional<r::BundleFs> sd_bundlefs;
static const char* sd_attach_error = "not_attached";
static r::InstalledAppCatalog catalog;
static r::InstalledApp selected;
static std::atomic<bool> running{}, completed{};
static r::AppRunOutcome last_outcome;
static std::atomic<bool> failed{};
static micropixel_app_environment_t environment{};
static char line[6200], reply[6200];
static uint8_t chunk[4096];
static char install_id[65];
static size_t install_size{}, install_received{};
static std::array<uint8_t, 32> install_sha{};
static bool installing{};
static TickType_t install_activity{};
static micropixel_system_launch_arguments_response_t launch{};
static bool desktop_dirty = true;
static bool display_testing;
static uint32_t desktop_page{};
static std::atomic<uint32_t> desktop_generation{};
struct DesktopTap {
    int32_t x, y;
    uint32_t generation;
    bool home;
};
static StaticQueue_t desktop_queue_storage;
static uint8_t desktop_queue_bytes[8 * sizeof(DesktopTap)];
static QueueHandle_t desktop_queue{};

// Called under Input's mutex. Queue only; the supervisor owns app lifecycle,
// catalog writes and rendering. Never stop or draw from the touch task.
bool DesktopTouch(void*, const d::TouchSample& sample) {
    static bool captured[16]{};
    static int32_t down_target[16]{};
    static uint32_t generation[16]{};
    if (sample.id >= 16) return false;
    const auto id = sample.id;
    const bool home = running.load();
    const auto target = [&]() {
        return home ? (DesktopHomeHit(sample.x, sample.y, graphics.Width()) ? kDesktopHome : -1)
                    : DesktopHit(sample.x, sample.y, graphics.Width(), graphics.Height());
    };
    if (sample.phase == d::TouchPhase::kDown) {
        captured[id] = !home || target() == kDesktopHome;
        down_target[id] = target();
        generation[id] = desktop_generation.load();
    }
    if (!captured[id]) return false;
    if (sample.phase == d::TouchPhase::kUp) {
        if (down_target[id] != -1 && target() == down_target[id]) {
            DesktopTap tap{sample.x, sample.y, generation[id], home};
            (void)xQueueSend(desktop_queue, &tap, 0);
        }
        captured[id] = false;
    } else if (sample.phase == d::TouchPhase::kCancel)
        captured[id] = false;
    return true;
}
void RenderDesktop() {
    const uint32_t pages = catalog.count ? (catalog.count + 5) / 6 : 1;
    if (desktop_page >= pages) desktop_page = pages - 1;
    DesktopItem items[6]{};
    const uint32_t start = desktop_page * 6;
    const uint32_t count = std::min<uint32_t>(6, catalog.count - start);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& a = catalog.apps[start + i];
        items[i] = {a.app_id.data(), a.display_name.data(), a.version.data(), a.bundle_size,
                    a.storage == r::AppStorage::kExternal};
    }
    SetDesktopGuestOverlay(false);
    desktop_dirty = !graphics.ShowDesktop(
        items, count, desktop_page, catalog.count, catalog.store_used_bytes, catalog.store_total_bytes,
        failed.load() ? "Last app failed. Diagnostics are available over UART."
                      : "Tap an app to launch. The house button returns here without rebooting.");
}

void Reply(uint32_t id, bool success, const char* fmt, ...) {
    int prefix = snprintf(reply, sizeof(reply), "MPX1 %lu %s ", (unsigned long)id, success ? "OK" : "ERROR");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reply + prefix, sizeof(reply) - prefix, fmt, ap);
    va_end(ap);
    mpx_console_line(reply);
}
bool Number(const char* s, uint32_t& n) {
    if (!s || !*s) return false;
    uint64_t value = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        value = value * 10 + (*s - '0');
        if (value > UINT32_MAX) return false;
    }
    n = value;
    return true;
}
bool Hex(const char* s, std::array<uint8_t, 32>& out) {
    if (!s || strlen(s) != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned n = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = s[2 * i + j];
            unsigned v = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
            if (v > 15) return false;
            n = n * 16 + v;
        }
        out[i] = n;
    }
    return true;
}
void HexString(const std::array<uint8_t, 32>& s, char (&out)[65]) {
    const char* h = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        out[2 * i] = h[s[i] >> 4];
        out[2 * i + 1] = h[s[i] & 15];
    }
    out[64] = 0;
}
bool Base64(const uint8_t* b, size_t n, char* out, size_t capacity) {
    size_t written{};
    if (mbedtls_base64_encode((uint8_t*)out, capacity - 1, &written, b, n)) return false;
    out[written] = 0;
    return true;
}
bool Decode64(const char* s, uint8_t* out, size_t capacity, size_t& size) {
    return s && mbedtls_base64_decode(out, capacity, &size, (const uint8_t*)s, strlen(s)) == 0;
}
void Logs::Page(uint32_t id, uint32_t after) {
    std::lock_guard l(mutex_);
    uint32_t first = seq_ > 15 ? seq_ - 15 : 1;
    uint32_t wanted = after + 1;
    if (wanted < first) wanted = first;
    if (wanted > seq_) {
        Reply(id, true, "LOG_PAGE boot %lu 0 0 0", (unsigned long)seq_);
        return;
    }
    auto& v = records_[(wanted - 1) % 16];
    char encoded[520];
    Base64(v.bytes, v.n, encoded, sizeof(encoded));
    Reply(id, true, "LOG_PAGE boot %lu %u %u 1 %lu %llu %lu app %s %s", (unsigned long)wanted, after + 1 < first,
          wanted < seq_, (unsigned long)v.seq, (unsigned long long)v.time, (unsigned long)v.level, v.id, encoded);
}
bool Reload() {
    desktop_dirty = true;
    desktop_generation.fetch_add(1);
    auto result = store->LoadCatalog(catalog);
    if (!result) {
        mpx_console_line("[micropixel] catalog load failed");
        return false;
    }
    return true;
}
bool AttachSdApps(bool create, bool reformat_pending = false) {
    if (sd_bundlefs) return Reload() && store->split();
    if (!sd_apps.Initialize(create, reformat_pending)) {
        sd_attach_error = sd_apps.error();
        (void)sd_apps.Close();
        return false;
    }
    sd_bundlefs.emplace(sd_apps);
    bool good = true;
    if (sd_apps.pending()) {
        const auto formatted = sd_bundlefs->Format();
        if (formatted != BUNDLEFS_OK) {
            char message[100];
            snprintf(message, sizeof(message), "[micropixel] SD library format verification failed: %d", formatted);
            mpx_console_line(message);
            sd_attach_error = "format_verification_failed";
            good = false;
        }
        if (good) {
            good = sd_apps.Publish();
            if (!good) sd_attach_error = sd_apps.error();
        }
    }
    if (good) {
        good = sd_bundlefs->Mount() == BUNDLEFS_OK;
        if (!good) sd_attach_error = "catalog_invalid_preserved";
    }
    if (!good) {
        if (!sd_apps.available()) sd_attach_error = sd_apps.error();
        mpx_console_line("[micropixel] SD app library unavailable; existing image preserved");
        sd_bundlefs.reset();
        (void)sd_apps.Close();
        return false;
    }
    store.emplace(*bundlefs, &*sd_bundlefs);
    good = Reload() && store->split();
    sd_attach_error = good ? "ready" : "catalog_load_failed";
    if (good) mpx_console_line("[micropixel] SD app library ready: /sd/micropixel/apps.bfs (64 MiB)");
    return good;
}
bool DetachSdApps() {
    store.emplace(*bundlefs);
    sd_bundlefs.reset();
    selected.source = {};
    const bool closed = sd_apps.Close();
    sd_storage_release();
    sd_attach_error = closed ? "ejected" : "eject_failed";
    return Reload() && closed;
}
bool CopyAppToSd(const char* app) {
    if (!app || !sd_bundlefs || !store->split()) return false;
    bundlefs_file_t source{};
    bundlefs_file_info_t info{};
    if (bundlefs->Open(app, source) != BUNDLEFS_OK || bundlefs->GetFileInfo(source, info) != BUNDLEFS_OK) return false;
    std::array<uint8_t, 32> sha{};
    if (bundlefs->GetFileSha256(app, sha.data()) != BUNDLEFS_OK) return false;
    bundlefs_writer_t writer{};
    if (sd_bundlefs->BeginReplace(app, info.size, writer) != BUNDLEFS_OK) return false;
    bool good = true;
    for (uint32_t offset = 0; good && offset < info.size;) {
        const uint32_t count = std::min<uint32_t>(sizeof(chunk), info.size - offset);
        good = bundlefs->Read(source, offset, chunk, count) == BUNDLEFS_OK &&
               sd_bundlefs->Write(writer, chunk, count) == BUNDLEFS_OK;
        offset += count;
    }
    if (good) good = sd_bundlefs->Commit(writer, sha.data()) == BUNDLEFS_OK;
    if (!good) sd_bundlefs->Abort(writer);
    // Retain the Flash copy and KV as a fallback. Catalog precedence shows SD once.
    return Reload() && good;
}
void Guest(void*) {
    last_outcome = runtime->RunApp(selected, launch);
    failed.store(last_outcome.completion == r::AppCompletion::kFailed);
    SetDesktopGuestOverlay(false);
    desktop_generation.fetch_add(1);
    running.store(false, std::memory_order_release);
    completed.store(true);
    vTaskDelete(nullptr);
}
bool Stop() {
    display_testing = false;
    desktop_dirty = true;
    if (!running.load()) return true;
    (void)runtime->RequestStop();
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(4000);
    while (running.load() && (int32_t)(end - xTaskGetTickCount()) > 0) vTaskDelay(pdMS_TO_TICKS(10));
    if (running.load()) {
        (void)runtime->ForceStop();
        end = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (running.load() && (int32_t)(end - xTaskGetTickCount()) > 0) vTaskDelay(1);
    }
    return !running.load();
}
bool DecodeLaunchArguments(std::string_view encoded, micropixel_system_launch_arguments_response_t& launch_arguments) {
    launch_arguments = {};
    launch_arguments.size = sizeof(launch_arguments);
    if (encoded.empty()) {
        return true;
    }
    size_t decoded_size = 0U;
    if (mbedtls_base64_decode(reinterpret_cast<uint8_t*>(launch_arguments.bytes), sizeof(launch_arguments.bytes),
                              &decoded_size, reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()) != 0 ||
        decoded_size == 0U || decoded_size > UINT16_MAX || launch_arguments.bytes[decoded_size - 1U] != '\0') {
        return false;
    }
    launch_arguments.bytes_length = static_cast<uint16_t>(decoded_size);
    size_t offset = 0U;
    while (offset < decoded_size) {
        if (launch_arguments.count >= MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT) {
            return false;
        }
        launch_arguments.offsets[launch_arguments.count++] = static_cast<uint16_t>(offset);
        while (offset < decoded_size && launch_arguments.bytes[offset] != '\0') {
            ++offset;
        }
        if (offset == decoded_size) {
            return false;
        }
        const uint32_t argument_offset = launch_arguments.offsets[launch_arguments.count - 1U];
        const uint32_t argument_length = static_cast<uint32_t>(offset - argument_offset);
        if (argument_length != 0U &&
            !device::IsValidUtf8(reinterpret_cast<const uint8_t*>(launch_arguments.bytes + argument_offset),
                                 argument_length)) {
            return false;
        }
        ++offset;
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

bool Start(const char* id, const char* encoded = nullptr) {
    if (!id || installing) return false;
    for (uint32_t i = 0; i < catalog.count; ++i)
        if (!strcmp(id, catalog.apps[i].app_id.data())) {
            // The active Guest still owns launch until it has stopped.
            static micropixel_system_launch_arguments_response_t next_launch{};
            if (!DecodeLaunchArguments(encoded ? std::string_view(encoded) : std::string_view{}, next_launch) ||
                !Stop())
                return false;
            launch = next_launch;
            selected = catalog.apps[i];
            failed.store(false);
            completed.store(false);
            desktop_generation.fetch_add(1);
            SetDesktopGuestOverlay(true);
            running.store(true);
            if (xTaskCreate(Guest, "mpx_guest", 8192, nullptr, 3, nullptr) != pdPASS) {
                running.store(false);
                SetDesktopGuestOverlay(false);
                return false;
            }
            return true;
        }
    return false;
}
void DesktopProcess() {
    if (completed.exchange(false)) desktop_dirty = true;
    DesktopTap tap{};
    while (xQueueReceive(desktop_queue, &tap, 0) == pdTRUE) {
        if (tap.generation != desktop_generation.load() || installing) continue;
        if (tap.home) {
            if (Stop()) desktop_dirty = true;
            continue;
        }
        if (running.load()) continue;
        if (display_testing) {
            display_testing = false;
            desktop_dirty = true;
            continue;
        }
        const int target = DesktopHit(tap.x, tap.y, graphics.Width(), graphics.Height());
        if (target == kDesktopPrevious && desktop_page) {
            --desktop_page;
            desktop_generation.fetch_add(1);
            desktop_dirty = true;
        } else if (target == kDesktopNext && (desktop_page + 1) * 6 < catalog.count) {
            ++desktop_page;
            desktop_generation.fetch_add(1);
            desktop_dirty = true;
        } else if (target >= 0) {
            const uint32_t index = desktop_page * 6 + static_cast<uint32_t>(target);
            if (index < catalog.count) (void)Start(catalog.apps[index].app_id.data());
        }
    }
    if (!running.load() && !installing && !display_testing && desktop_dirty) RenderDesktop();
}
void InputTask(void*) {
    for (;;) {
        input.Poll();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}
void AbortInstall() {
    store->AbortAppInstall();
    installing = false;
    install_received = install_size = 0;
    install_id[0] = 0;
}
bool Sequence(const char* encoded) {
    size_t size{};
    if (!Decode64(encoded, chunk, sizeof(chunk) - 1, size) || size > 3072) return false;
    chunk[size] = 0;
    cJSON* root = cJSON_ParseWithLength((char*)chunk, size);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    struct Operation {
        uint32_t delay;
        bool key;
        d::TouchSample touch;
        d::KeySample press;
    };
    static Operation operations[64];
    int count = cJSON_GetArraySize(root);
    bool valid = count > 0 && count <= 64;
    uint32_t total = 0;
    for (int i = 0; valid && i < count; ++i) {
        auto* src = cJSON_GetArrayItem(root, i);
        auto& o = operations[i];
        o = {};
        const char* type = JsonText(src, "type");
        valid = type && JsonUnsigned(src, "delayMs", 5000, o.delay) && o.delay <= 60000 - total;
        total += o.delay;
        if (!valid) break;
        if (!strcmp(type, "touch")) {
            uint32_t x{}, y{}, pressure{};
            valid = ParseTouchPhase(JsonText(src, "phase"), o.touch.phase) && JsonUnsigned(src, "id", 15, o.touch.id) &&
                    JsonUnsigned(src, "x", graphics.Width() - 1, x) &&
                    JsonUnsigned(src, "y", graphics.Height() - 1, y) &&
                    JsonUnsigned(src, "pressurePerMille", 1000, pressure);
            o.touch.x = x;
            o.touch.y = y;
            o.touch.pressure_per_mille = pressure;
        } else if (!strcmp(type, "key")) {
            o.key = true;
            valid = ParseKeyCode(JsonText(src, "code"), o.press.code) &&
                    ParseKeyPhase(JsonText(src, "phase"), o.press.phase) &&
                    JsonUnsigned(src, "repeatCount", 1000, o.press.repeat_count) &&
                    ((o.press.phase == d::KeyPhase::kRepeat) == (o.press.repeat_count != 0));
        } else
            valid = false;
    }
    cJSON_Delete(root);
    if (!valid) return false;
    for (int i = 0; i < count; ++i) {
        auto& o = operations[i];
        if (o.delay) vTaskDelay(pdMS_TO_TICKS(o.delay));
        uint64_t ts = bflb_mtimer_get_time_us();
        if (o.key) {
            o.press.timestamp_us = ts;
            if (!input.InjectKey(o.press)) return false;
        } else {
            o.touch.timestamp_us = ts;
            if (!input.InjectTouch(o.touch)) return false;
        }
    }
    return true;
}
void Capture() {
    static uint32_t sequence{};
    uint8_t* jpeg{};
    size_t size{};
    if (!graphics.CaptureJpeg(jpeg, size)) {
        mpx_console_line("[micropixel] screenshot allocation failed");
        return;
    }
    char begin[120], end[80];
    ++sequence;
    snprintf(begin, sizeof(begin), "\nMICROPIXEL_CAPTURE_BEGIN %lu %lu %lu JPEG %lu DISPLAY\n", (unsigned long)sequence,
             (unsigned long)graphics.Width(), (unsigned long)graphics.Height(), (unsigned long)size);
    snprintf(end, sizeof(end), "\nMICROPIXEL_CAPTURE_END %lu\n", (unsigned long)sequence);
    mpx_console_frame(begin, jpeg, size, end);
    heap_caps_free(jpeg);
}
bool ImportSd(const char* app, const char* digest, const char* path) {
    std::array<uint8_t, 32> sha{};
    if (!app || !path || std::strncmp(path, "/sd/", 4) || std::strstr(path, "..") || std::strlen(path) > 200 ||
        !Hex(digest, sha) || installing || !Stop() || !sd_storage_mount())
        return false;
    FIL file{};
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    const size_t size = f_size(&file);
    auto begin = store->BeginAppInstall(app, size);
    bool good = begin.has_value();
    size_t offset = 0;
    while (good && offset < size) {
        UINT received = 0;
        const UINT wanted = static_cast<UINT>(std::min(sizeof(chunk), size - offset));
        good = f_read(&file, chunk, wanted, &received) == FR_OK && received == wanted;
        if (good) good = store->WriteAppInstall(offset, {chunk, received}).has_value();
        offset += received;
    }
    if (f_close(&file) != FR_OK) good = false;
    if (good) {
        r::AppInstallRequest request{};
        request.size = size;
        request.expected_app_id = app;
        request.expected_sha256 = sha;
        request.environment = &environment;
        good = store->Install(request).has_value();
    }
    if (!good) store->AbortAppInstall();
    Reload();
    return good;
}
void Command(char* text) {
    if (!strcmp(text, "MICROPIXEL_CAPTURE")) {
        Capture();
        return;
    }

    char* state{};
    char* magic = strtok_r(text, " \r", &state);
    if (!magic || strcmp(magic, "MPX1")) return;
    char* rid = strtok_r(nullptr, " \r", &state);
    uint32_t id{};
    if (!Number(rid, id)) return;
    char* op = strtok_r(nullptr, " \r", &state);
    if (!op) {
        Reply(id, false, "invalid_command");
        return;
    }
    auto Arg = [&]() { return strtok_r(nullptr, " \r", &state); };
#ifdef MPX_PANEL_DIAGNOSTICS
    if (!strcmp(op, "DISPLAY_SWAP")) {
        uint32_t enabled{};
        if (!Number(Arg(), enabled) || enabled > 1) {
            Reply(id, false, "expected_0_or_1");
            return;
        }
        graphics.SetDisplayByteSwap(enabled != 0);
        Reply(id, true, "DISPLAY_SWAP %u", (unsigned)enabled);
        return;
    }
    if (!strcmp(op, "DISPLAY_PANEL")) {
        uint32_t bgr{}, inverted{};
        char* a = Arg();
        char* b = Arg();
        if (!Number(a, bgr) || !Number(b, inverted) || bgr > 1 || inverted > 1) {
            Reply(id, false, "expected_bgr_inverted_0_or_1");
            return;
        }
        graphics.SetDisplayPanelColors(bgr != 0, inverted != 0);
        Reply(id, true, "DISPLAY_PANEL %u %u", (unsigned)bgr, (unsigned)inverted);
        return;
    }
#endif
    if (!strcmp(op, "DISPLAY_TEST")) {
        if (installing || !Stop()) {
            Reply(id, false, "busy");
            return;
        }
        desktop_generation.fetch_add(1);
        SetDesktopGuestOverlay(false);
        display_testing = graphics.ShowColorTest();
        Reply(id, display_testing, "RESULT color_test");
        return;
    }
    if (!strcmp(op, "DESKTOP_HOME")) {
        bool good = Stop();
        Reply(id, good, "RESULT %s", good ? "desktop" : "stop_timeout");
        return;
    }
    if (!strcmp(op, "DESKTOP_STATUS")) {
        Reply(id, true, "DESKTOP_STATUS %s %lu %lu", running.load() ? "app" : "home", (unsigned long)catalog.count,
              (unsigned long)desktop_page);
        return;
    }
    if (!strcmp(op, "APP_IMPORT_SD")) {
        char* app = Arg();
        char* digest = Arg();
        char* path = Arg();
        bool good = ImportSd(app, digest, path);
        Reply(id, good, "RESULT %s", good ? "installed" : "sd_import_failed");
        return;
    }
    if (!strcmp(op, "SD_DIAG")) {
        if (installing || !Stop()) {
            Reply(id, false, "busy");
            return;
        }
        if (!sd_storage_mount()) {
            Reply(id, false, "mount_%u", (unsigned)sd_storage_last_result());
            return;
        }
        FILINFO info{};
        unsigned results[3];
        unsigned long sizes[3]{};
        const char* paths[] = {SdAppStorage::kPath, SdAppStorage::kPendingPath, "/sd/micropixel/apps.failed"};
        for (unsigned i = 0; i < 3; ++i) {
            results[i] = f_stat(paths[i], &info);
            if (results[i] == FR_OK) sizes[i] = info.fsize;
        }
        DWORD clusters{};
        FATFS* fs{};
        const auto free_result = f_getfree("/sd", &clusters, &fs);
        Reply(id, true, "SD_DIAG bfs=%u:%lu new=%u:%lu failed=%u:%lu free_result=%u free_bytes=%llu", results[0],
              sizes[0], results[1], sizes[1], results[2], sizes[2], (unsigned)free_result,
              free_result == FR_OK ? (unsigned long long)clusters * fs->csize * 512 : 0ULL);
        return;
    }
    if (!strcmp(op, "SD_STATUS")) {
        bool good = sd_storage_mount();
        Reply(id, true, "SD_STATUS %u %u", good, (unsigned)sd_storage_last_result());
        return;
    }
    if (!strcmp(op, "SD_APPSTORE_INIT")) {
        if (installing || !Stop()) {
            Reply(id, false, "busy");
            return;
        }
        bool good = AttachSdApps(true);
        Reply(id, good, "RESULT %s", good ? "sd_ready" : sd_attach_error);
        return;
    }
    if (!strcmp(op, "SD_APPSTORE_REFORMAT_PENDING")) {
        char* confirm = Arg();
        if (!confirm || strcmp(confirm, "ERASE") || installing || !Stop() || sd_bundlefs) {
            Reply(id, false, "confirmation_or_idle_required");
            return;
        }
        bool good = AttachSdApps(true, true);
        Reply(id, good, "RESULT %s", good ? "sd_ready" : sd_attach_error);
        return;
    }
    if (!strcmp(op, "SD_APPSTORE_ARCHIVE_PENDING")) {
        if (installing || !Stop() || sd_bundlefs) {
            Reply(id, false, "busy");
            return;
        }
        bool good = sd_storage_mount() && f_rename(SdAppStorage::kPendingPath, "/sd/micropixel/apps.failed") == FR_OK;
        Reply(id, good, "RESULT %s", good ? "pending_preserved" : "pending_archive_failed");
        return;
    }
    if (!strcmp(op, "SD_APPSTORE_EJECT")) {
        if (installing || !Stop()) {
            Reply(id, false, "busy");
            return;
        }
        bool good = DetachSdApps();
        Reply(id, good, "RESULT %s", good ? "sd_ejected" : "sd_eject_failed");
        return;
    }
    if (!strcmp(op, "SD_APPSTORE_STATUS")) {
        auto s = sd_apps.statistics();
        Reply(id, true, "SD_APPSTORE %u %s %llu %llu %llu %llu %llu %llu %lu %llu", store->split(),
              sd_bundlefs && !sd_apps.available() ? sd_apps.error() : sd_attach_error,
              (unsigned long long)catalog.system_storage.total_bytes,
              (unsigned long long)catalog.system_storage.used_bytes,
              (unsigned long long)catalog.external_storage.total_bytes,
              (unsigned long long)catalog.external_storage.used_bytes, (unsigned long long)s.read_bytes,
              (unsigned long long)s.read_us, (unsigned long)s.read_calls, (unsigned long long)s.write_bytes);
        return;
    }
    if (!strcmp(op, "APP_STORAGE")) {
        char* app = Arg();
        for (uint32_t i = 0; app && i < catalog.count; ++i)
            if (!strcmp(app, catalog.apps[i].app_id.data())) {
                Reply(id, true, "APP_STORAGE %s %s", app,
                      catalog.apps[i].storage == r::AppStorage::kExternal ? "sd" : "flash");
                return;
            }
        Reply(id, false, "app_not_found");
        return;
    }
    if (!strcmp(op, "APP_COPY_TO_SD")) {
        char* app = Arg();
        if (installing || !Stop()) {
            Reply(id, false, "busy");
            return;
        }
        bool good = CopyAppToSd(app);
        Reply(id, good, "RESULT %s", good ? "copied_to_sd" : "sd_copy_failed");
        return;
    }
    if (!strcmp(op, "HELLO")) {
        Reply(id, true, "HELLO 1 3072 %llu",
              (unsigned long long)(store->split() ? sd_apps.geometry().size_bytes : flash.geometry().size_bytes));
        return;
    }
    if (!strcmp(op, "DEVICE_STATUS")) {
        Reply(id, true,
              "DEVICE_STATUS Ym91ZmZhbG8tZGV2 " MPX_BOARD_MODEL_BASE64 " " MPX_BOARD_CHIP_BASE64
              " %llu %s %s %lu %llu %llu %u %u %u %u 0 0 0",
              (unsigned long long)bflb_mtimer_get_time_us() / 1000, running.load() ? selected.app_id.data() : "-",
              running.load() ? "running" : "stopped", (unsigned long)catalog.count,
              (unsigned long long)catalog.store_used_bytes, (unsigned long long)catalog.store_total_bytes,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), graphics.Width(), graphics.Height());
        return;
    }
    if (!strcmp(op, "APP_LIST")) {
        uint32_t offset{};
        if (!Number(Arg(), offset) || offset > catalog.count) {
            Reply(id, false, "invalid_offset");
            return;
        }
        if (offset == catalog.count) {
            Reply(id, true, "APP_LIST %lu %lu %llu %llu 0", (unsigned long)offset, (unsigned long)catalog.count,
                  (unsigned long long)catalog.store_used_bytes, (unsigned long long)catalog.store_total_bytes);
            return;
        }
        auto& a = catalog.apps[offset];
        char name[400], sha[65];
        Base64((const uint8_t*)a.display_name.data(), strlen(a.display_name.data()), name, sizeof(name));
        HexString(a.sha256, sha);
        bool active = running.load() && !strcmp(a.app_id.data(), selected.app_id.data());
        Reply(id, true, "APP_LIST %lu %lu %llu %llu 1 %s,%lu,%s,%u,%s,%s,%s", (unsigned long)offset + 1,
              (unsigned long)catalog.count, (unsigned long long)catalog.store_used_bytes,
              (unsigned long long)catalog.store_total_bytes, a.app_id.data(), (unsigned long)a.bundle_size, name,
              active, active ? "running" : "stopped", sha, a.version.data());
        return;
    }
    if (!strcmp(op, "INPUT_SEQUENCE")) {
        bool good = Sequence(Arg());
        Reply(id, good, "RESULT %s", good ? "input_delivered" : "invalid_input_sequence");
        return;
    }
    if (!strcmp(op, "APP_START")) {
        char* app = Arg();
        char* args = Arg();
        bool started = Start(app, args);
        Reply(id, started, "RESULT %s", started ? "started" : "start_failed");
        return;
    }
    if (!strcmp(op, "APP_STOP")) {
        Reply(id, Stop(), "RESULT stopped");
        return;
    }
    if (!strcmp(op, "APP_UNINSTALL")) {
        char* app = Arg();
        if (!app || installing || !Stop()) {
            Reply(id, false, "busy_or_invalid_app");
            return;
        }
        auto result = store->UninstallApp(app);
        Reload();
        Reply(id, result.has_value(), "RESULT %s", result ? "uninstalled" : "uninstall_failed");
        return;
    }
    if (!strcmp(op, "APP_LAST_ERROR")) {
        if (!failed.load()) {
            Reply(id, true, "APP_ERROR NONE");
            return;
        }
        char detail[400];
        Base64((const uint8_t*)last_outcome.detail.data(), strlen(last_outcome.detail.data()), detail, sizeof(detail));
        Reply(id, true, "APP_ERROR %s %s %s %u %ld %s", last_outcome.app_id.data(),
              r::AppSessionErrorPhase(last_outcome.error), r::AppSessionErrorCode(last_outcome.error),
              last_outcome.has_exit_code, (long)last_outcome.exit_code, detail);
        return;
    }
    if (!strcmp(op, "LOG_READ")) {
        Arg();
        uint32_t seq{};
        if (!Number(Arg(), seq)) {
            Reply(id, false, "invalid_cursor");
            return;
        }
        logs.Page(id, seq);
        return;
    }
    if (!strcmp(op, "APP_INSTALL_BEGIN")) {
        char* app = Arg();
        uint32_t size{};
        char* size_text = Arg();
        char* digest = Arg();
        std::array<uint8_t, 32> digest_bytes{};
        if (!app || strlen(app) > 64 || !Number(size_text, size) || !size || !Hex(digest, digest_bytes) || !Stop()) {
            Reply(id, false, "invalid_install_or_busy");
            return;
        }
        AbortInstall();
        snprintf(install_id, sizeof(install_id), "%s", app);
        install_size = size;
        install_sha = digest_bytes;
        auto result = store->BeginAppInstall(install_id, size);
        if (!result) {
            Reply(id, false, "install_begin_failed_%u", (unsigned)result.error());
            AbortInstall();
            return;
        }
        installing = true;
        install_activity = xTaskGetTickCount();
        Reply(id, true, "INSTALL_READY 3072");
        return;
    }
    if (!strcmp(op, "APP_INSTALL_CHUNK")) {
        uint32_t offset{};
        char* o = Arg();
        char* data = Arg();
        size_t bytes{};
        if (!installing || !Number(o, offset) || offset != install_received ||
            !Decode64(data, chunk, sizeof(chunk), bytes) || !bytes || bytes > 3072 ||
            bytes > install_size - install_received) {
            Reply(id, false, "invalid_install_chunk");
            return;
        }
        auto result = store->WriteAppInstall(offset, {chunk, bytes});
        if (!result) {
            AbortInstall();
            Reply(id, false, "install_write_failed");
            return;
        }
        install_received += bytes;
        install_activity = xTaskGetTickCount();
        Reply(id, true, "INSTALL_CHUNK %lu", (unsigned long)install_received);
        return;
    }
    if (!strcmp(op, "APP_INSTALL_ABORT")) {
        AbortInstall();
        Reply(id, true, "RESULT aborted");
        return;
    }
    if (!strcmp(op, "APP_INSTALL_COMMIT")) {
        if (!installing || install_received != install_size) {
            Reply(id, false, "incomplete_install");
            return;
        }
        r::AppInstallRequest request{};
        request.size = install_size;
        request.expected_app_id = install_id;
        request.expected_sha256 = install_sha;
        request.environment = &environment;
        auto result = store->Install(request);
        installing = false;
        Reload();
        if (result)
            Reply(id, true, "RESULT installed");
        else
            Reply(id, false, "install_commit_failed_%u", (unsigned)result.error());
        return;
    }
    if (!strcmp(op, "PSRAM_CHECK")) {
        if (installing || !Stop()) {
            Reply(id, false, "busy");
            return;
        }
        size_t tested{};
        bool good = micropixel_psram_check(&tested);
        Reply(id, good, "RESULT psram_%s_%lu", good ? "pass" : "fail", (unsigned long)tested);
        return;
    }
    if (!strcmp(op, "DEVICE_TASKS")) {
        Reply(id, true, "TASK_PAGE 0 0 0 0 0 0 0");
        return;
    }
    Reply(id, false, "unsupported_command_%s", op);
}
}  // namespace
extern "C" void micropixel_host_start(void*) {
    mpx_console_init();
    mpx_console_line("[micropixel] " MPX_BOARD_NAME " RV32 ilp32f / ABI 2.0");
    if (psa_crypto_init() != PSA_SUCCESS || !graphics.Initialize(mpx_port::DrawDesktopGuestOverlay)) {
        mpx_console_line("[micropixel] graphics/crypto init failed");
        vTaskDelete(nullptr);
    }
    graphics.ShowMessage("MicroPixel / " MPX_BOARD_NAME "");
    bflb_mtd_init();
    if (!InitializeKv() || !flash.Initialize()) {
        graphics.ShowMessage("MicroPixel: storage unavailable");
        mpx_console_line("[micropixel] storage init failed; existing media preserved");
        vTaskDelete(nullptr);
    }
    bundlefs.emplace(flash);
    auto mounted = bundlefs->Mount();
    if (mounted != BUNDLEFS_OK) {
        bool blank = true;
        uint8_t page[256];
        for (uint64_t o = 0; o < flash.geometry().size_bytes && blank; o += sizeof(page)) {
            if (!flash.Read(o, page)) {
                blank = false;
                break;
            }
            for (auto b : page)
                if (b != 255) {
                    blank = false;
                    break;
                }
        }
        if (!blank || bundlefs->Format() != BUNDLEFS_OK) {
            graphics.ShowMessage("MicroPixel: BundleFS mount failed");
            mpx_console_line("[micropixel] BundleFS mount failed; existing media preserved");
            vTaskDelete(nullptr);
        }
    }
    store.emplace(*bundlefs);
    if (!Reload()) {
        vTaskDelete(nullptr);
    }
    (void)AttachSdApps(MPX_SD_AUTO_CREATE);
    bool touch = input.Initialize();
    if (!touch) mpx_console_line("[micropixel] GT911 unavailable; serial input injection remains available");
    bool sound = audio_output.Initialize() == ESP_OK;
    if (sound) sound = audio.Initialize(audio_output, 48000) == ESP_OK;
    (void)registry.InitializeStorage();
    (void)registry.RegisterDisplay();
    if (touch) (void)registry.RegisterTouch();
    if (sound) (void)registry.RegisterAudioOutput();
    static d::DeviceServices devices(
        graphics, {MPX_PANEL_NAME, MPX_PANEL_BUS, "RGB565", graphics.Width(), graphics.Height(), 60}, input,
        sound ? static_cast<d::Audio&>(audio) : static_cast<d::Audio&>(unavailable_audio), random_source, registry,
        registry, registry, registry, battery);
    static micropixel::work::BackgroundExecutor background;
    auto initialized = r::AppRuntime::Initialize(devices, background, "en", &logs);
    if (!initialized) {
        mpx_console_line("[micropixel] runtime initialization failed");
        vTaskDelete(nullptr);
    }
    runtime.emplace(std::move(*initialized));
    environment.core_abi = MICROPIXEL_ABI_VERSION;
    micropixel_app_set_logical_display(&environment, graphics.Width(), graphics.Height());
    environment.capabilities = (touch ? 1 : 0) | 2 | (sound ? 4 : 0);
    for (auto& v : environment.services) v = MICROPIXEL_INTERFACE_VERSION(1, 0);
    environment.services[1] = MICROPIXEL_INTERFACE_VERSION(1, 1);
    desktop_queue = xQueueCreateStatic(8, sizeof(DesktopTap), desktop_queue_bytes, &desktop_queue_storage);
    input.BindHostTouch(DesktopTouch, nullptr);
    (void)xTaskCreate(InputTask, "mpx_touch", 2048, nullptr, 3, nullptr);
    mpx_console_line("[micropixel] READY: MPX1, UART 115200");
    RenderDesktop();
    mpx_console_line("[desktop] READY: resident app launcher; tap to start, house to stop");
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        DesktopProcess();
        if (installing && (TickType_t)(xTaskGetTickCount() - install_activity) > pdMS_TO_TICKS(60000)) AbortInstall();
        int c;
        while ((c = mpx_console_read()) >= 0) {
            if (c == '\n') {
                if (!overflow) {
                    line[used] = 0;
                    Command(line);
                }
                used = 0;
                overflow = false;
            } else if (c != '\r') {
                if (used + 1 < sizeof(line))
                    line[used++] = (char)c;
                else
                    overflow = true;
            }
        }
        vTaskDelay(1);
    }
}
