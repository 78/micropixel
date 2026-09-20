// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_SECTION_READER_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_SECTION_READER_HPP

#include <cstdint>
#include <expected>
#include <span>

#include "runtime/bundle/bundle_format.h"
#include "runtime/bundle/bundle_source.h"

namespace micropixel::runtime {

enum class SectionReadError : uint8_t { kNone, kInvalidRange, kNoMemory, kIo, kHashMismatch };

// Sequential, bounded section input. Source and optional whole-Bundle mapping are
// borrowed until destruction. Finish() verifies every section byte, including any
// bytes the decoder did not consume; decoded output must remain private until then.
// A source must return the same immutable content or fail after replacement/removal.
class BundleSectionReader final {
   public:
    BundleSectionReader() = default;
    BundleSectionReader(const BundleSectionReader&) = delete;
    BundleSectionReader& operator=(const BundleSectionReader&) = delete;
    ~BundleSectionReader();

    [[nodiscard]] std::expected<void, SectionReadError> Open(const micropixel_bundle_source_t& source,
                                                             const micropixel_bundle_section_t& section,
                                                             std::span<const uint8_t> mapped_bundle = {});
    // Adapter for already-addressable bytes whose verification belongs to the caller.
    [[nodiscard]] std::expected<void, SectionReadError> OpenMemory(std::span<const uint8_t> bytes);
    // Header inspection only: does not consume or hash bytes and is limited to 4 KiB.
    [[nodiscard]] std::expected<void, SectionReadError> PeekPrefix(std::span<uint8_t> output);
    [[nodiscard]] std::expected<void, SectionReadError> Read(std::span<uint8_t> output);
    [[nodiscard]] std::expected<void, SectionReadError> Finish();
    [[nodiscard]] uint32_t size() const { return size_; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const char* FailureDetail() const;

   private:
    [[nodiscard]] std::expected<void, SectionReadError> Fail(SectionReadError error);
    [[nodiscard]] std::expected<void, SectionReadError> Fill();
    [[nodiscard]] std::span<const uint8_t> Available() const;
    void Consume(std::span<const uint8_t> bytes);

    static constexpr uint32_t kBufferSize = 4096U;
    const micropixel_bundle_source_t* source_{};
    const uint8_t* memory_{};
    uint8_t* buffer_{};
    uint32_t source_offset_{};
    uint32_t size_{};
    uint32_t position_{};
    uint32_t buffered_end_{};
    uint32_t hash_{2166136261U};
    uint32_t expected_hash_{};
    bool verify_{};
    SectionReadError error_{SectionReadError::kNone};
};

}  // namespace micropixel::runtime
#endif
