/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CONTEXT_HPP
#define LOCKDEP_LOCK_CONTEXT_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/thread_lock_state.hpp>

namespace lockdep {
/// The held-lock state a context owns: ThreadLockState where validation is
/// compiled in, and an empty stand-in elsewhere, so that a member such as the
/// kernel's `Thread::m_lock_context` costs nothing in a build without lockdep.
///
/// The name predates ThreadLockState -- Zircon's name, which says that the
/// state may belong to a CPU rather than a thread -- and stays for the members
/// declared with it.
using LockContext = IfLockValidationEnabled<ThreadLockState, priv::NullThreadLockState>;

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CONTEXT_HPP
