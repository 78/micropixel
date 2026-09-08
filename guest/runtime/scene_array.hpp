#ifndef MICROPIXEL_RUNTIME_SCENE_ARRAY_HPP
#define MICROPIXEL_RUNTIME_SCENE_ARRAY_HPP

#include <stddef.h>

#include <memory>
#include <new>
#include <type_traits>

namespace micropixel::runtime {

// Plain scene records only. Growth is explicit and fallible at resource creation;
// mutations within the reserved working set never allocate.
template <typename T>
class SceneArray final {
    static_assert(std::is_trivially_copyable_v<T>);

   public:
    [[nodiscard]] bool Reserve(size_t requested) {
        if (requested <= capacity_) return true;
        if (requested > static_cast<size_t>(-1) / sizeof(T) / 2) return false;
        size_t capacity = capacity_ == 0 ? 4 : capacity_;
        while (capacity < requested) capacity *= 2;
        auto replacement = std::unique_ptr<T[]>(new (std::nothrow) T[capacity]{});
        if (!replacement) return false;
        for (size_t i = 0; i < size_; ++i) replacement[i] = data_[i];
        data_ = static_cast<std::unique_ptr<T[]>&&>(replacement);
        capacity_ = capacity;
        return true;
    }
    void resize(size_t count, T value = {}) {
        if (count > capacity_) __builtin_trap();
        for (size_t i = size_; i < count; ++i) data_[i] = value;
        size_ = count;
    }
    void assign(size_t count, T value) {
        resize(count);
        for (size_t i = 0; i < count; ++i) data_[i] = value;
    }
    void push_back(T value) {
        const auto index = size_;
        resize(size_ + 1);
        data_[index] = value;
    }
    void emplace_back() { push_back({}); }
    void clear() { size_ = 0; }
    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] T* data() { return data_.get(); }
    [[nodiscard]] const T* data() const { return data_.get(); }
    T& operator[](size_t index) { return data_[index]; }
    const T& operator[](size_t index) const { return data_[index]; }

   private:
    std::unique_ptr<T[]> data_;
    size_t size_{}, capacity_{};
};
}  // namespace micropixel::runtime
#endif
