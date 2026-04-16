#pragma once

#include <lockdep/types.hpp>
#include <lockdep/config.hpp>
#include <lockdep/lock-class.hpp>

#include <bits/bitmask.hpp>

#include <ustl/collections/intrusive/slist.hpp>

namespace lockdep {
namespace priv {
    class AcquiredLockEntry {
        using Self = AcquiredLockEntry;
      public:
        enum class Flags: u32 {
            None = 0,
            ReadOnly = 0_bit,
        };
        BITS_IMPL_BITMASK_INNER(Flags);

        AcquiredLockEntry(void *lock, u32 order, usize acquired_ip, LockClassId id)
            : m_lock(lock)
            , m_flags(Flags::None)
            , m_order(order)
            , m_acquired_ip(acquired_ip)
            , m_class(id) {
        }

        AcquiredLockEntry() = default;

        auto cid() const -> LockClassId { return m_class; }
        auto flags() const -> Flags { return m_flags; }
        auto order() const -> u32 { return m_order; }
        auto lock() const -> void * { return m_lock; }
      private:
        AcquiredLockEntry(Self &&) = delete;
        AcquiredLockEntry(Self const &) = delete;
        auto operator=(Self &&) -> Self & = delete;
        auto operator=(Self const &) -> Self & = delete;

        /// The lock that was acquired.
        void *m_lock;
        Flags m_flags;
        u32 m_order;
        usize m_acquired_ip;
        LockClassId m_class;

        ustl::collections::intrusive::SlistMemberHook<> m_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_hook, HookOption);
      public:
        USTL_DECLARE_SLIST(Self, List, HookOption);
    };

    class LockContext {
        using Self = LockContext;
      public:
        auto acquire(AcquiredLockEntry &entry) -> void;
        auto release(AcquiredLockEntry &entry) -> void;
      private:
        auto validate_acquire() -> void;
        auto report(LockClassId bad, LockClassId conflicting, LockResult result) -> void;

        AcquiredLockEntry::List m_acquired_locks;
        u32 m_reporting_disabled_count;
    }; // class LockContext

} // namespace lockdep::priv

} // namespace lockdep