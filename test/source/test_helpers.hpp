#pragma once

namespace test_helpers
{

inline constexpr bool is_sanitized_build = []() -> bool
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
    return true;
#else
#ifdef __has_feature
#if __has_feature(address_sanitizer)
    return true;
#elif __has_feature(undefined_behavior_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
#endif
}();

}  // namespace test_helpers
