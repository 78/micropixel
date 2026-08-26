#ifndef MICROPIXEL_SDK_PANIC_HPP
#define MICROPIXEL_SDK_PANIC_HPP

namespace micropixel {

// Terminates the current Guest After writing the supplied reason to the Host
// log. Use this for application invariants that cannot be recovered locally.
[[noreturn]] void Panic(const char* reason);

// Checks an application invariant and panics with a useful diagnostic when it
// is false. The capitalized name does not collide with the C assert macro.
inline void Assert(bool condition, const char* reason) {
    if (!condition) [[unlikely]] {
        Panic(reason);
    }
}

}  // namespace micropixel

#endif
