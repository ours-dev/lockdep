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
constexpr auto LOCK_METADATA_ONLY_ENABLED = ENABLED_FEATURE_LEVEL == FeatureLevel::MetadataOnly;

// Whether or not lock validation is globally enabled.
constexpr auto LOCK_VALIDATION_ENABLED = ENABLED_FEATURE_LEVEL == FeatureLevel::Validation;

// Whether or not lock metadata is available.
constexpr auto LOCK_METADATA_AVAILABLE = LOCK_VALIDATION_ENABLED || LOCK_METADATA_ONLY_ENABLED;

template <typename T, typename U>
using IfLockMetadataOnlyEnabled = std::conditional_t<LOCK_METADATA_ONLY_ENABLED, T, U>;

template <typename T, typename U>
using IfLockValidationEnabled = std::conditional_t<LOCK_VALIDATION_ENABLED, T, U>;

} // namespace lockdep
