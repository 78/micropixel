#include "apps/showcase/showcase_app.hpp"

int main() {
    return showcase::Run({.title = "ORBIT PAD",
                          .subtitle = "Move a marker through a simple touch field",
                          .log_name = "orbit-pad: ready",
                          .accent = micropixel::Color::Rgb(197U, 243U, 109U),
                          .mode = showcase::Mode::kOrbitPad});
}
