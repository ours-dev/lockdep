#include "lockdep/lock-context.hpp"

#include "lockdep/lock-class.hpp"
#include "lockdep/lock-environment.hpp"

namespace lockdep::priv {
    auto LockContext::acquire(AcquiredLockEntry &entry) -> void {
        auto const new_class = entry.cid();
        if (new_class->is_tracking_disabled()) {
            return;
        } else if (new_class->is_reporting_disabled()) {
            ++m_reporting_disabled_count;
            return;
        }

        for (auto &lock : m_acquired_locks) {
            auto const old_class = lock.cid();
            if (old_class->is_leaf()) {
                Self::report(new_class, old_class, LockResult::AcquireAfterLeaf);
            } else if (new_class == old_class) {
                if (entry.lock() == lock.lock()) {
                    // Same lock acquired multiple times.
                    Self::report(new_class, old_class, LockResult::Reentrance);
                } else if(!new_class->is_multi_acquire() && entry.order() <= lock.order()) {
                    // Non-multi-acquire lock and out of order.
                    if (!new_class->is_nestable() && entry.order() == 0) {
                        // Not nestable and same order
                        Self::report(new_class, old_class, LockResult::AlreadyAcquired);
                    } else {
                        // Nestable lock and out of order.
                        Self::report(new_class, old_class, LockResult::InvalidNesting);
                    }
                }
            } else {
                auto const result = LockClass::add_dependency(new_class, old_class);
                if (result == LockResult::Success) {
                }
            }
        }

        m_acquired_locks.push_back(entry);
    }

    auto LockContext::release(AcquiredLockEntry &entry) -> void {
        if (auto const cid = entry.cid(); cid->is_tracking_disabled()) {
            return;
        } else if (cid->is_reporting_disabled()) {
            --m_reporting_disabled_count;
            return;
        }

        m_acquired_locks.remove(entry);
    }
} // namespace lockdep::priv