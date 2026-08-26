#include "device/text.hpp"

namespace micropixel::device {

bool IsValidUtf8(const uint8_t* text, uint32_t length) {
    if (text == nullptr || length == 0U) {
        return false;
    }
    uint32_t index = 0U;
    while (index < length) {
        const uint8_t first = text[index++];
        if (first == 0U) {
            return false;
        }
        if (first < 0x80U) {
            continue;
        }

        uint32_t remaining = 0U;
        uint32_t codepoint = 0U;
        uint32_t minimum = 0U;
        if ((first & 0xe0U) == 0xc0U) {
            remaining = 1U;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            remaining = 2U;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            remaining = 3U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (remaining > length - index) {
            return false;
        }
        for (uint32_t continuation = 0U; continuation < remaining; ++continuation) {
            const uint8_t next = text[index++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

}  // namespace micropixel::device
