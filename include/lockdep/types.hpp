/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_TYPES_HPP
#define LOCKDEP_TYPES_HPP 1

#include <cstddef>

namespace lockdep {
    class LockClassState;

    /// Identifies a lock class.  The address of the class' state object is
    /// unique and stable for the lifetime of the image, so it doubles as the id.
    using LockClassId = LockClassState const *;

    constexpr LockClassId kInvalidLockClassId = nullptr;

    /// lockdep sits below the kernel, so it cannot reach for `usize` from
    /// //kernel/include/ours/prelude.hpp.  Mirror ustl's convention instead.
    using Size = std::size_t;

} // namespace lockdep

#endif // #ifndef LOCKDEP_TYPES_HPP
