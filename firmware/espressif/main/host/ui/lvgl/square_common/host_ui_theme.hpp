#pragma once

#include <array>
#include <cstdint>

struct _lv_display_t;
using lv_display_t = _lv_display_t;  // NOLINT(readability-identifier-naming)

namespace micropixel::host_ui::lvgl::square_common::theme {

// Host UI colors are centralized here so the Hall, loading surfaces, status
// layer, system menus, dialogs, and shared controls stay visually coherent.
// Add future themes as complete palettes. Install() registers the palette as
// the display's LVGL theme; SetModeLocked() changes it at runtime.
struct ThemePalette final {
    uint32_t canvas{};
    uint32_t surface{};
    uint32_t elevated_surface{};
    uint32_t pressed_surface{};
    uint32_t border{};
    uint32_t card_border{};
    uint32_t strong_border{};
    uint32_t primary_text{};
    uint32_t overlay_text{};
    uint32_t secondary_text{};
    uint32_t muted_text{};
    uint32_t accent{};
    uint32_t strong_accent{};
    uint32_t control_accent{};
    uint32_t success{};
    uint32_t warning{};
    uint32_t danger{};
    uint32_t orange{};
};

inline constexpr ThemePalette kDeepBlueTheme{
    .canvas = 0x08111fU,
    .surface = 0x111f32U,
    .elevated_surface = 0x182d48U,
    .pressed_surface = 0x24364eU,
    .border = 0x2e4562U,
    .card_border = 0x42607fU,
    .strong_border = 0x42607fU,
    .primary_text = 0xf2f7ffU,
    .overlay_text = 0xf2f7ffU,
    .secondary_text = 0x91a4bdU,
    .muted_text = 0x748aa5U,
    .accent = 0x69a7ffU,
    .strong_accent = 0x287ee8U,
    .control_accent = 0x69a7ffU,
    .success = 0x4dd6a4U,
    .warning = 0xf4c86aU,
    .danger = 0xff6b74U,
    .orange = 0xff9f43U,
};

// OLED-first palette: large areas stay true black, surfaces use neutral
// charcoal, and controls use the Control Console's brand green.
inline constexpr ThemePalette kPureBlackTheme{
    .canvas = 0x000000U,
    .surface = 0x111214U,
    .elevated_surface = 0x191b1fU,
    .pressed_surface = 0x24272cU,
    .border = 0x34383fU,
    .card_border = 0x565c66U,
    .strong_border = 0x4b5058U,
    .primary_text = 0xf4f5f2U,
    .overlay_text = 0xf4f5f2U,
    .secondary_text = 0x969a9fU,
    .muted_text = 0x73777eU,
    .accent = 0x72d25bU,
    .strong_accent = 0x285d31U,
    .control_accent = 0x72d25bU,
    .success = 0x56d7a5U,
    .warning = 0xf4c76bU,
    .danger = 0xff6b74U,
    .orange = 0xff9f43U,
};

// Daylight palette: warm ivory replaces stark white on large surfaces while
// cards remain white and controls use the Control Console's brand green.
inline constexpr ThemePalette kSoftIvoryTheme{
    .canvas = 0xf4f1eaU,
    .surface = 0xffffffU,
    .elevated_surface = 0xeae6deU,
    .pressed_surface = 0xddd8ceU,
    .border = 0xd0cbc1U,
    .card_border = 0xafa89cU,
    .strong_border = 0x938b7dU,
    .primary_text = 0x1d2024U,
    .overlay_text = 0xffffffU,
    .secondary_text = 0x60656cU,
    .muted_text = 0x858a90U,
    .accent = 0x285d31U,
    .strong_accent = 0x285d31U,
    .control_accent = 0x72d25bU,
    .success = 0x208a63U,
    .warning = 0xb7791fU,
    .danger = 0xd0444dU,
    .orange = 0xc96f24U,
};

enum class Mode : uint8_t {
    kPureBlack,
    kDeepBlue,
    kSoftIvory,
};

inline ThemePalette kActiveTheme = kPureBlackTheme;

// Canvases.
inline constexpr uint32_t kBlack = 0x000000U;
inline uint32_t& kHallBackground = kActiveTheme.canvas;
inline uint32_t& kMenuBackground = kActiveTheme.canvas;
inline uint32_t& kLoadingBackground = kActiveTheme.canvas;

// Surfaces.
inline uint32_t& kPressedBackground = kActiveTheme.pressed_surface;
inline uint32_t& kActionPressedBackground = kActiveTheme.pressed_surface;
inline uint32_t& kNavigationBackground = kActiveTheme.elevated_surface;
inline uint32_t& kPanelBackground = kActiveTheme.surface;
inline uint32_t& kUpdateBackground = kActiveTheme.surface;
inline uint32_t& kActionBackground = kActiveTheme.elevated_surface;
inline uint32_t& kCoverBackground = kActiveTheme.elevated_surface;

// Borders, dividers, and tracks.
inline uint32_t& kDivider = kActiveTheme.border;
inline uint32_t& kDisabledIndicator = kActiveTheme.border;
inline uint32_t& kBorder = kActiveTheme.border;
inline uint32_t& kControlTrack = kActiveTheme.border;
inline uint32_t& kActionBorder = kActiveTheme.border;
inline uint32_t& kStrongBorder = kActiveTheme.strong_border;

// Hall card surfaces.
inline uint32_t& kHallCardBackground = kActiveTheme.surface;
inline uint32_t& kHallCardBorder = kActiveTheme.card_border;
inline uint32_t& kHallCoverBackground = kActiveTheme.elevated_surface;
inline uint32_t& kHallCardControlBackground = kActiveTheme.canvas;

// Text.
inline uint32_t& kPrimaryText = kActiveTheme.primary_text;
inline uint32_t& kOverlayText = kActiveTheme.overlay_text;
inline uint32_t& kSecondaryText = kActiveTheme.secondary_text;
inline uint32_t& kMutedText = kActiveTheme.muted_text;
inline uint32_t& kDisabledText = kActiveTheme.muted_text;
inline uint32_t& kDetailText = kActiveTheme.secondary_text;
inline uint32_t& kLoadingText = kActiveTheme.primary_text;
inline uint32_t& kKeyboardDot = kActiveTheme.secondary_text;

// Accent and semantic states.
inline uint32_t& kAccentStrong = kActiveTheme.strong_accent;
inline uint32_t& kAccent = kActiveTheme.accent;
inline uint32_t& kControlAccent = kActiveTheme.control_accent;
inline uint32_t& kStatusControl = kActiveTheme.control_accent;
inline uint32_t& kBrandAccent = kActiveTheme.control_accent;
inline uint32_t& kSuccess = kActiveTheme.success;
inline uint32_t& kPositive = kActiveTheme.success;
inline uint32_t& kWarning = kActiveTheme.warning;
inline uint32_t& kDanger = kActiveTheme.danger;
inline uint32_t& kDangerSoft = kActiveTheme.danger;
inline uint32_t& kStop = kActiveTheme.danger;
inline uint32_t& kNotification = kActiveTheme.danger;
inline uint32_t& kOrange = kActiveTheme.orange;

// Hall-specific states.
inline uint32_t& kHallError = kActiveTheme.danger;
inline uint32_t& kHallErrorDetail = kActiveTheme.danger;
inline uint32_t& kUpdateBorder = kActiveTheme.success;
inline uint32_t& kUpdateText = kActiveTheme.success;
inline std::array<uint32_t, 3> kHallCoverColors{kOrange, kSuccess, kControlAccent};

// Status layer and modal surfaces.
inline constexpr uint32_t kStatusScrim = kBlack;
inline constexpr uint32_t kModalScrim = kBlack;
inline uint32_t& kStatusLayerBackground = kActiveTheme.canvas;
inline uint32_t& kPerformanceOverlayBackground = kActiveTheme.surface;
inline uint32_t& kStatusDialogBackground = kActiveTheme.surface;
inline uint32_t& kStatusPanelBackground = kActiveTheme.elevated_surface;
inline uint32_t& kDisabledControlBackground = kActiveTheme.elevated_surface;
inline uint32_t& kInactiveControlBackground = kActiveTheme.pressed_surface;
inline uint32_t& kStatusDialogBorder = kActiveTheme.strong_border;
inline uint32_t& kDangerBorder = kActiveTheme.danger;

// Wi-Fi status details.
inline uint32_t& kWifiError = kActiveTheme.danger;
inline uint32_t& kWifiWarning = kActiveTheme.warning;

[[nodiscard]] const ThemePalette& PaletteFor(Mode mode);
[[nodiscard]] const ThemePalette& ActivePalette();
[[nodiscard]] Mode ActiveMode();

// Must be called from the LVGL task, or while the platform's LVGL lock is held.
[[nodiscard]] bool Install(lv_display_t* display, Mode mode = Mode::kPureBlack);
[[nodiscard]] bool SetModeLocked(Mode mode);

}  // namespace micropixel::host_ui::lvgl::square_common::theme
