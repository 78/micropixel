#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "sdk/micropixel.hpp"

namespace {

volatile uint32_t input_value = 7U;

struct alignas(64) AlignedValue final {
    uint32_t value;
};

}  // namespace

int main() {
    const uint32_t seed = input_value;
    std::array<uint32_t, 4U> fixed{seed + 3U, seed + 1U, seed + 4U, seed + 2U};
    std::span<uint32_t> view{fixed};
    std::sort(view.begin(), view.end());
    micropixel::Assert(view.front() == seed + 1U && view.back() == seed + 4U, "stl: sort/span failed");

    std::vector<uint32_t> values(view.begin(), view.end());
    values.push_back(seed + 5U);
    micropixel::Assert(values.size() == 5U && values.back() == seed + 5U, "stl: vector failed");

    std::map<uint32_t, std::string_view> labels;
    labels.emplace(seed, "first");
    labels.emplace(seed + 1U, "second");
    micropixel::Assert(labels.at(seed + 1U) == "second", "stl: map/string_view failed");

    std::queue<uint32_t> pending;
    pending.push(seed);
    pending.push(seed + 1U);
    pending.pop();
    micropixel::Assert(pending.front() == seed + 1U, "stl: queue failed");

    std::unique_ptr<uint32_t[]> owned = std::make_unique<uint32_t[]>(3U);
    owned[0] = seed;
    owned[1] = seed + 1U;
    owned[2] = seed + 2U;
    micropixel::Assert(owned[2] == seed + 2U, "stl: unique_ptr/new failed");

    uint32_t* direct = new uint32_t(seed + 6U);
    micropixel::Assert(*direct == seed + 6U, "stl: direct new failed");
    delete direct;
    std::unique_ptr<AlignedValue> aligned = std::make_unique<AlignedValue>(AlignedValue{seed + 7U});
    micropixel::Assert(reinterpret_cast<uintptr_t>(aligned.get()) % alignof(AlignedValue) == 0U,
                       "stl: aligned new failed");
    uint8_t* exhausted = new (std::nothrow) uint8_t[40U * 1024U];
    micropixel::Assert(exhausted == nullptr, "stl: nothrow OOM failed");

    std::string text = "MicroPixel STL ";
    text += "conformance uses dynamic storage";
    micropixel::Assert(text.size() > 23U, "stl: string failed");

    const std::optional<uint32_t> selected = values[seed % values.size()];
    std::variant<uint32_t, std::string_view> result{*selected};
    const uint32_t* number = std::get_if<uint32_t>(&result);
    micropixel::Assert(number != nullptr && *number >= seed + 1U, "stl: optional/variant failed");
    return 0;
}
