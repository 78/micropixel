#ifndef MICROPIXEL_SDK_RESULT_HPP
#define MICROPIXEL_SDK_RESULT_HPP

#include "sdk/error.hpp"

namespace micropixel {

// Freestanding counterpart of std::unexpected<Error>. Result<T> intentionally
// fixes the error type so Guest applications do not need the standard library.
class [[nodiscard]] Unexpected final {
   public:
    explicit constexpr Unexpected(Error error) : error_(error) {}

    [[nodiscard]] constexpr Error& error() & { return error_; }
    [[nodiscard]] constexpr const Error& error() const& { return error_; }
    [[nodiscard]] constexpr Error&& error() && { return static_cast<Error&&>(error_); }

   private:
    Error error_;
};

[[nodiscard]] constexpr Unexpected unexpected(Error error) { return Unexpected{error}; }

// Result<T> is the freestanding Guest subset of std::expected<T, Error>.
// Invalid value()/error() access traps because Guest exceptions are disabled.
template <typename T>
class [[nodiscard]] Result final {
   public:
    Result(T value) : storage_(ValueTag{}, static_cast<T&&>(value)), state_(State::kValue) {}

    Result(Unexpected unexpected)
        : storage_(ErrorTag{}, static_cast<Unexpected&&>(unexpected).error()), state_(State::kError) {}

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    Result(Result&& other) noexcept : storage_(MoveStorage(other)), state_(other.state_) {}

    Result& operator=(Result&&) = delete;

    ~Result() {
        if (has_value()) {
            storage_.value_.~T();
        } else {
            storage_.error_.~Error();
        }
    }

    [[nodiscard]] bool has_value() const noexcept { return state_ == State::kValue; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& operator*() & {
        RequireValue();
        return storage_.value_;
    }

    [[nodiscard]] const T& operator*() const& {
        RequireValue();
        return storage_.value_;
    }

    [[nodiscard]] T&& operator*() && {
        RequireValue();
        return static_cast<T&&>(storage_.value_);
    }

    [[nodiscard]] T* operator->() {
        RequireValue();
        return __builtin_addressof(storage_.value_);
    }

    [[nodiscard]] const T* operator->() const {
        RequireValue();
        return __builtin_addressof(storage_.value_);
    }

    [[nodiscard]] T& value() & { return **this; }
    [[nodiscard]] const T& value() const& { return **this; }
    [[nodiscard]] T&& value() && { return *static_cast<Result&&>(*this); }

    [[nodiscard]] Error& error() & {
        RequireError();
        return storage_.error_;
    }

    [[nodiscard]] const Error& error() const& {
        RequireError();
        return storage_.error_;
    }

    [[nodiscard]] Error&& error() && {
        RequireError();
        return static_cast<Error&&>(storage_.error_);
    }

    [[nodiscard]] T value_or(T fallback) const& {
        if (has_value()) {
            return storage_.value_;
        }
        return static_cast<T&&>(fallback);
    }

    [[nodiscard]] T value_or(T fallback) && {
        if (has_value()) {
            return static_cast<T&&>(storage_.value_);
        }
        return static_cast<T&&>(fallback);
    }

   private:
    enum class State : uint8_t {
        kValue,
        kError,
    };

    struct ValueTag {};
    struct ErrorTag {};

    union Storage {
        explicit Storage(ValueTag, T&& value) : value_(static_cast<T&&>(value)) {}

        explicit Storage(ErrorTag, Error error) : error_(error) {}

        ~Storage() {}

        T value_;
        Error error_;
    };

    void RequireValue() const {
        if (!has_value()) {
            __builtin_trap();
        }
    }

    void RequireError() const {
        if (has_value()) {
            __builtin_trap();
        }
    }

    static Storage MoveStorage(Result& other) {
        if (other.has_value()) {
            return Storage(ValueTag{}, static_cast<T&&>(other.storage_.value_));
        }
        return Storage(ErrorTag{}, static_cast<Error&&>(other.storage_.error_));
    }

    Storage storage_;
    State state_;
};

template <>
class [[nodiscard]] Result<void> final {
   public:
    constexpr Result() = default;
    constexpr Result(Unexpected unexpected)
        : error_(static_cast<Unexpected&&>(unexpected).error()), has_value_(false) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }

    constexpr void value() const {
        if (!has_value_) {
            __builtin_trap();
        }
    }

    [[nodiscard]] constexpr Error& error() & {
        RequireError();
        return error_;
    }

    [[nodiscard]] constexpr const Error& error() const& {
        RequireError();
        return error_;
    }

    [[nodiscard]] constexpr Error&& error() && {
        RequireError();
        return static_cast<Error&&>(error_);
    }

   private:
    constexpr void RequireError() const {
        if (has_value_) {
            __builtin_trap();
        }
    }

    Error error_{ErrorCode::kInternal};
    bool has_value_{true};
};

}  // namespace micropixel

#endif
