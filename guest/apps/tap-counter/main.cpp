#include "apps/showcase/showcase_app.hpp"

int main() {
    return showcase::Run({.title = "TAP COUNTER",
                          .subtitle = "A tiny counter for quick input checks",
                          .log_name = "tap-counter: ready",
                          .accent = micropixel::Color::Rgb(255U, 159U, 67U),
                          .mode = showcase::Mode::kTapCounter});
}
