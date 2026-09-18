#pragma once

#include <algorithm>
#include <charconv>
#include <string_view>

#include "device/contracts/cellular.hpp"

namespace micropixel::platform::metalio_claw4::diagnostics {

inline std::string_view Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    value.remove_prefix(first);
    return value.substr(0, value.find_last_not_of(" \t") + 1);
}

inline std::string_view Line(std::string_view response, std::string_view prefix) {
    while (!response.empty()) {
        const auto end = response.find_first_of("\r\n");
        const auto line = Trim(response.substr(0, end));
        if (line.starts_with(prefix)) return Trim(line.substr(prefix.size()));
        if (end == std::string_view::npos) break;
        response.remove_prefix(end + 1);
    }
    return {};
}

inline std::string_view Field(std::string_view line, unsigned index) {
    // AT fields may contain quoted commas; never split inside a quoted field.
    for (unsigned field = 0; !line.empty(); ++field) {
        bool quoted = false;
        size_t end = 0;
        for (; end < line.size(); ++end) {
            if (line[end] == '"') quoted = !quoted;
            if (line[end] == ',' && !quoted) break;
        }
        if (quoted) return {};
        if (field == index) return Trim(line.substr(0, end));
        if (end == line.size()) break;
        line.remove_prefix(end + 1);
    }
    return {};
}

inline int8_t Number(std::string_view value, int maximum) {
    if (value.empty()) return -1;
    int number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && number >= 0 && number <= maximum
               ? static_cast<int8_t>(number)
               : -1;
}

template <size_t N>
bool Text(std::string_view field, std::array<char, N>& output) {
    if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
        field.remove_prefix(1);
        field.remove_suffix(1);
    } else {
        return false;
    }
    if (field.empty() || field.size() >= N) return false;
    for (size_t i = 0; i < field.size(); ++i) {
        if (static_cast<unsigned char>(field[i]) < 32 || field[i] == '"') return false;
    }
    std::copy(field.begin(), field.end(), output.begin());
    return true;
}

template <size_t N>
bool Identifier(std::string_view field, std::array<char, N>& output, std::string_view alphabet, size_t minimum = 1) {
    output.fill(0);
    if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
        field.remove_prefix(1);
        field.remove_suffix(1);
    }
    if (field.size() < minimum || field.size() >= N || field.find_first_not_of(alphabet) != std::string_view::npos)
        return false;
    std::copy(field.begin(), field.end(), output.begin());
    return true;
}

inline void Cell(std::string_view line, device::CellularTelemetry& result) {
    // Only registered query responses carry a usable serving-cell location.
    const auto registration = Number(Field(line, 1), 10);
    if (registration != 1 && registration != 5) return;
    if (!Identifier(Field(line, 2), result.tac, "0123456789abcdefABCDEF") ||
        !Identifier(Field(line, 3), result.cell_id, "0123456789abcdefABCDEF") ||
        (result.access_technology = Number(Field(line, 4), 127)) < 0) {
        result.tac.fill(0);
        result.cell_id.fill(0);
        result.access_technology = -1;
    }
}

inline void Plmn(std::string_view line, device::CellularTelemetry& result) {
    if (Number(Field(line, 1), 2) != 2) return;
    std::array<char, 7> value{};
    if (!Identifier(Field(line, 2), value, "0123456789", 5)) return;
    std::copy_n(value.begin(), 3, result.mcc.begin());
    std::copy(value.begin() + 3, value.end() - 1, result.mnc.begin());
}

inline device::CellularSimStatus SimStatus(std::string_view response) {
    const auto value = Line(response, "+CPIN:");
    using Status = device::CellularSimStatus;
    if (Line(response, "+CME ERROR:") == "10") return Status::kAbsent;
    if (value == "READY") return Status::kReady;
    if (value == "SIM PIN") return Status::kPinRequired;
    if (value == "SIM PUK") return Status::kPukRequired;
    return value.empty() ? Status::kUnknown : Status::kNotReady;
}

}  // namespace micropixel::platform::metalio_claw4::diagnostics
