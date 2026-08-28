#include <cstdlib>
#include <cstring>
#include <iostream>

#include "platform/transports/ascii_line_framer.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    micropixel::platform::transports::AsciiLineFramer framer;
    char storage[16]{};
    framer.Bind(storage, sizeof(storage));
    uint32_t lines = 0U;
    char latest[16]{};
    const auto receive = [&](const char* line) {
        ++lines;
        std::strncpy(latest, line, sizeof(latest) - 1U);
    };

    const uint8_t first[] = {'M', 'P', 'X'};
    const uint8_t second[] = {'1', ' ', '1', '\r', '\n'};
    framer.Consume(first, sizeof(first), receive);
    framer.Consume(second, sizeof(second), receive);
    Check(lines == 1U && std::strcmp(latest, "MPX1 1") == 0, "fragmented CRLF line must be reconstructed");

    const uint8_t overlong[] = "0123456789abcdef trailing\nOK\n";
    framer.Consume(overlong, sizeof(overlong) - 1U, receive);
    Check(lines == 2U && std::strcmp(latest, "OK") == 0,
          "overlong input must be discarded through newline and then resynchronize");

    const uint8_t invalid[] = {'B', 'A', 'D', 0x01U, 'X', '\n', 'N', 'E', 'X', 'T', '\n'};
    framer.Consume(invalid, sizeof(invalid), receive);
    Check(lines == 3U && std::strcmp(latest, "NEXT") == 0,
          "non-printable input must discard the complete affected line");

    std::cout << "ASCII line framer tests passed\n";
    return 0;
}
