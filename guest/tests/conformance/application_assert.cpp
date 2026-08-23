#include "sdk/micropixel.hpp"

int main() {
    micropixel::AssertThat(false, "application_assert: explicit invariant failure");
    return 0;
}
