#pragma once

#include <cstddef>
#include <cstdint>

namespace micropixel::platform::graphics {

struct DamageRect final {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

struct DamageRegion final {
    const void* source{};
    DamageRect rect{};
};

struct DamageMergePolicy final {
    uint64_t max_extra_pixels{};
    uint64_t max_region_pixels{};
};

// Fixed-capacity damage accumulator for real-time graphics paths. Regions from
// different sources are never combined because their coordinates are local to
// different pixel stores. Nearby regions use the configured overdraw budget;
// when the array is full, the least-expensive same-source pair is combined so
// correctness does not depend on a Guest issuing fewer updates.
template <size_t Capacity>
class DamageRegionSet final {
    static_assert(Capacity > 0U);

   public:
    [[nodiscard]] bool Add(const void* source, DamageRect rect, DamageMergePolicy policy) {
        if (source == nullptr || !Valid(rect)) {
            return false;
        }

        DamageRegion incoming{source, rect};
        MergeEligibleRegions(incoming, policy);
        if (count_ < Capacity) {
            regions_[count_++] = incoming;
            return true;
        }
        return MergeAtCapacity(incoming, policy);
    }

    void Clear() {
        count_ = 0U;
        capacity_merge_count_ = 0U;
    }

    [[nodiscard]] size_t Size() const { return count_; }
    [[nodiscard]] bool Empty() const { return count_ == 0U; }
    [[nodiscard]] size_t CapacityValue() const { return Capacity; }
    [[nodiscard]] uint32_t CapacityMergeCount() const { return capacity_merge_count_; }

    [[nodiscard]] const DamageRegion& operator[](size_t index) const { return regions_[index]; }

   private:
    struct UnionCost final {
        DamageRect rect{};
        uint64_t extra_pixels{};
        uint64_t region_pixels{};
        bool valid{};
    };

    [[nodiscard]] static bool Valid(const DamageRect& rect) {
        return rect.width != 0U && rect.height != 0U &&
               static_cast<uint64_t>(rect.x) + rect.width <= static_cast<uint64_t>(UINT32_MAX) + 1U &&
               static_cast<uint64_t>(rect.y) + rect.height <= static_cast<uint64_t>(UINT32_MAX) + 1U;
    }

    [[nodiscard]] static uint64_t Pixels(const DamageRect& rect) {
        return static_cast<uint64_t>(rect.width) * rect.height;
    }

    [[nodiscard]] static UnionCost Union(const DamageRect& left, const DamageRect& right) {
        const uint64_t left_right = static_cast<uint64_t>(left.x) + left.width;
        const uint64_t left_bottom = static_cast<uint64_t>(left.y) + left.height;
        const uint64_t right_right = static_cast<uint64_t>(right.x) + right.width;
        const uint64_t right_bottom = static_cast<uint64_t>(right.y) + right.height;
        const uint64_t union_left = left.x < right.x ? left.x : right.x;
        const uint64_t union_top = left.y < right.y ? left.y : right.y;
        const uint64_t union_right = left_right > right_right ? left_right : right_right;
        const uint64_t union_bottom = left_bottom > right_bottom ? left_bottom : right_bottom;
        const uint64_t union_width = union_right - union_left;
        const uint64_t union_height = union_bottom - union_top;
        if (union_left > UINT32_MAX || union_top > UINT32_MAX || union_width > UINT32_MAX ||
            union_height > UINT32_MAX) {
            return {};
        }
        const uint64_t region_pixels = union_width * union_height;
        const uint64_t source_pixels = Pixels(left) + Pixels(right);
        return {
            .rect =
                {
                    .x = static_cast<uint32_t>(union_left),
                    .y = static_cast<uint32_t>(union_top),
                    .width = static_cast<uint32_t>(union_width),
                    .height = static_cast<uint32_t>(union_height),
                },
            .extra_pixels = region_pixels > source_pixels ? region_pixels - source_pixels : 0U,
            .region_pixels = region_pixels,
            .valid = true,
        };
    }

