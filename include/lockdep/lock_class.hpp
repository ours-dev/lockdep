/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CLASS_HPP
#define LOCKDEP_LOCK_CLASS_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/types.hpp>

#include <utility>

#include <ours/compiler.hpp>

namespace lockdep {
namespace priv {
    template <bool Enabled, typename Class, typename Lock, usize Index, ValidationOptions Options>
    class LockClassStateStorage;

    template <typename Class, typename Lock, usize Index, ValidationOptions Options>
    class LockClassStateStorage<true, Class, Lock, Index, Options> {
        using State = IfLockValidationEnabled<ValidatorLockClassState, MetadataLockClassState>;

      public:
        /// Used to be declared and never defined, so the first caller would
        /// have failed to link. constexpr so that a lock with static storage
        /// duration can be constant-initialised with its class id.
        static constexpr auto id() -> LockClassId {
            return m_lock_class_state.id();
        }

        static auto state() -> State const & {
            return m_lock_class_state;
        }

      private:
        /// One object per (Class, Lock, Index, Options) instantiation. Its
        /// address is the class id, which is why it is an inline variable
        /// rather than defined out of line: every translation unit must agree
        /// on it. Zircon's LockClassStateStorage::lock_class_state_ in
        /// lockdep/lock_class.h plays the same dual role.
        static inline State m_lock_class_state{Options};
    };

    template <typename Class, typename Lock, usize Index, ValidationOptions Options>
    class LockClassStateStorage<false, Class, Lock, Index, Options> {
      public:
        static constexpr auto id() -> LockClassId {
            return INVALID_LOCK_CLASS_ID;
        }
    };

} // namespace priv

/// Names a lock class. |Class| is the type that owns the lock, |Index|
/// separates several locks of the same type within one owner.
template <typename Class, typename Lock, usize Index = 0,
          ValidationOptions Options = LockValidationOptionsV<Lock>>
class LockClass
    : public priv::LockClassStateStorage<LOCK_METADATA_AVAILABLE, Class, Lock, Index, Options> {};

/// The class a bare lock falls back to when the acquisition site names no
/// owner. Every lock of the same type shares it.
///
/// Deliberately coarse, and the reason docs/interface.md replaces it: until
/// each lock declaration names its own class, two different spinlocks are one
/// class to the validator and ordinary nesting reads as re-acquisition.
/// Keeping the lookup behind this one alias is what keeps that change local.
template <typename Lock>
using DefaultLockClass = LockClass<void, Lock, 0, LockValidationOptionsV<Lock>>;

/// The name of a lock class, written where a lock is declared:
///
///     lockdep::Lock<sync::SpinLock> m_pi_lock{lockdep::lock_class<struct ThreadPiLock>()};
///
/// A token and nothing more. It carries the tag that names the class and the
/// options the declaration adds; the class itself is made where the token
/// meets the lock type, in Lock's constructor. That is because a lock class
/// belongs to a (tag, lock type) pair rather than to the tag alone: its
/// options are the union of what the declaration says and what the lock type
/// says about every lock of its kind -- every SpinLock is IrqSafe -- and only
/// the constructor knows both. Zircon keys LockClass on the lock type for the
/// same reason (lockdep/lock_class.h).
///
/// The tag is any type, and is usually declared on the spot by the
/// elaborated type specifier `struct ThreadPiLock`; its name is what a report
/// shows. Zircon names a class by its containing type and the line number,
/// through LOCK_DEP_INSTRUMENT's __LINE__; a tag does the same job in plain
/// syntax.
template <typename Tag, ValidationOptions Options>
struct LockClassName {};

/// Spelled as a function rather than a variable template because a constexpr
/// variable is named in UPPER_CASE in this tree, and `LOCK_CLASS<...>` at
/// every lock declaration would shout.
template <typename Tag, ValidationOptions Options = ValidationOptions::None>
constexpr auto lock_class() -> LockClassName<Tag, Options> {
    return {};
}

namespace priv {
    /// A free function, not a member of NamedLockClass: a static_assert in a
    /// class body runs before that class's member function bodies count as
    /// defined, so it cannot call them.
    constexpr auto has_option(ValidationOptions const options, ValidationOptions const option)
        -> bool {
        return (options & option) != ValidationOptions::None;
    }

    /// The class a Lock<LockType> named by |Tag| belongs to, with the options
    /// of the declaration and of the lock type merged.
    template <typename Tag, typename LockType, ValidationOptions DeclaredOptions>
    struct NamedLockClass {
        static constexpr auto OPTIONS = DeclaredOptions | LockValidationOptionsV<LockType>;

