/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_RUNTIME_API_HPP
#define LOCKDEP_RUNTIME_API_HPP 1

#include <lockdep/lock_traits.hpp>
#include <lockdep/types.hpp>

/// What the environment has to provide for lockdep to validate anything.
///
/// Declared here, defined by the environment -- the kernel, or a test. All of
/// them are referenced only where validation is compiled in, so a build with
/// lockdep_enable=false links without any of them. Zircon's counterpart is
/// lockdep/runtime_api.h, and the kernel's definitions of it are in
/// kernel/lib/lockdep/lock_dep.cc.
namespace lockdep {
class AcquiredLockEntry;
class ThreadLockState;

/// The held-lock list that an acquisition of a lock with |options| belongs on.
///
/// The environment picks by the lock's options, as Zircon's
/// SystemGetThreadLockState() does: the per-CPU list for an IrqSafe lock and
/// the running thread's list otherwise. An irq-safe lock can be taken from
/// interrupt context, where the "current thread" is merely the one that was
/// interrupted, and recording it on that thread's list would mix two
/// contexts' acquisitions into one order.
///
/// May return nullptr where there is no context yet -- early boot, before the
/// first thread -- and the acquisition then goes unrecorded.
auto system_get_thread_lock_state(ValidationOptions options) -> ThreadLockState *;

/// Reports one violation. |conflicting| is the held entry the new one
/// conflicts with, where there is one.
///
/// Anything this takes a lock for -- a console lock, say -- must be one whose
/// class is ReportingDisabled, or be taken while reporting is suspended, as it
/// is for the duration of this call: otherwise reporting a violation would
/// validate, and possibly report, the reporter's own locks.
auto system_lock_validation_error(AcquiredLockEntry const &entry,
                                  AcquiredLockEntry const *conflicting,
                                  ThreadLockState const &state, LockResult result) -> void;

/// Reports a violation the class asked to be fatal -- a ReAcquireFatal class
/// taken twice, or anything taken after a Leaf lock. Expected not to return in
/// a kernel; a test harness may return, and validation then carries on.
auto system_lock_validation_fatal(AcquiredLockEntry const &entry, ThreadLockState const &state,
                                  LockResult result) -> void;

/// Reports a cycle in the lock order graph found by loop_detection_pass(),
/// naming one class in it.
auto system_circular_lock_dependency_detected(LockClassId root) -> void;

/// Tells the environment that the lock order graph has gained an edge.
///
/// The environment arranges for loop_detection_pass() to run later, on a
/// context of its own and never concurrently with itself -- the pass keeps
/// its scratch state in the graph. It is called from the acquire path, so it
/// must only take note: Zircon's kernel sets a flag that a lockdep thread
/// polls.
auto system_trigger_loop_detection() -> void;

} // namespace lockdep

#endif // #ifndef LOCKDEP_RUNTIME_API_HPP
