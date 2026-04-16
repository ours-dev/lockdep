#pragma once

#include <tuple> // std::tuple

#include <lockdep/types.hpp>

#include <bits/bitmask.hpp>

namespace lockdep {
    enum class LockFlags {
        None,

        IrqSafe = 0_bit,

        Nestable = 1_bit,

        MultiAcquire = 2_bit,

        Leaf = 3_bit,

        SingletonLock = 4_bit,

        ReAcquireFatal = 5_bit,

        ActiveListDisabled = 6_bit,

        TrackingDisabled = 7_bit,

        ReportingDisabled = 8_bit,
    };
    BITS_IMPL_BITMASK(LockFlags);

    template <typename Lock, LockFlags Control>
    struct LockTraitsImpl {
        static auto constexpr CONTROL = Control;
    };

    template <typename Lock>
    struct MakeLockTraits {
        using ArgsTuple = decltype(lock_dep_extract_traits_helper(static_cast<Lock *>(nullptr)));

        using Type = LockTraitsImpl<Lock, std::tuple_element_t<0, ArgsTuple>::VALUE>;
    }; 

    template <typename Lock>
    using LockTraits = typename MakeLockTraits<Lock>::Type;

} // namespace lockdep

/// Using function rather than class by pass namespace issues 
/// brought by template class instanization.
#define LOCK_DEP_TRAITS(LOCK_TYPE, LOCK_FLAGS) \
    constexpr auto lock_dep_extract_traits_helper(LOCK_TYPE *) \
        -> std::integral_constant<lockdep::LockFlags, LOCK_FLAGS>;