    [[nodiscard]] static bool Overlaps(const DamageRect& left, const DamageRect& right) {
        return static_cast<uint64_t>(left.x) < static_cast<uint64_t>(right.x) + right.width &&
               static_cast<uint64_t>(right.x) < static_cast<uint64_t>(left.x) + left.width &&
               static_cast<uint64_t>(left.y) < static_cast<uint64_t>(right.y) + right.height &&
               static_cast<uint64_t>(right.y) < static_cast<uint64_t>(left.y) + left.height;
    }

    [[nodiscard]] static bool Eligible(const DamageRect& left, const DamageRect& right, const UnionCost& cost,
                                       DamageMergePolicy policy) {
        // Regions of one set never overlap: renderers rely on that to visit
        // operations once per pixel (translucent operations would otherwise
        // blend twice). Overlapping pairs therefore always collapse, as do
        // exactly adjacent ones; only speculative overdraw obeys the policy.
        return cost.valid &&
               (Overlaps(left, right) || (cost.extra_pixels <= policy.max_extra_pixels &&
                                          (cost.extra_pixels == 0U || cost.region_pixels <= policy.max_region_pixels)));
    }

    void Remove(size_t index) {
        regions_[index] = regions_[count_ - 1U];
        --count_;
    }

    void MergeEligibleRegions(DamageRegion& incoming, DamageMergePolicy policy) {
        bool merged = true;
        while (merged) {
            merged = false;
            for (size_t index = 0U; index < count_; ++index) {
                if (regions_[index].source != incoming.source) {
                    continue;
                }
                const UnionCost cost = Union(regions_[index].rect, incoming.rect);
                if (!Eligible(regions_[index].rect, incoming.rect, cost, policy)) {
                    continue;
                }
                incoming.rect = cost.rect;
                Remove(index);
                merged = true;
                break;
            }
        }
    }

    [[nodiscard]] bool MergeAtCapacity(const DamageRegion& incoming, DamageMergePolicy policy) {
        size_t best_first = Capacity;
        size_t best_second = Capacity;
        bool best_includes_incoming = false;
        UnionCost best{};
        for (size_t index = 0U; index < count_; ++index) {
            if (regions_[index].source != incoming.source) {
                continue;
            }
            const UnionCost cost = Union(regions_[index].rect, incoming.rect);
            if (!cost.valid) {
                continue;
            }
            if (best_first == Capacity || cost.extra_pixels < best.extra_pixels ||
                (cost.extra_pixels == best.extra_pixels && cost.region_pixels < best.region_pixels)) {
                best_first = index;
                best_second = Capacity;
                best_includes_incoming = true;
                best = cost;
            }
        }
        for (size_t first = 0U; first < count_; ++first) {
            for (size_t second = first + 1U; second < count_; ++second) {
                if (regions_[first].source != regions_[second].source) {
                    continue;
                }
                const UnionCost cost = Union(regions_[first].rect, regions_[second].rect);
                if (!cost.valid) {
                    continue;
                }
                if (best_first == Capacity || cost.extra_pixels < best.extra_pixels ||
                    (cost.extra_pixels == best.extra_pixels && cost.region_pixels < best.region_pixels)) {
                    best_first = first;
                    best_second = second;
                    best_includes_incoming = false;
                    best = cost;
                }
            }
        }
        if (best_first == Capacity) {
            return false;
        }
        regions_[best_first].rect = best.rect;
        if (!best_includes_incoming) {
            regions_[best_second] = incoming;
        }
        ++capacity_merge_count_;
        // The union may now overlap regions it did not touch before; fold
        // those in so the no-overlap invariant holds after a capacity merge.
        bool overlaps_other = false;
        for (size_t index = 0U; index < count_; ++index) {
            overlaps_other = overlaps_other || (index != best_first && regions_[index].source == incoming.source &&
                                                Overlaps(regions_[index].rect, regions_[best_first].rect));
        }
        if (overlaps_other) {
            DamageRegion merged = regions_[best_first];
            Remove(best_first);
            MergeEligibleRegions(merged, policy);
            regions_[count_++] = merged;
        }
        return true;
    }

    DamageRegion regions_[Capacity]{};
    size_t count_{};
    uint32_t capacity_merge_count_{};
};

}  // namespace micropixel::platform::graphics
