#include "apps/showcase/showcase_app.hpp"

int main() {
    return showcase::Run({.title = "PIXEL SKETCH",
                          .subtitle = "Paint a small grid by touch and drag",
                          .log_name = "pixel-sketch: ready",
                          .accent = micropixel::Color::Rgb(105U, 167U, 255U),
                          .mode = showcase::Mode::kPixelSketch});
}
