/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock-class.hpp>
#include <lockdep/lock-context.hpp>
#include <lockdep/state-saver.hpp>
#include <lockdep/types.hpp>

#include <utility>

#include <ustl/util/noncopyable.hpp>
#include <ustl/util/tuple.hpp>

namespace lockdep {
namespace priv {
    /// Remembers which context a `Guard` registered with, so the release
    /// unregisters from that same context.  Empty when validation is off.
    ///
    /// Access goes through `get`/`set` rather than the member directly: the
    /// holder's type does not depend on the guard's template parameters, so the
    /// discarded branch of an `if constexpr` inside the guard is still name
    /// checked and must compile against the empty specialisation too.
    template <bool Enabled>
    struct ContextHolder {
        auto get() const -> LockContext * {
            return context;
        }

        auto set(LockContext *value) -> void {
            context = value;
        }

        LockContext *context{nullptr};
    };

    template <>
    struct ContextHolder<false> {
        auto get() const -> LockContext * {
            return nullptr;
        }

        auto set(LockContext *) -> void {}
    };

    /// The `ScopedGuard` equivalent, one slot per lock.
    template <bool Enabled, Size Count>
    struct ContextArray {
        auto get(Size index) const -> LockContext * {
            return contexts[index];
        }

        auto set(Size index, LockContext *value) -> void {
            contexts[index] = value;
        }

        LockContext *contexts[Count]{};
    };

    template <Size Count>
    struct ContextArray<false, Count> {
        auto get(Size) const -> LockContext * {
            return nullptr;
        }

        auto set(Size, LockContext *) -> void {}
    };

} // namespace lockdep::priv

    /// The RAII object to help us easier to use custom lock features.
    ///
    /// `Options` are the `SavedStateContainer` tags naming the CPU state that
    /// must be saved before the lock is taken -- `lockdep::IrqSave`,
    /// `lockdep::PreemptDisable`, or their combination.  Each requested option
    /// is turned into the RAII saver the lock declared through
    /// `LOCKDEP_STATE_SAVER`, and that saver is a member of the guard, so the
    /// state really is saved rather than merely named.
    ///
    /// Member order is the contract: savers are constructed before the lock is
    /// acquired and destroyed after it is released.
    template <typename LockType, typename... Options>
    class Guard {
        using Self = Guard;

        static_assert((kIsSavedStateContainer<Options> && ...),
                      "Guard options must be lockdep state tags (lockdep::IrqSave, "
                      "lockdep::PreemptDisable, lockdep::IrqSaveAndPreemptDisable). "
                      "An unrelated empty struct would compile away to no saved state at all.");

      public:
        using Lock = LockType;

        /// The union of every requested option.
        static constexpr auto kOption = (SavedStateOption::None | ... | Options::kOption);

        explicit Guard(Lock &lock)
            : m_lock(lock)
            , m_entry(DefaultLockClass<Lock>::id()) {
            this->guard();
        }

        /// Tag-object form: `Guard guard{lock, kIrqSave}`.  The tags carry no
        /// data; they exist so the option can be spelled at the call site
        /// instead of in the template argument list.
        ///
        /// Constrained so it does not collide with the single-argument
        /// constructor when `Options` is empty.
        Guard(Lock &lock, Options...)
            requires(sizeof...(Options) > 0)
            : Guard(lock) {}

        ~Guard() {
            unlock();
        }

        /// Releases the lock early.  The saved CPU state stays saved until the
        /// guard goes out of scope, because the savers are scope-bound RAII
        /// objects with no re-arm step.
        auto unlock() -> void {
            if (m_held) {
                m_lock.release();
                if constexpr (kLockValidationEnabled) {
                    if (auto *context = m_context.get()) {
                        context->release(m_entry);
                        m_context.set(nullptr);
                    }
                }
                m_held = false;
            }
        }

        /// Gives up ownership without releasing the lock.
        auto unguard() -> void {
            if (m_held) {
                if constexpr (kLockValidationEnabled) {
                    if (auto *context = m_context.get()) {
                        context->release(m_entry);
                        m_context.set(nullptr);
                    }
                }
                m_held = false;
            }
        }

