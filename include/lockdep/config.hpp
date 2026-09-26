/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_CONFIG_HPP
#define LOCKDEP_CONFIG_HPP 1

#include <type_traits>

/// Roughly how many distinct classes one lock class may be recorded as
/// preceding; the dependency set rounds it up to a prime (see
/// DEPENDENCY_SET_CAPACITY).
///
/// The set is a fixed array so that recording an edge never allocates --
/// edges are recorded from inside the acquire path, where allocating is not an
/// option. Overflowing it costs completeness, not soundness: the overflowing
/// edge is reported and dropped. Zircon's counterpart is
/// LOCK_DEP_MAX_DEPENDENCIES in lockdep/common.h.
#ifndef LOCKDEP_MAX_DEPENDENCIES
#define LOCKDEP_MAX_DEPENDENCIES 32
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

/// The build owns the feature level. config("lockdep") in
/// //kernel/products/BUILD.gn turns the lockdep_enable, lockdep_metadata_only
/// and lockdep_lock_name_tracing GN arguments into CONFIG_LOCK_DEP and
/// LOCK_DEP_ENABLED_FEATURE_LEVEL.
///
/// This used to be hardcoded to Validation, which ignored those arguments
/// entirely: a build that asked for no lockdep still gave every Thread a lock
/// context, and a translation unit could never disagree with the build it
/// belonged to only because it never listened to it.
#if defined(CONFIG_LOCK_DEP) && defined(LOCK_DEP_ENABLED_FEATURE_LEVEL)
constexpr auto ENABLED_FEATURE_LEVEL = static_cast<FeatureLevel>(LOCK_DEP_ENABLED_FEATURE_LEVEL);
#else
constexpr auto ENABLED_FEATURE_LEVEL = FeatureLevel::Disabled;
#endif

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

template <typename T, typename U>
using IfLockMetadataAvailable = std::conditional_t<LOCK_METADATA_AVAILABLE, T, U>;

constexpr usize MAX_DEPENDENCIES = LOCKDEP_MAX_DEPENDENCIES;
static_assert(MAX_DEPENDENCIES > 0, "a lock class must be able to record at least one edge");

} // namespace lockdep

#endif // #ifndef LOCKDEP_CONFIG_HPP
