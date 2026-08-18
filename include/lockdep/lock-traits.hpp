/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_TRAITS_HPP
#define LOCKDEP_LOCK_TRAITS_HPP 1

#include <lockdep/types.hpp>

#include <ustl/traits/integral_constant.hpp>
#include <ustl/util/enum_bits.hpp>

namespace lockdep {
    enum class ValidationControl {
        None = 0,

        IrqSafe = (1 << 0),

        Nestable = (1 << 1),

        MultiAcquire = (1 << 2),

        Leaf = (1 << 3),

        ReportingDisabled = (1 << 4),

        SingletonLock = (1 << 5),

        ReAcquireFatal = (1 << 6),

        ActiveListDisabled = (1 << 7),

        TrackingDisabled = (1 << 8),
    };
    USTL_ENABLE_ENUM_BITMASK(ValidationControl);

    template <typename Lock, ValidationControl Control>
    struct LockTraitsImpl {
        using LockType = Lock;

        static auto constexpr kControl = Control;

        static auto constexpr has(ValidationControl control) -> bool {
            return (kControl & control) != ValidationControl::None;
        }
    };

} // namespace lockdep

/// Fallback for lock types that never declared traits.  It has to be visible
/// before `MakeLockTraits` is defined so ordinary lookup finds it at template
/// definition time, while ADL still picks up the per-lock overloads that
/// `LOCK_DEP_TRAITS` declares next to each lock type.
///
/// Without it a lock with no `LOCK_DEP_TRAITS` is a hard error rather than a
/// lock with default traits.
constexpr auto lock_dep_extract_traits_helper(void const volatile *)
    -> ustl::traits::IntConstant<lockdep::ValidationControl::None>;

namespace lockdep {
    template <typename Lock>
    struct MakeLockTraits {
        using Constant = decltype(lock_dep_extract_traits_helper(static_cast<Lock *>(nullptr)));

        using Type = LockTraitsImpl<Lock, Constant::value>;
    };

    template <typename Lock>
    using LockTraits = typename MakeLockTraits<Lock>::Type;

    template <typename Lock>
    constexpr auto kLockControlOf = LockTraits<Lock>::kControl;

} // namespace lockdep

/// Using a function rather than a class to bypass the namespace issues brought
/// by template class instantiation: the declaration can sit right next to the
/// lock type and still be found through ADL from inside `lockdep`.
#define LOCK_DEP_TRAITS(LOCK_TYPE, VALIDATION_CONTROL)                                             \
    constexpr auto lock_dep_extract_traits_helper(LOCK_TYPE *)                                     \
        -> ustl::traits::IntConstant<VALIDATION_CONTROL>;

#endif // #ifndef LOCKDEP_LOCK_TRAITS_HPP
