#ifndef MICROPIXEL_SDK_SYMBOLS_HPP
#define MICROPIXEL_SDK_SYMBOLS_HPP

// Stable UTF-8 symbols supported by every MicroPixel SystemFont role. Guest
// Apps use these constants as ordinary label/button text and never depend on
// LVGL headers, Font objects, or board-specific Host implementation details.
namespace micropixel::symbols {

inline constexpr char kAudio[] = "\xEF\x80\x81";        // U+F001
inline constexpr char kVideo[] = "\xEF\x80\x88";        // U+F008
inline constexpr char kList[] = "\xEF\x80\x8B";         // U+F00B
inline constexpr char kOk[] = "\xEF\x80\x8C";           // U+F00C
inline constexpr char kClose[] = "\xEF\x80\x8D";        // U+F00D
inline constexpr char kPower[] = "\xEF\x80\x91";        // U+F011
inline constexpr char kSettings[] = "\xEF\x80\x93";     // U+F013
inline constexpr char kHome[] = "\xEF\x80\x95";         // U+F015
inline constexpr char kDownload[] = "\xEF\x80\x99";     // U+F019
inline constexpr char kDrive[] = "\xEF\x80\x9C";        // U+F01C
inline constexpr char kRefresh[] = "\xEF\x80\xA1";      // U+F021
inline constexpr char kLock[] = "\xEF\x80\xA3";         // U+F023
inline constexpr char kMute[] = "\xEF\x80\xA6";         // U+F026
inline constexpr char kVolumeMid[] = "\xEF\x80\xA7";    // U+F027
inline constexpr char kVolumeMax[] = "\xEF\x80\xA8";    // U+F028
inline constexpr char kImage[] = "\xEF\x80\xBE";        // U+F03E
inline constexpr char kTint[] = "\xEF\x81\x83";         // U+F043
inline constexpr char kPrevious[] = "\xEF\x81\x88";     // U+F048
inline constexpr char kPlay[] = "\xEF\x81\x8B";         // U+F04B
inline constexpr char kPause[] = "\xEF\x81\x8C";        // U+F04C
inline constexpr char kStop[] = "\xEF\x81\x8D";         // U+F04D
inline constexpr char kNext[] = "\xEF\x81\x91";         // U+F051
inline constexpr char kEject[] = "\xEF\x81\x92";        // U+F052
inline constexpr char kLeft[] = "\xEF\x81\x93";         // U+F053
inline constexpr char kRight[] = "\xEF\x81\x94";        // U+F054
inline constexpr char kPlus[] = "\xEF\x81\xA7";         // U+F067
inline constexpr char kMinus[] = "\xEF\x81\xA8";        // U+F068
inline constexpr char kEyeOpen[] = "\xEF\x81\xAE";      // U+F06E
inline constexpr char kEyeClosed[] = "\xEF\x81\xB0";    // U+F070
inline constexpr char kWarning[] = "\xEF\x81\xB1";      // U+F071
inline constexpr char kShuffle[] = "\xEF\x81\xB4";      // U+F074
inline constexpr char kUp[] = "\xEF\x81\xB7";           // U+F077
inline constexpr char kDown[] = "\xEF\x81\xB8";         // U+F078
inline constexpr char kLoop[] = "\xEF\x81\xB9";         // U+F079
inline constexpr char kDirectory[] = "\xEF\x81\xBB";    // U+F07B
inline constexpr char kUpload[] = "\xEF\x82\x93";       // U+F093
inline constexpr char kCall[] = "\xEF\x82\x95";         // U+F095
inline constexpr char kCut[] = "\xEF\x83\x84";          // U+F0C4
inline constexpr char kCopy[] = "\xEF\x83\x85";         // U+F0C5
inline constexpr char kSave[] = "\xEF\x83\x87";         // U+F0C7
inline constexpr char kMenu[] = "\xEF\x83\x89";         // U+F0C9
inline constexpr char kEnvelope[] = "\xEF\x83\xA0";     // U+F0E0
inline constexpr char kCharge[] = "\xEF\x83\xA7";       // U+F0E7
inline constexpr char kPaste[] = "\xEF\x83\xAA";        // U+F0EA
inline constexpr char kBell[] = "\xEF\x83\xB3";         // U+F0F3
inline constexpr char kKeyboard[] = "\xEF\x84\x9C";     // U+F11C
inline constexpr char kLocation[] = "\xEF\x84\xA4";     // U+F124
inline constexpr char kFile[] = "\xEF\x85\x9B";         // U+F15B
inline constexpr char kWifi[] = "\xEF\x87\xAB";         // U+F1EB
inline constexpr char kBatteryFull[] = "\xEF\x89\x80";  // U+F240
inline constexpr char kBatteryHigh[] = "\xEF\x89\x81";  // U+F241
inline constexpr char kBatteryMid[] = "\xEF\x89\x82";   // U+F242
inline constexpr char kBatteryLow[] = "\xEF\x89\x83";   // U+F243
inline constexpr char kBatteryEmpty[] = "\xEF\x89\x84"; // U+F244
inline constexpr char kUsb[] = "\xEF\x8A\x87";          // U+F287
inline constexpr char kBluetooth[] = "\xEF\x8A\x93";    // U+F293
inline constexpr char kTrash[] = "\xEF\x8B\xAD";        // U+F2ED
inline constexpr char kEdit[] = "\xEF\x8C\x84";         // U+F304
inline constexpr char kBackspace[] = "\xEF\x95\x9A";    // U+F55A
inline constexpr char kSdCard[] = "\xEF\x9F\x82";       // U+F7C2
inline constexpr char kNewLine[] = "\xEF\xA2\xA2";      // U+F8A2

}  // namespace micropixel::symbols

#endif
