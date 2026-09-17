#pragma once

#include "config.hpp"
#include "types.hpp"
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/util/noncopyable.hpp>

namespace lockdep {
namespace priv {
    class AcquiredLockEntry {
        using Self = AcquiredLockEntry;

      public:
      private:
        USTL_NOCOPYABLE(AcquiredLockEntry);

        [[maybe_unused]]
        LockClassId m_id;

        ustl::collections::intrusive::SlistMemberHook<> m_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_hook, HookOption);

      public:
        USTL_DECLARE_SLIST(Self, List, HookOption);
    };

    class LockContext {
        using Self = LockContext;

      public:
        auto acquire(AcquiredLockEntry &entry) -> void;

      private:
        auto validate_acquire() -> void;

        AcquiredLockEntry::List m_acquired_locks;
    };

    class NullLockContext {};
} // namespace priv

using LockContext = IfLockValidationEnabled<priv::LockContext, priv::NullLockContext>;

} // namespace lockdep
