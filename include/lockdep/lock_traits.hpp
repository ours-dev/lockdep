#pragma once

#include <ustl/util/enum_bits.hpp>
#include <ustl/util/tuple.hpp>

namespace lockdep {
enum class ValidationControl {
    None,

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
    static auto constexpr CONTROL = Control;
};

template <typename Lock>
struct MakeLockTraits {
    using ArgsTuple = decltype(lock_dep_extract_traits_helper(static_cast<Lock *>(nullptr)));

    using Type = LockTraitsImpl<Lock, ustl::TupleElementT<ArgsTuple, 0>::VALUE>;
};

template <typename Lock>
using LockTraits = typename MakeLockTraits<Lock>::Type;

} // namespace lockdep

/// Using function rather than class by pass namespace issues
/// brought by template class instanization.
#define LOCK_DEP_TRAITS(LOCK_TYPE, VALIDATION_CONTROL)                                             \
    constexpr auto lock_dep_extract_traits_helper(LOCK_TYPE *) -> ustl::Tuple<                     \
        ustl::traits::IntegralConstant<lockdep::ValidationControl, VALIDATION_CONTROL>>;