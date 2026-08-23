#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;

int main() {
    micropixel::Application app;
    app.Run(app.After(20_ms, [&] { app.log().Info("schedule_after: fired"); }));
}
