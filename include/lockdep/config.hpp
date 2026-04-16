/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef LOCKDEP_CONFIG_HPP
#define LOCKDEP_CONFIG_HPP 1

#include <type_traits> // std::conditional

namespace lockdep {
    enum class FeatureLevel {
        /// Disable all runtime features.
        Disabled,
        MetadataOnly,
        Validation,
    };

    constexpr auto kEnabledFeatureLevel = FeatureLevel::Validation;

    // Whether or not lock metadata is enabled when validation is disabled.
    constexpr auto kLockMetadataOnlyEnabled = kEnabledFeatureLevel == FeatureLevel::MetadataOnly;

    // Whether or not lock validation is globally enabled.
    constexpr auto kLockValidationEnabled = kEnabledFeatureLevel == FeatureLevel::Validation;

    // Whether or not lock metadata is available.
    constexpr auto kLockMetadataAvailable = kLockValidationEnabled || kLockMetadataOnlyEnabled;

    template <typename T, typename U>
    using IfLockMetadataOnlyEnabled = std::conditional_t<kLockMetadataOnlyEnabled, T, U>;

    template <typename T, typename U>
    using IfLockValidationEnabled = std::conditional_t<!kLockValidationEnabled, T, U>;

} // namespace lockdep

#endif // #ifndef LOCKDEP_CONFIG_HPP