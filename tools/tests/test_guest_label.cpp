#include <cstdlib>
#include <cstring>
#include <iostream>

#include "sdk/ui/label.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void DescribesLayoutStateWithoutAllocation() {
    const micropixel::ui::Label label;
    const auto description = label.ToString();
    Check(std::strcmp(description.c_str(),
                      "Label bounds=(x=0,y=0,w=0,h=0) measured=(w=0,h=0) horizontal=center "
                      "vertical=center text=[]") == 0,
          "Label::ToString must include bounds, metrics, alignment and text");
}

}  // namespace

int main() {
    DescribesLayoutStateWithoutAllocation();
    std::cout << "guest label tests passed: 1 case\n";
    return 0;
}
