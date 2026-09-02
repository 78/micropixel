#include <cstdlib>
#include <cstring>
#include <iostream>

#include "sdk/ui/image_button.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void UsesClipAsTheDefaultPolicy() {
    const micropixel::ui::ImageButtonProperties properties{};
    Check(properties.overflow == micropixel::ui::TextOverflow::kClip,
          "image buttons must clip text overflow unless strict rejection is requested");
}

void DescribesObjectStateWithoutAllocation() {
    const micropixel::ui::ImageButton button;
    const auto description = button.ToString();
    Check(std::strcmp(description.c_str(),
                      "ImageButton bounds=(x=0,y=0,w=0,h=0) measured=(w=0,h=0) overflow=clip "
                      "clipped=false text=[]") == 0,
          "ImageButton::ToString must include bounds, metrics, policy, clipping state and text");
}

void DescribesTheSpecificOverflowingButton() {
    const auto diagnostic = micropixel::ui::FormatTextOverflowDiagnostic(
        "ImageButton", "clipped", micropixel::Rect{12, 24, 80, 40}, micropixel::TextMetrics{96U, 44U, 35},
        "PLAY", micropixel::ui::TextOverflow::kClip, true);
    Check(std::strcmp(diagnostic.c_str(),
                      "ImageButton overflow: action=clipped bounds=(x=12,y=24,w=80,h=40) "
                      "measured=(w=96,h=44) overflow=clip clipped=true text=[PLAY]") == 0,
          "ImageButton overflow diagnostic must identify the specific control and measured content");
}

}  // namespace

int main() {
    UsesClipAsTheDefaultPolicy();
    DescribesObjectStateWithoutAllocation();
    DescribesTheSpecificOverflowingButton();
    std::cout << "guest image button tests passed: 3 cases\n";
    return 0;
}
