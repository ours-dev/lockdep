/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_CONFIG_HPP
#define LOCKDEP_CONFIG_HPP 1

#include <cstddef>
#include <type_traits>

/// The build system owns the feature level.  `config("lockdep")` in
/// //kernel/products/BUILD.gn turns the `lockdep_enable`,
/// `lockdep_metadata_only` and `lockdep_lock_name_tracing` GN arguments into
/// the two macros consumed below.
///
/// Deriving the level from those macros rather than hardcoding it is what keeps
/// a translation unit from disagreeing with the build it belongs to: a
/// hardcoded level silently gives every `Thread` a lock context even in builds
/// that asked for no lockdep at all.
#if defined(CONFIG_LOCK_DEP) && defined(LOCK_DEP_ENABLED_FEATURE_LEVEL)
#   define LOCKDEP_FEATURE_LEVEL LOCK_DEP_ENABLED_FEATURE_LEVEL
#else
#   define LOCKDEP_FEATURE_LEVEL 0
#endif

/// How many distinct classes one lock class may be recorded as preceding.
///
/// The dependency set is a fixed array so that recording an edge never
/// allocates -- edges are recorded from inside the acquire path, where
/// allocating is not an option.  Overflowing it costs completeness, not
/// soundness: the overflowing edge is reported and dropped.
#ifndef LOCKDEP_MAX_DEPENDENCIES
#   define LOCKDEP_MAX_DEPENDENCIES 32
#endif

namespace lockdep {
    enum class FeatureLevel {
        /// Disable all runtime features.
        Disabled = 0,

        /// Track lock classes, but do not validate acquisition order.
        MetadataOnly = 1,

        /// Track lock classes and validate acquisition order.
        Validation = 2,
    };

    constexpr auto kEnabledFeatureLevel = static_cast<FeatureLevel>(LOCKDEP_FEATURE_LEVEL);

    /// Whether or not lock metadata is enabled when validation is disabled.
    constexpr auto kLockMetadataOnlyEnabled = kEnabledFeatureLevel == FeatureLevel::MetadataOnly;

    /// Whether or not lock validation is globally enabled.
    constexpr auto kLockValidationEnabled = kEnabledFeatureLevel == FeatureLevel::Validation;

    /// Whether or not lock metadata is available.
    constexpr auto kLockMetadataAvailable = kLockValidationEnabled || kLockMetadataOnlyEnabled;

    template <typename T, typename U>
    using IfLockMetadataOnlyEnabled = std::conditional_t<kLockMetadataOnlyEnabled, T, U>;

    template <typename T, typename U>
    using IfLockValidationEnabled = std::conditional_t<kLockValidationEnabled, T, U>;

    template <typename T, typename U>
    using IfLockMetadataAvailable = std::conditional_t<kLockMetadataAvailable, T, U>;

    constexpr std::size_t kMaxDependencies = LOCKDEP_MAX_DEPENDENCIES;
    static_assert(kMaxDependencies > 0, "a lock class must be able to record at least one edge");

} // namespace lockdep

#endif // #ifndef LOCKDEP_CONFIG_HPP
