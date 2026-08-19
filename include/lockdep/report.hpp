/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_REPORT_HPP
#define LOCKDEP_REPORT_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/types.hpp>

namespace lockdep {
    enum class Violation {
        /// A lock of `acquiring` was taken while a lock of `held` was already
        /// held, and the reverse order has been observed before.  Two threads
        /// running the two orders can deadlock.
        CircularDependency,

        /// A lock of the same class was acquired twice by one context, and the
        /// class did not declare `Nestable` or `MultiAcquire`.
        ReAcquire,

        /// Something was acquired while a lock declared `Leaf` was held.  A
        /// leaf lock is the last lock a context may take.
        LeafAcquire,

        /// The dependency set of `held` is full, so this edge was not recorded.
        /// Validation stays sound for what it did record but is no longer
        /// complete; raise LOCKDEP_MAX_DEPENDENCIES if this fires.
        DependencySetFull,
    };

    struct ViolationReport {
        Violation violation;

        /// The class whose acquisition triggered the report.
        LockClassId acquiring;

        /// The class already held that the violation is about, or
        /// `kInvalidLockClassId` when the violation involves only one class.
        LockClassId held;

        /// Whether the class asked for this to be treated as fatal rather than
        /// merely reported.
        bool fatal;
    };

    /// Reports one violation.
    ///
    /// Supplied by the environment, like `current_lock_context()`: lockdep has
    /// no opinion on how a kernel panics or logs.  It is only referenced when
    /// validation is enabled, so a build with `lockdep_enable=false` links
    /// without providing it.
    auto report_violation(ViolationReport const &report) -> void;

} // namespace lockdep

#endif // #ifndef LOCKDEP_REPORT_HPP