        /// The combinations Zircon's DefaultLockFlags rejects. MultiAcquire
        /// lets a context hold any number of locks of the class in any order,
        /// which is exactly what Nestable forbids and what ReAcquireFatal
        /// would treat as fatal.
        static_assert(!(has_option(OPTIONS, ValidationOptions::MultiAcquire) &&
                        has_option(OPTIONS, ValidationOptions::Nestable)),
                      "MultiAcquire and Nestable are mutually exclusive");
        static_assert(!(has_option(OPTIONS, ValidationOptions::MultiAcquire) &&
                        has_option(OPTIONS, ValidationOptions::ReAcquireFatal)),
                      "MultiAcquire and ReAcquireFatal are mutually exclusive");

        using Storage = LockClassStateStorage<LOCK_METADATA_AVAILABLE, Tag, LockType, 0, OPTIONS>;
    };

    /// A lock's class id, or nothing at all when there is no metadata to point
    /// at -- so that a Lock then costs exactly what the lock inside it costs.
    template <bool Available>
    struct ClassIdHolder {
        constexpr explicit ClassIdHolder(LockClassId const id) : m_id(id) {}

        [[nodiscard]]
        constexpr auto get() const -> LockClassId {
            return m_id;
        }

        LockClassId m_id;
    };

    template <>
    struct ClassIdHolder<false> {
        constexpr explicit ClassIdHolder(LockClassId) {}

        [[nodiscard]]
        constexpr auto get() const -> LockClassId {
            return INVALID_LOCK_CLASS_ID;
        }
    };

} // namespace priv

/// A lock type that can check that the current context holds it.
template <typename LockType>
concept AssertsHeld = requires(LockType const &lock) { lock.assert_held(); };

/// A lock and the class it belongs to.
///
/// Composition rather than Zircon's shape. Zircon makes the class part of the
/// type, `LockDep<Class, LockType, Index> : Lock<LockType>`, and then needs
/// the base class to erase it again so that an accessor can return
/// `Lock<LockType> *` whatever the class. Holding the class id as a member
/// makes `Lock<SpinLock>` the one type from the start: there is nothing to
/// erase and no base class. Zircon stores the id in the lock anyway
/// (Lock::AvailableState::id_), so this pays nothing it did not.
///
/// The object is the capability the thread-safety analysis sees: guard
/// members with TA_GUARDED(m_lock) where m_lock is the Lock, and lock()
/// hands the inner lock out as that same capability.
template <typename LockType>
class TA_CAPABILITY("lock") Lock {
    using Self = Lock;

  public:
    /// |args| go to the inner lock's constructor.
    template <typename Tag, ValidationOptions Options, typename... Args>
    constexpr explicit Lock(LockClassName<Tag, Options>, Args &&...args)
        : m_lock(std::forward<Args>(args)...)
        , m_id(priv::NamedLockClass<Tag, LockType, Options>::Storage::id()) {}

    Lock(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// The inner lock. Taking it directly bypasses lockdep: a Guard over this
    /// Lock is what records the acquisition.
    TA_RETURN_CAPABILITY(*this)
    auto lock() -> LockType & {
        return m_lock;
    }

    TA_RETURN_CAPABILITY(*this)
    auto lock() const -> LockType const & {
        return m_lock;
    }

    /// Asserts that the current context holds this lock, and tells the
    /// thread-safety analysis so for code it cannot follow -- a function
    /// reached only with the lock held, whose caller the analysis does not
    /// see. Zircon's lockdep::AssertHeld().
    ///
    /// A member, not a free function as in Zircon: the analysis attribute has
    /// to name what it asserts, and only a member can name its object
    /// (TA_ASSERT() means *this) in the leading position that trailing return
    /// types leave it. The runtime check is the inner lock's own
    /// assert_held(); a lock type without one cannot be asserted on, since an
    /// assertion the analysis trusts but nothing checks would be worse than
    /// none.
    TA_ASSERT()
    auto assert_held() const -> void
        requires AssertsHeld<LockType>
    {
        static_cast<void>(m_lock.assert_held());
    }

    /// INVALID_LOCK_CLASS_ID when lock metadata is compiled out.
    [[nodiscard]]
    constexpr auto id() const -> LockClassId {
        return m_id.get();
    }

  private:
    LockType m_lock;

    [[no_unique_address]]
    priv::ClassIdHolder<LOCK_METADATA_AVAILABLE> m_id;
};

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_HPP
