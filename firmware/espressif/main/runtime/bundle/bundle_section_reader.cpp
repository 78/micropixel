// SPDX-License-Identifier: Apache-2.0
#include "runtime/bundle/bundle_section_reader.hpp"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"

namespace micropixel::runtime {

BundleSectionReader::~BundleSectionReader() { heap_caps_free(buffer_); }

std::expected<void, SectionReadError> BundleSectionReader::Fail(SectionReadError error) {
    if (error_ == SectionReadError::kNone) error_ = error;
    return std::unexpected(error_);
}

std::expected<void, SectionReadError> BundleSectionReader::Open(const micropixel_bundle_source_t& source,
                                                                const micropixel_bundle_section_t& section,
                                                                std::span<const uint8_t> mapped_bundle) {
    if (size_ != 0U || error_ != SectionReadError::kNone || section.size == 0U) {
        return Fail(SectionReadError::kInvalidRange);
    }
    uint32_t source_size = 0U;
    if (!micropixel_bundle_source_size(&source, &source_size)) return Fail(SectionReadError::kIo);
    if (section.offset > source_size || section.size > source_size - section.offset) {
        return Fail(SectionReadError::kInvalidRange);
    }
    if (!mapped_bundle.empty()) {
        if (section.offset > mapped_bundle.size() || section.size > mapped_bundle.size() - section.offset) {
            return Fail(SectionReadError::kInvalidRange);
        }
        memory_ = mapped_bundle.data() + section.offset;
    } else {
        buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffer_ == nullptr) return Fail(SectionReadError::kNoMemory);
    }
    source_ = &source;
    source_offset_ = section.offset;
    size_ = section.size;
    expected_hash_ = section.hash;
    verify_ = true;
    return {};
}

std::expected<void, SectionReadError> BundleSectionReader::OpenMemory(std::span<const uint8_t> bytes) {
    if (size_ != 0U || error_ != SectionReadError::kNone || bytes.empty() || bytes.size() > UINT32_MAX) {
        return Fail(SectionReadError::kInvalidRange);
    }
    memory_ = bytes.data();
    size_ = static_cast<uint32_t>(bytes.size());
    return {};
}

std::expected<void, SectionReadError> BundleSectionReader::Fill() {
    if (error_ != SectionReadError::kNone) return std::unexpected(error_);
    if (size_ == 0U) return Fail(SectionReadError::kInvalidRange);
    if (memory_ != nullptr || position_ < buffered_end_ || position_ == size_) return {};
    const uint32_t count = std::min(kBufferSize, size_ - position_);
    if (!micropixel_bundle_source_read(source_, source_offset_ + position_, buffer_, count)) {
        return Fail(SectionReadError::kIo);
    }
    buffered_end_ = position_ + count;
    return {};
}

std::span<const uint8_t> BundleSectionReader::Available() const {
    if (memory_ != nullptr) return {memory_ + position_, size_ - position_};
    return {buffer_ + position_ % kBufferSize, buffered_end_ - position_};
}

void BundleSectionReader::Consume(std::span<const uint8_t> bytes) {
    if (verify_) {
        for (uint8_t byte : bytes) hash_ = (hash_ ^ byte) * 16777619U;
    }
    position_ += static_cast<uint32_t>(bytes.size());
}

std::expected<void, SectionReadError> BundleSectionReader::PeekPrefix(std::span<uint8_t> output) {
    if (position_ != 0U || output.size() > kBufferSize || output.size() > size_) {
        return Fail(SectionReadError::kInvalidRange);
    }
    if (auto result = Fill(); !result) return result;
    if (!output.empty()) std::memcpy(output.data(), Available().data(), output.size());
    return {};
}

std::expected<void, SectionReadError> BundleSectionReader::Read(std::span<uint8_t> output) {
    if (output.size() > size_ - position_) return Fail(SectionReadError::kInvalidRange);
    if (auto result = Fill(); !result) return result;
    while (!output.empty()) {
        if (auto result = Fill(); !result) return result;
        const auto bytes = Available().first(std::min(output.size(), Available().size()));
        std::memcpy(output.data(), bytes.data(), bytes.size());
        Consume(bytes);
        output = output.subspan(bytes.size());
    }
    return {};
}

std::expected<void, SectionReadError> BundleSectionReader::Finish() {
    if (auto result = Fill(); !result) return result;
    while (position_ < size_) {
        if (auto result = Fill(); !result) return result;
        Consume(Available());
    }
    if (verify_ && hash_ != expected_hash_) return Fail(SectionReadError::kHashMismatch);
    return {};
}

const char* BundleSectionReader::FailureDetail() const {
    switch (error_) {
        case SectionReadError::kNone:
            return "section read succeeded";
        case SectionReadError::kInvalidRange:
            return "section range invalid or truncated";
        case SectionReadError::kNoMemory:
            return "section read buffer allocation failed";
        case SectionReadError::kIo:
            return "section IO failed or file content changed";
        case SectionReadError::kHashMismatch:
            return "section hash mismatch";
    }
    return "section read failed";
}

}  // namespace micropixel::runtime
