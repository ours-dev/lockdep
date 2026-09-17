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

} // namespace lockdep

#endif // #ifndef LOCKDEP_TYPES_HPP
