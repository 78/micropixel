#include <cstdlib>
#include <cstring>
#include <iostream>

#include "sdk/ui/flex_container.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void DescribesEmptyFlexContainer() {
    const micropixel::ui::FlexContainer container;
    const auto description = container.ToString();
    Check(std::strcmp(description.c_str(),
                      "FlexContainer bounds=(x=0,y=0,w=0,h=0) direction=vertical children=0 labels=0 "
                      "grids=0 image_buttons=0 text_buttons=0") == 0,
          "FlexContainer::ToString must summarize bounds, direction and typed child counts");
}

void DescribesEmptyGridContainer() {
    const micropixel::ui::GridContainer container;
    const auto description = container.ToString();
    Check(std::strcmp(description.c_str(),
                      "GridContainer bounds=(x=0,y=0,w=0,h=0) rows=0 inferred_rows=0 columns=0 cells=0 "
                      "row_gap=0 column_gap=0") == 0,
          "GridContainer::ToString must summarize bounds, tracks, cells and gaps");
}

}  // namespace

int main() {
    DescribesEmptyFlexContainer();
    DescribesEmptyGridContainer();
    std::cout << "guest UI description tests passed: 2 cases\n";
    return 0;
}