        auto held() const -> bool {
            return m_held;
        }

      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(Guard);

        auto guard() -> void {
            if constexpr (kLockValidationEnabled) {
                // Register before taking the lock: a report about the
                // acquisition that is about to deadlock has to name it.
                m_context.set(current_lock_context());
                if (auto *context = m_context.get()) {
                    context->acquire(m_entry);
                }
            }
            m_lock.acquire();
            m_held = true;
        }

        using IrqSaver = StateSaverIfRequested<Lock, kOption, SavedStateOption::IrqSave>;
        using PreemptSaver =
            StateSaverIfRequested<Lock, kOption, SavedStateOption::PreemptDisable>;

        /// Constructed first, destroyed last: interrupts are disabled around
        /// everything below, and preemption around the lock itself.
        [[no_unique_address]] IrqSaver m_irq_saver{};
        [[no_unique_address]] PreemptSaver m_preempt_saver{};

        Lock &m_lock;
        [[no_unique_address]] AcquiredLockEntry m_entry;
        [[no_unique_address]] priv::ContextHolder<kLockValidationEnabled> m_context{};
        bool m_held{false};
    };

    /// Degenerate guard for call sites that are generic over "some lock or no
    /// lock at all".
    template <>
    class Guard<void> {
      public:
        Guard(...) {}
        ~Guard() {}

        auto unlock() -> void {}
        auto unguard() -> void {}
        auto held() const -> bool { return false; }
    };

    template <typename LockType, typename... Options>
    Guard(LockType &, Options...) -> Guard<LockType, Options...>;

    /// Acquires several locks in the order written and releases them in the
    /// reverse order.
    ///
    /// Each lock is registered with the current context just like `Guard` does,
    /// so an ordering violation between the locks held here is still visible to
    /// the validator.
    template <typename... LockTypes>
    class ScopedGuard {
        using Self = ScopedGuard;

        static constexpr Size kCount = sizeof...(LockTypes);
        static_assert(kCount > 0, "ScopedGuard needs at least one lock");

      public:
        explicit ScopedGuard(LockTypes &...locks)
            : m_locks{&locks...}
            , m_entries{AcquiredLockEntry{DefaultLockClass<LockTypes>::id()}...} {
            acquire_all(std::index_sequence_for<LockTypes...>{});
        }

        ~ScopedGuard() {
            release_all(std::index_sequence_for<LockTypes...>{});
        }

        auto unlock() -> void {
            release_all(std::index_sequence_for<LockTypes...>{});
        }

      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(ScopedGuard);

        template <Size Index>
        auto acquire_at() -> void {
            if constexpr (kLockValidationEnabled) {
                m_contexts.set(Index, current_lock_context());
                if (auto *context = m_contexts.get(Index)) {
                    context->acquire(m_entries[Index]);
                }
            }
            std::get<Index>(m_locks)->acquire();
            m_held[Index] = true;
        }

        template <Size Index>
        auto release_at() -> void {
            if (!m_held[Index]) {
                return;
            }
            std::get<Index>(m_locks)->release();
            if constexpr (kLockValidationEnabled) {
                if (auto *context = m_contexts.get(Index)) {
                    context->release(m_entries[Index]);
                    m_contexts.set(Index, nullptr);
                }
            }
            m_held[Index] = false;
        }

        template <Size... Indices>
        auto acquire_all(std::index_sequence<Indices...>) -> void {
            (acquire_at<Indices>(), ...);
        }

        template <Size... Indices>
        auto release_all(std::index_sequence<Indices...>) -> void {
            // Reverse of the acquisition order.
            (release_at<kCount - 1 - Indices>(), ...);
        }

        ustl::Tuple<LockTypes *...> m_locks;
        AcquiredLockEntry m_entries[kCount];
        [[no_unique_address]] priv::ContextArray<kLockValidationEnabled, kCount> m_contexts{};
        bool m_held[kCount]{};
    };

    template <typename... LockTypes>
    ScopedGuard(LockTypes &...) -> ScopedGuard<LockTypes...>;

} // namespace lockdep

#endif // #ifndef LOCKDEP_GUARD_HPP
