#pragma once

#include <type_traits>

namespace lockdep {
    enum class FeatureLevel {
        /// Disable all runtime features.
        Disabled,
        MetadataOnly,
        Validation,
    };

    constexpr auto ENABLED_FEATURE_LEVEL = FeatureLevel::Validation;

    // Whether or not lock metadata is enabled when validation is disabled.
    constexpr auto LockMetadataOnlyEnabled = ENABLED_FEATURE_LEVEL == FeatureLevel::MetadataOnly;

    // Whether or not lock validation is globally enabled.
    constexpr auto kLockValidationEnabled = ENABLED_FEATURE_LEVEL == FeatureLevel::Validation;

    // Whether or not lock metadata is available.
    constexpr auto kLockMetadataAvailable = kLockValidationEnabled || LockMetadataOnlyEnabled;

    template <typename T, typename U>
    using IfLockMetadataOnlyEnabled = std::conditional_t<LockMetadataOnlyEnabled, T, U>;

    template <typename T, typename U>
    using IfLockValidationEnabled = std::conditional_t<kLockValidationEnabled, T, U>;

} // namespace lockdep
