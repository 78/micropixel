#include "apps/showcase/showcase_app.hpp"

int main() {
    return showcase::Run({.title = "COLOR LAB",
                          .subtitle = "Generate bright palettes with hardware random",
                          .log_name = "color-lab: ready",
                          .accent = micropixel::Color::Rgb(77U, 214U, 164U),
                          .mode = showcase::Mode::kColorLab});
}
