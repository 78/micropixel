#include "sdk/micropixel.hpp"

int main() {
    micropixel::Assert(false, "application_assert: explicit invariant failure");
    return 0;
}
