#pragma once

#include <cstdint>
#include <cstddef>

namespace lockdep {
    using i8    = ::std::int8_t;
    using i16   = ::std::int16_t;
    using i32   = ::std::int32_t;
    using i64   = ::std::int64_t;
    using isize = ::std::ptrdiff_t;

    using u8    = ::std::uint8_t;
    using u16   = ::std::uint16_t;
    using u32   = ::std::uint32_t;
    using u64   = ::std::uint64_t;
    using usize = ::std::size_t;

    class LockClass;
    using LockClassId = LockClass const *;

    // Result type that represents whether a lock attempt was successful, or if not
    // which check failed.
    enum class LockResult: u8 {
        Success,
        AlreadyAcquired,
        OutOfOrder,
        InvalidNesting,
        InvalidIrqSafety,
        Reentrance,
        ShouldNotHold,
        AcquireAfterLeaf,

        // Non-fatal error that indicates the dependency hash set for a particular
        // lock class is full. Consider increasing the size of the lock dependency
        // sets if this error is   reported.
        MaxLockDependencies,

        // Internal error value used to differentiate between dependency set updates
        // that add a new edge and those that do not. Only new     edges trigger loop
        // detection. 
        DependencyExists,
    };
} // namespace lockdep