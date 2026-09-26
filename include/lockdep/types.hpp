/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_TYPES_HPP
#define LOCKDEP_TYPES_HPP 1

namespace lockdep {
class LockClassState;

/// Identifies a lock class. The address of the class' state object is unique
/// and stable for the lifetime of the image, so it doubles as the id -- the
/// same trick as Zircon's LockClassId in lockdep/common.h.
using LockClassId = LockClassState const *;

constexpr LockClassId INVALID_LOCK_CLASS_ID = nullptr;

/// What the validator concluded about one acquisition. Zircon's LockResult
/// (lockdep/common.h), less its internal DependencyExists, which only ever
/// distinguished "new edge" from "known edge" inside the validator.
enum class LockResult : u8 {
    Success,

    /// A second lock of the same class, while the class is neither Nestable
    /// nor MultiAcquire.
    AlreadyAcquired,

    /// The reverse of this order has been recorded before: two contexts
    /// running the two orders can deadlock.
    OutOfOrder,

    /// A second lock of a Nestable class, without the rising order that
    /// nesting requires.
    InvalidNesting,

    /// An irq-unsafe lock taken while an irq-safe one is held.
    InvalidIrqSafety,

    /// The very same lock taken twice.
    Reentrance,

    /// Locks held where the caller asserted none are.
    ShouldNotHold,

    /// Anything taken while a Leaf lock is held.
    AcquireAfterLeaf,

    /// The class's dependency set is full, so an edge went unrecorded.
    /// Costs completeness, not soundness; raise LOCKDEP_MAX_DEPENDENCIES.
    MaxLockDependencies,
};

constexpr auto to_string(LockResult const result) -> char const * {
    switch (result) {
        case LockResult::Success:
            return "Success";
        case LockResult::AlreadyAcquired:
            return "Already Acquired";
        case LockResult::OutOfOrder:
            return "Out Of Order";
        case LockResult::InvalidNesting:
            return "Invalid Nesting";
        case LockResult::InvalidIrqSafety:
            return "Invalid Irq Safety";
        case LockResult::Reentrance:
            return "Reentrance";
        case LockResult::ShouldNotHold:
            return "Should Not Hold";
        case LockResult::AcquireAfterLeaf:
            return "Acquire After Leaf";
        case LockResult::MaxLockDependencies:
            return "Max Lock Dependencies";
    }
    return "Unknown";
}

} // namespace lockdep

#endif // #ifndef LOCKDEP_TYPES_HPP
