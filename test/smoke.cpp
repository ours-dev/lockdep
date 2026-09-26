// Compile-and-run smoke test for the lockdep headers.
// Built once per feature level; see run_tests.py.

#include <lockdep/config.hpp>
#include <lockdep/guard.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/lock_policy.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/runtime_api.hpp>
#include <lockdep/state_saver.hpp>
#include <lockdep/thread_lock_state.hpp>
#include <lockdep/types.hpp>

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <cstdio>

#include <ours/compiler.hpp>

namespace fake {

// Stands in for arch::AutoInterruptDisabler / task::AutoPreemptDisabler.
inline int g_irq_depth = 0;
inline int g_preempt_depth = 0;

struct AutoInterruptDisabler {
    AutoInterruptDisabler() {
        ++g_irq_depth;
    }
    ~AutoInterruptDisabler() {
        --g_irq_depth;
    }
};

struct AutoPreemptDisabler {
    AutoPreemptDisabler() {
        ++g_preempt_depth;
    }
    ~AutoPreemptDisabler() {
        --g_preempt_depth;
    }
};

struct SpinLock {
    auto acquire() -> void {
        held = true;
    }
    auto release() -> void {
        held = false;
    }
    bool held{false};
};

// A lock that declares neither traits nor state savers: it must still work
// through the fallbacks.
struct BareLock {
    auto acquire() -> void {}
    auto release() -> void {}
};

// A lock that, like the kernel's SpinLock, cannot be held safely without a
// saver, and says so: a Guard on it must name an option.
struct StrictLock {
    auto acquire() -> void {}
    auto release() -> void {}
};
LOCKDEP_STATE_SAVER(StrictLock, lockdep::SavedStateOption::IrqSave, AutoInterruptDisabler);
auto lockdep_option_required(StrictLock *) -> void;

LOCK_DEP_TRAITS(SpinLock, lockdep::ValidationOptions::IrqSafe);
LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::IrqSave, AutoInterruptDisabler);
LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::PreemptDisable, AutoPreemptDisabler);
// The combined option names its own saver, the way spin_lock.hpp does it.
auto lockdep_get_state_saver(SpinLock *, lockdep::IrqSaveAndPreemptDisable *)
    -> std::tuple<AutoInterruptDisabler, AutoPreemptDisabler>;

// Distinct owner types, so each gets its own lock class.
struct OwnerA {};
struct OwnerB {};
struct OwnerC {};
struct OwnerD {};
struct OwnerE {};
struct OwnerF {};
struct OwnerNestable {};
struct OwnerLeaf {};

struct NestableLock {
    auto acquire() -> void {}
    auto release() -> void {}
};
LOCK_DEP_TRAITS(NestableLock, lockdep::ValidationOptions::Nestable);

struct LeafLock {
    auto acquire() -> void {}
    auto release() -> void {}
};
LOCK_DEP_TRAITS(LeafLock, lockdep::ValidationOptions::Leaf);

// A lock annotated the way the kernel's locks are, so that -Wthread-safety
// checks what Guard and the policies do with it. The other fakes carry no
// annotations, and a policy that took a lock unannotated compiled cleanly
// against them while failing on every lock in the kernel.
inline int g_assert_held_calls = 0;

struct TA_CAPABILITY("mutex") AnnotatedLock {
    TA_ACQUIRE()
    auto acquire() -> void {}

    TA_RELEASE()
    auto release() -> void {}

    TA_ASSERT()
    auto assert_held() const -> void {
        ++g_assert_held_calls;
    }
};
LOCKDEP_STATE_SAVER(AnnotatedLock, lockdep::SavedStateOption::IrqSave, AutoInterruptDisabler);

// Records the order its instances are acquired in.
inline void const *g_sequence[8]{};
inline int g_sequence_length = 0;

struct SequencedLock {
    auto acquire() -> void {
        g_sequence[g_sequence_length] = this;
        ++g_sequence_length;
    }
    auto release() -> void {}
};

// A lock that declares policies of its own, the way the kernel's locks are
// meant to, including a try-lock that can fail.
struct PolicyLock {
    auto acquire() -> void {
        held = true;
    }
    auto try_acquire() -> bool {
        if (busy) {
            return false;
        }
        held = true;
        return true;
    }
    auto release() -> void {
        held = false;
    }
    bool held{false};
    bool busy{false};
};

// What the Guard asked of TracingPolicy, in order, and whether the lock
// context already held a record of the acquisition at each call.
struct TraceEntry {
    char call;
    bool recorded;
};
inline TraceEntry g_trace[8]{};
inline int g_trace_length = 0;
inline int g_saved = 0;

inline auto trace(char const call) -> void {
    auto const *const state =
        lockdep::system_get_thread_lock_state(lockdep::ValidationOptions::None);
    g_trace[g_trace_length] = TraceEntry{call, !state->empty()};
    ++g_trace_length;
}

// Saves "state" in pre_validate() and tries the lock in acquire(); a failed
// try undoes what pre_validate() did, which is the policy's job, not the
// Guard's.
struct TracingPolicy {
    struct State {
        bool saved{false};
    };

    using ValidationGuard = lockdep::NullValidationGuard;

    static auto pre_validate(PolicyLock &, State &state) -> void {
        trace('p');
        state.saved = true;
        ++g_saved;
    }

    static auto acquire(PolicyLock &lock, State &state) -> bool {
        trace('a');
        if (lock.try_acquire()) {
            return true;
        }
        state.saved = false;
        --g_saved;
        return false;
    }

    static auto release(PolicyLock &lock, State &state) -> void {
        trace('r');
        lock.release();
        state.saved = false;
        --g_saved;
    }
};

// An option tag of the lock's own: policies are not limited to lockdep's.
struct TryLock {};

auto lockdep_policy(PolicyLock *, TryLock *) -> TracingPolicy;
// Both a saver and a policy for IrqSave: the declared policy has to win.
LOCKDEP_STATE_SAVER(PolicyLock, lockdep::SavedStateOption::IrqSave, AutoInterruptDisabler);
auto lockdep_policy(PolicyLock *, lockdep::IrqSave *) -> TracingPolicy;

} // namespace fake

namespace lookalike {
// What ours::IrqSave used to be: an empty struct with the right name.
struct IrqSave {};
} // namespace lookalike

// --- environment seams -------------------------------------------------------

namespace {
// The two kinds of held-lock state an environment keeps: one for irq-safe
// locks -- the CPU's, in a kernel -- and one for everything else -- the
// running thread's. The split is Zircon's; see system_get_thread_lock_state().
lockdep::ThreadLockState g_thread_state;
lockdep::ThreadLockState g_cpu_state;

constexpr auto RESULTS = 16;
int g_results[RESULTS]{};
int g_report_count = 0;
int g_fatal_count = 0;
int g_cycle_reports = 0;
int g_trigger_count = 0;
lockdep::LockResult g_last_result = lockdep::LockResult::Success;

// Set to make the reporter itself take a lock, the way a kernel reporter takes
// its console lock.
lockdep::AcquiredLockEntry *g_reporter_entry = nullptr;

auto reset_reports() -> void {
    for (auto &count: g_results) {
        count = 0;
    }
    g_report_count = 0;
    g_fatal_count = 0;
    g_cycle_reports = 0;
    g_trigger_count = 0;
    g_last_result = lockdep::LockResult::Success;
}

auto reports_of(lockdep::LockResult const result) -> int {
    return g_results[static_cast<int>(result)];
}

// Whether any acquisition is recorded as held, on either state.
auto recorded() -> bool {
    return !g_thread_state.empty() || !g_cpu_state.empty();
}
} // namespace

namespace lockdep {
auto system_get_thread_lock_state(ValidationOptions const options) -> ThreadLockState * {
    if ((options & ValidationOptions::IrqSafe) != ValidationOptions::None) {
        return &g_cpu_state;
    }
    return &g_thread_state;
}

auto system_lock_validation_error(AcquiredLockEntry const &, AcquiredLockEntry const *,
                                  ThreadLockState const &, LockResult const result) -> void {
    ++g_results[static_cast<int>(result)];
    ++g_report_count;
    g_last_result = result;
    if (g_reporter_entry != nullptr) {
        // Validated in the middle of a report: must not report again.
        g_thread_state.acquire(*g_reporter_entry);
        g_thread_state.release(*g_reporter_entry);
    }
}

auto system_lock_validation_fatal(AcquiredLockEntry const &, ThreadLockState const &, LockResult)
    -> void {
    ++g_fatal_count;
}

auto system_circular_lock_dependency_detected(LockClassId) -> void {
    ++g_cycle_reports;
}

auto system_trigger_loop_detection() -> void {
    ++g_trigger_count;
}
} // namespace lockdep

// --- traits ------------------------------------------------------------------

static_assert(lockdep::LockValidationOptionsV<fake::SpinLock> ==
                  lockdep::ValidationOptions::IrqSafe,
              "declared traits must be picked up through ADL");
static_assert(lockdep::LockValidationOptionsV<fake::BareLock> == lockdep::ValidationOptions::None,
              "a lock with no LOCK_DEP_TRAITS must fall back to None");

// --- state savers ------------------------------------------------------------

static_assert(std::is_same_v<lockdep::StateSaver<fake::SpinLock, lockdep::IrqSave>,
                             fake::AutoInterruptDisabler>,
              "StateSaver must resolve to the declared saver, not to a container");
static_assert(std::is_same_v<lockdep::StateSaver<fake::SpinLock, lockdep::PreemptDisable>,
                             fake::AutoPreemptDisabler>,
              "the option must select between the savers a lock declares");
// A lock that declares no saver for an option does not resolve one at all:
// asking for it is a compile error, never a silent "save nothing".
template <typename Lock, typename Option>
concept HasStateSaver = requires { typename lockdep::StateSaver<Lock, Option>; };

static_assert(!HasStateSaver<fake::BareLock, lockdep::IrqSave>,
              "a lock with no saver must not fall back to saving nothing");

// Guard static_asserts on this, so an option-less Guard on StrictLock does not
// compile. That failure cannot be observed from here -- a static_assert in a
// class body is not a substitution failure -- so the trait it rests on is
// what is checked, in both directions.
static_assert(lockdep::OptionRequired<fake::StrictLock>,
              "a lock that declares lockdep_option_required() must require an option");
static_assert(!lockdep::OptionRequired<fake::SpinLock> && !lockdep::OptionRequired<fake::BareLock>,
              "a lock that declares nothing must keep guarding without options");

// --- policy lookup -----------------------------------------------------------

static_assert(lockdep::HasPolicy<fake::BareLock, lockdep::NoOption>,
              "a lock with no declarations is held with DefaultLockPolicy");
static_assert(!lockdep::HasPolicy<fake::StrictLock, lockdep::NoOption>,
              "a lock that demands an option has no option-less policy");
static_assert(!lockdep::HasPolicy<fake::BareLock, lockdep::IrqSave>,
              "an option the lock declares nothing for has no policy");
static_assert(!lockdep::HasPolicy<fake::SpinLock, lookalike::IrqSave>,
              "a look-alike tag matches no policy");
static_assert(
    std::is_same_v<lockdep::PolicyFor<fake::PolicyLock, lockdep::IrqSave>, fake::TracingPolicy>,
    "a declared policy wins over a declared saver");
static_assert(
    std::is_same_v<lockdep::PolicyFor<fake::PolicyLock, fake::TryLock>, fake::TracingPolicy>,
    "a lock's own option tags resolve like lockdep's");
static_assert(
    lockdep::LockPolicy<lockdep::PolicyFor<fake::SpinLock, lockdep::IrqSave>, fake::SpinLock>,
    "a state saver is presented as a complete policy");

// --- locks named at their declaration ----------------------------------------

// A type that owns locks the way kernel objects will: each declaration names
// its own class, with a tag declared on the spot.
struct Owner {
    lockdep::Lock<fake::SpinLock> m_first{lockdep::lock_class<struct OwnerFirstLock>()};
    lockdep::Lock<fake::SpinLock> m_second{
        lockdep::lock_class<struct OwnerSecondLock, lockdep::ValidationOptions::Leaf>()};
    // The same tag as m_first, on another lock type: a class of its own.
    lockdep::Lock<fake::BareLock> m_bare{lockdep::lock_class<struct OwnerFirstLock>()};
};

// A lock with static storage duration is constant-initialised, class id and
// all -- which is what keeps global locks out of static-initialisation order.
constinit lockdep::Lock<fake::BareLock> g_constant_lock{lockdep::lock_class<struct ConstantLock>()};

static_assert(lockdep::LOCK_METADATA_AVAILABLE ||
                  sizeof(lockdep::Lock<fake::SpinLock>) == sizeof(fake::SpinLock),
              "without metadata a Lock costs exactly what its inner lock costs");

auto named_classes() -> bool {
    Owner const one;
    Owner const two;
    if constexpr (!lockdep::LOCK_METADATA_AVAILABLE) {
        return one.m_first.id() == lockdep::INVALID_LOCK_CLASS_ID &&
               g_constant_lock.id() == lockdep::INVALID_LOCK_CLASS_ID;
    } else {
        auto const irq_safe = lockdep::ValidationOptions::IrqSafe;
        auto const leaf = lockdep::ValidationOptions::Leaf;
        return one.m_first.id() != lockdep::INVALID_LOCK_CLASS_ID &&
               // Two declarations, two classes.
               one.m_first.id() != one.m_second.id() &&
               // One declaration, one class, however many objects.
               one.m_first.id() == two.m_first.id() &&
               // One tag on two lock types, two classes.
               one.m_first.id() != one.m_bare.id() &&
               // The lock type's options, and the declaration's on top.
               lockdep::options_of(one.m_first.id()) == irq_safe &&
               lockdep::options_of(one.m_second.id()) == (irq_safe | leaf) &&
               g_constant_lock.id() != lockdep::INVALID_LOCK_CLASS_ID;
    }
}

// Guards over declared locks.

lockdep::Lock<fake::AnnotatedLock> g_wrapped{lockdep::lock_class<struct GuardedData>()};
int g_guarded_value TA_GUARDED(g_wrapped) = 0;

// Two locks of one type, each declared with its own class, nest without the
// validator taking them for one lock. Two bare locks of that type do not: they
// share DefaultLockClass, which is the limitation declaring them removes.
auto declared_locks_nest() -> bool {
    Owner owner;
    reset_reports();
    {
        lockdep::Guard outer{owner.m_first};
        lockdep::Guard inner{owner.m_second};
        static_assert(std::is_same_v<decltype(outer), lockdep::Guard<fake::SpinLock>>,
                      "a Guard over Lock<T> is a Guard<T>");
    }
    auto const declared_clean = reports_of(lockdep::LockResult::AlreadyAcquired) == 0;

    fake::SpinLock a;
    fake::SpinLock b;
    reset_reports();
    {
        lockdep::Guard outer{a};
        lockdep::Guard inner{b};
    }
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        return declared_clean && reports_of(lockdep::LockResult::AlreadyAcquired) == 1;
    } else {
        return declared_clean && reports_of(lockdep::LockResult::AlreadyAcquired) == 0;
    }
}

// The capability a Guard over a Lock acquires is the Lock, which is what the
// data is guarded by; -Wthread-safety would reject the write otherwise.
auto write_under_guard() -> bool {
    lockdep::Guard<fake::AnnotatedLock, lockdep::IrqSave> guard{g_wrapped};
    g_guarded_value = 1;
    return fake::g_irq_depth == 1;
}

// What a helper reached only with the lock held does: it asserts, the lock
// checks, and the analysis takes the assertion's word for the write.
auto touch_under_assertion() -> void {
    g_wrapped.assert_held();
    g_guarded_value = 2;
}

auto assert_held_checks() -> bool {
    lockdep::Guard<fake::AnnotatedLock, lockdep::IrqSave> guard{g_wrapped};
    auto const before = fake::g_assert_held_calls;
    touch_under_assertion();
    return fake::g_assert_held_calls == before + 1;
}

auto call_unlocked_drops_and_retakes(fake::SpinLock &lock) -> bool {
    lockdep::Guard<fake::SpinLock, lockdep::IrqSave> guard{lock};
    auto inside = false;
    guard.call_unlocked([&] {
        // Released through the policy: unlocked, state restored, unrecorded.
        inside = !lock.held && fake::g_irq_depth == 0 && !recorded();
    });
    auto const retaken = lock.held && guard.held() && fake::g_irq_depth == 1;
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        return inside && retaken && recorded();
    } else {
        return inside && retaken;
    }
}

auto call_untracked_keeps_the_lock(fake::SpinLock &lock) -> bool {
    lockdep::Guard<fake::SpinLock, lockdep::IrqSave> guard{lock};
    auto inside = false;
    guard.call_untracked([&] {
        // Still held, state still saved; only the record is gone.
        inside = lock.held && fake::g_irq_depth == 1 && !recorded();
    });
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        return inside && lock.held && recorded();
    } else {
        return inside && lock.held;
    }
}

// Ordered and multiple guards.

// Two locks of one Nestable class: one declaration's tag, so one class.
struct Tree {
    lockdep::Lock<fake::BareLock> m_parent{
        lockdep::lock_class<struct TreeNode, lockdep::ValidationOptions::Nestable>()};
    lockdep::Lock<fake::BareLock> m_child{
        lockdep::lock_class<struct TreeNode, lockdep::ValidationOptions::Nestable>()};
};

auto ordered_guards() -> bool {
    Tree tree;
    reset_reports();
    {
        lockdep::Guard parent{tree.m_parent, lockdep::LockOrder{1}};
        lockdep::Guard child{tree.m_child, lockdep::LockOrder{2}};
        static_assert(std::is_same_v<decltype(child), lockdep::Guard<fake::BareLock>>,
                      "an ordered Guard over Lock<T> is a Guard<T>");
    }
    auto const rising = g_report_count == 0;
    {
        lockdep::Guard parent{tree.m_parent, lockdep::LockOrder{2}};
        lockdep::Guard child{tree.m_child, lockdep::LockOrder{1}};
    }
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        return rising && reports_of(lockdep::LockResult::InvalidNesting) == 1;
    } else {
        return rising && g_report_count == 0;
    }
}

// Three locks of one class, as a ScopedGuard sees them.
struct Buckets {
    lockdep::Lock<fake::SequencedLock> m_first{lockdep::lock_class<struct Bucket>()};
    lockdep::Lock<fake::SequencedLock> m_second{lockdep::lock_class<struct Bucket>()};
    lockdep::Lock<fake::SequencedLock> m_third{lockdep::lock_class<struct Bucket>()};
};

// Taken in address order whatever order they are given in, so two call sites
// listing the same locks differently agree -- and the validator sees one class
// held three times with rising orders, which is no violation.
auto scoped_guard_takes_address_order() -> bool {
    Buckets buckets;
    reset_reports();
    fake::g_sequence_length = 0;
    auto all_held = false;
    {
        lockdep::ScopedGuard guard{buckets.m_third, buckets.m_first, buckets.m_second};
        static_assert(
            std::is_same_v<decltype(guard),
                           lockdep::ScopedGuard<lockdep::NoOption, fake::SequencedLock,
                                                fake::SequencedLock, fake::SequencedLock>>);
        all_held = static_cast<bool>(guard);
    }
    auto const ascending = fake::g_sequence_length == 3 &&
                           fake::g_sequence[0] < fake::g_sequence[1] &&
                           fake::g_sequence[1] < fake::g_sequence[2];
    {
        lockdep::ScopedGuard guard{buckets.m_second, buckets.m_third, buckets.m_first};
    }
    return all_held && ascending && g_report_count == 0 && !recorded();
}

// The option, given as a leading tag, applies to each lock: every saver is held
// while the group is. Without the tag the same locks are held with no saver --
// and a lock type that has policies for options must not lead deduction to
// read its first lock as a tag, whether one lock follows it or none.
auto scoped_guard_with_option() -> bool {
    struct Pair {
        lockdep::Lock<fake::SpinLock> m_left{lockdep::lock_class<struct PairLock>()};
        lockdep::Lock<fake::SpinLock> m_right{lockdep::lock_class<struct PairLock>()};
    } pair;
    auto unsaved = false;
    {
        lockdep::ScopedGuard guard{pair.m_left, pair.m_right};
        static_assert(
            std::is_same_v<decltype(guard), lockdep::ScopedGuard<lockdep::NoOption, fake::SpinLock,
                                                                 fake::SpinLock>>);
        unsaved = fake::g_irq_depth == 0;
    }
    {
        lockdep::ScopedGuard guard{pair.m_left};
        static_assert(std::is_same_v<decltype(guard),
                                     lockdep::ScopedGuard<lockdep::NoOption, fake::SpinLock>>);
        unsaved = unsaved && fake::g_irq_depth == 0;
    }
    auto saved = false;
    {
        lockdep::ScopedGuard guard{lockdep::IRQ_SAVE, pair.m_left, pair.m_right};
        static_assert(
            std::is_same_v<decltype(guard),
                           lockdep::ScopedGuard<lockdep::IrqSave, fake::SpinLock, fake::SpinLock>>);
        saved = fake::g_irq_depth == 2;
    }
    return unsaved && saved && fake::g_irq_depth == 0;
}

// A class is reported by name: a declared lock by its tag, a bare lock by its
// type.
auto class_names() -> bool {
    Owner const owner;
    std::string_view const declared = lockdep::name_of(owner.m_first.id());
    std::string_view const bare = lockdep::name_of(lockdep::DefaultLockClass<fake::SpinLock>::id());
    if constexpr (lockdep::LOCK_METADATA_AVAILABLE) {
        return declared == "OwnerFirstLock" && bare == "fake::SpinLock";
    } else {
        return declared.empty() && bare.empty();
    }
}

// lock() hands out the inner lock as the Lock's own capability, so taking it
// through lock() is balanced to -Wthread-safety.
auto annotated_through_lock() -> bool {
    lockdep::Lock<fake::AnnotatedLock> lock{lockdep::lock_class<struct AnnotatedWrapped>()};
    lock.lock().acquire();
    lock.lock().release();
    return true;
}

// --- lock classes ------------------------------------------------------------

static_assert(lockdep::LOCK_METADATA_AVAILABLE ==
                  (lockdep::ENABLED_FEATURE_LEVEL != lockdep::FeatureLevel::Disabled),
              "metadata availability must track the feature level");

auto class_ids_are_distinct() -> bool {
    auto const spin = lockdep::DefaultLockClass<fake::SpinLock>::id();
    auto const bare = lockdep::DefaultLockClass<fake::BareLock>::id();
    if constexpr (lockdep::LOCK_METADATA_AVAILABLE) {
        return spin != bare && spin != lockdep::INVALID_LOCK_CLASS_ID;
    } else {
        return spin == lockdep::INVALID_LOCK_CLASS_ID && bare == lockdep::INVALID_LOCK_CLASS_ID;
    }
}

// --- guards ------------------------------------------------------------------

auto plain_guard(fake::SpinLock &lock) -> bool {
    lockdep::Guard guard{lock};
    return lock.held;
}

auto irq_guard(fake::SpinLock &lock) -> bool {
    lockdep::Guard<fake::SpinLock, lockdep::IrqSave> guard{lock};
    // The saver is a member, so the state really is saved while the lock is held.
    return lock.held && fake::g_irq_depth == 1 && fake::g_preempt_depth == 0;
}

auto combined_guard(fake::SpinLock &lock) -> bool {
    lockdep::Guard<fake::SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{lock};
    return lock.held && fake::g_irq_depth == 1 && fake::g_preempt_depth == 1;
}

auto tag_object_guard(fake::SpinLock &lock) -> bool {
    lockdep::Guard guard{lock, lockdep::IRQ_SAVE};
    static_assert(std::is_same_v<decltype(guard), lockdep::Guard<fake::SpinLock, lockdep::IrqSave>>,
                  "the deduction guide must carry the tag into the template arguments");
    return lock.held && fake::g_irq_depth == 1;
}

auto bare_lock_guard(fake::BareLock &lock) -> bool {
    // A lock with no savers still guards with no options.
    lockdep::Guard guard{lock};
    return guard.held();
}

auto strict_guard(fake::StrictLock &lock) -> bool {
    // The only spelling StrictLock accepts.
    lockdep::Guard<fake::StrictLock, lockdep::IrqSave> guard{lock};
    return guard.held() && fake::g_irq_depth == 1;
}

auto early_unlock(fake::SpinLock &lock) -> bool {
    lockdep::Guard<fake::SpinLock, lockdep::IrqSave> guard{lock};
    guard.unlock();
    // Restored at the unlock, not at the end of the scope.
    return !lock.held && !guard.held() && fake::g_irq_depth == 0;
}

auto unguard_keeps_saved_state(fake::SpinLock &lock) -> bool {
    auto kept = false;
    {
        lockdep::Guard<fake::SpinLock, lockdep::IrqSave> guard{lock};
        guard.unguard();
        // Handed off, not released: the lock stays taken and the state stays
        // saved, but the record of the acquisition is gone with the frame.
        kept = lock.held && !guard.held() && fake::g_irq_depth == 1 && !recorded();
    }
    // The new owner releases; the frame that saved the state restored it.
    lock.release();
    return kept && fake::g_irq_depth == 0;
}

// Guard over an annotated lock, through both DefaultLockPolicy and a saver
// adapted into a policy, including an early unlock.
auto annotated_guards(fake::AnnotatedLock &lock) -> bool {
    {
        lockdep::Guard guard{lock};
    }
    lockdep::Guard<fake::AnnotatedLock, lockdep::IrqSave> guard{lock};
    auto const saved = fake::g_irq_depth == 1;
    guard.unlock();
    return saved && fake::g_irq_depth == 0;
}

auto reset_trace() -> void {
    fake::g_trace_length = 0;
    fake::g_saved = 0;
}

auto trace_is(char const *calls, bool const *recorded) -> bool {
    auto length = 0;
    while (calls[length] != 0) {
        ++length;
    }
    if (fake::g_trace_length != length) {
        return false;
    }
    for (auto i = 0; i < length; ++i) {
        if (fake::g_trace[i].call != calls[i] || fake::g_trace[i].recorded != recorded[i]) {
            return false;
        }
    }
    return true;
}

// pre_validate() before the record, acquire() after it, release() after the
// record is gone. The record only exists where validation is compiled in.
auto policy_order() -> bool {
    fake::PolicyLock lock;
    reset_trace();
    {
        lockdep::Guard<fake::PolicyLock, fake::TryLock> guard{lock};
        if (!guard.held() || !lock.held || fake::g_saved != 1) {
            return false;
        }
    }
    constexpr auto V = lockdep::LOCK_VALIDATION_ENABLED;
    bool const recorded[] = {false, V, false};
    return trace_is("par", recorded) && fake::g_saved == 0 && !lock.held;
}

// A try-lock that fails leaves no record, no saved state and nothing to
// release.
auto failed_try_lock() -> bool {
    fake::PolicyLock lock;
    lock.busy = true;
    reset_trace();
    auto nothing_left = false;
    {
        lockdep::Guard<fake::PolicyLock, fake::TryLock> guard{lock};
        nothing_left =
            !guard.held() && !static_cast<bool>(guard) && fake::g_saved == 0 && !recorded();
    }
    constexpr auto V = lockdep::LOCK_VALIDATION_ENABLED;
    bool const recorded[] = {false, V};
    return nothing_left && trace_is("pa", recorded);
}

auto void_guard() -> bool {
    lockdep::Guard<void> guard{};
    return !guard.held();
}

// --- guard cost --------------------------------------------------------------

// With validation compiled out the entry, the context slot and the savers must
// all vanish, leaving just the lock reference and the held flag.
static_assert(lockdep::LOCK_VALIDATION_ENABLED ||
                  sizeof(lockdep::Guard<fake::BareLock>) <= 2 * sizeof(void *),
              "a guard must cost nothing extra when validation is disabled");

// --- dependency graph --------------------------------------------------------

// Each lock class is named by a distinct owner type, so the graph has real
// nodes to connect.

template <typename Owner>
using ClassOf = lockdep::LockClass<Owner, fake::BareLock>;

// A class with options of its own, for the rules those options switch on.
template <typename Owner, lockdep::ValidationOptions Options>
using ClassWith = lockdep::LockClass<Owner, fake::BareLock, 0, Options>;

// Records an acquisition of |lock| under |Class| on the thread's state around
// |body|, the way a Guard would.
template <typename Class>
auto hold_as(fake::BareLock &lock, auto &&body, usize const order = 0) -> void {
    lockdep::AcquiredLockEntry entry{&lock, Class::id(), order};
    g_thread_state.acquire(entry);
    lock.acquire();
    body();
    lock.release();
    g_thread_state.release(entry);
}

template <typename Owner>
auto hold(fake::BareLock &lock, auto &&body) -> void {
    hold_as<ClassOf<Owner>>(lock, body);
}

auto records_edges_in_one_direction() -> bool {
    fake::BareLock a;
    fake::BareLock b;

    reset_reports();
    // A then B, twice: an ordering seen repeatedly is not a violation.
    hold<fake::OwnerA>(a, [&] { hold<fake::OwnerB>(b, [] {}); });
    hold<fake::OwnerA>(a, [&] { hold<fake::OwnerB>(b, [] {}); });

    if (g_report_count != 0) {
        std::printf("  consistent order reported %d violations\n", g_report_count);
        return false;
    }
    // Only the first acquisition added an edge, and only it asked for a pass.
    if (g_trigger_count != 1) {
        std::printf("  %d loop-detection triggers for one new edge\n", g_trigger_count);
        return false;
    }
    return lockdep::loop_detection_pass() == 0;
}

auto catches_direct_inversion() -> bool {
    fake::BareLock b;
    fake::BareLock c;

    reset_reports();
    hold<fake::OwnerB>(b, [&] { hold<fake::OwnerC>(c, [] {}); });
    // Now the other way round: this closes a two-node cycle.
    hold<fake::OwnerC>(c, [&] { hold<fake::OwnerB>(b, [] {}); });
    // And again: the edge is known now, so the inversion is not re-reported.
    hold<fake::OwnerC>(c, [&] { hold<fake::OwnerB>(b, [] {}); });

    if (reports_of(lockdep::LockResult::OutOfOrder) != 1) {
        std::printf("  inversion reported %d times at acquire time, not once\n",
                    reports_of(lockdep::LockResult::OutOfOrder));
        return false;
    }
    // And the detector must see the same cycle in the graph.
    if (lockdep::loop_detection_pass() == 0 || g_cycle_reports == 0) {
        std::printf("  detector found no cycle after an inversion\n");
        return false;
    }
    return true;
}

// The case the acquire-time fast path cannot see: no two of these acquisitions
// are an inversion of each other, but together they close a three-node cycle.
// Only the graph walk finds it.
auto catches_transitive_cycle() -> bool {
    fake::BareLock d;
    fake::BareLock e;
    fake::BareLock f;

    auto const before = lockdep::loop_detection_pass();

    reset_reports();
    hold<fake::OwnerD>(d, [&] { hold<fake::OwnerE>(e, [] {}); }); // D -> E
    hold<fake::OwnerE>(e, [&] { hold<fake::OwnerF>(f, [] {}); }); // E -> F
    hold<fake::OwnerF>(f, [&] { hold<fake::OwnerD>(d, [] {}); }); // F -> D

    if (reports_of(lockdep::LockResult::OutOfOrder) != 0) {
        std::printf("  acquire path claimed to see a transitive cycle\n");
        return false;
    }

    reset_reports();
    auto const after = lockdep::loop_detection_pass();
    if (after <= before) {
        std::printf("  detector missed the transitive cycle (%zu -> %zu)\n", before, after);
        return false;
    }
    // One report for the one new cycle, naming its root.
    if (g_cycle_reports != static_cast<int>(after)) {
        std::printf("  %d cycle reports for %zu cycles\n", g_cycle_reports, after);
        return false;
    }

    // A second pass must reach the same conclusion; the walk marks nodes as it
    // goes and has to leave them usable for the next run.
    if (lockdep::loop_detection_pass() != after) {
        std::printf("  a repeated pass disagreed with the first\n");
        return false;
    }
    return true;
}

// Releases need not nest. Take two, drop the outer one first, and the list
// must still end up empty.
auto release_need_not_nest() -> bool {
    fake::BareLock a;
    fake::BareLock b;
    lockdep::AcquiredLockEntry outer{&a, ClassOf<fake::OwnerA>::id()};
    lockdep::AcquiredLockEntry inner{&b, ClassOf<fake::OwnerB>::id()};

    g_thread_state.acquire(outer);
    g_thread_state.acquire(inner);
    g_thread_state.release(outer);
    if (g_thread_state.empty()) {
        std::printf("  releasing the outer entry dropped the inner one too\n");
        return false;
    }
    g_thread_state.release(inner);
    return g_thread_state.empty();
}

// A second lock of a class already held.
template <typename Class>
auto second_of_class(void const *first, void const *second, usize const first_order,
                     usize const second_order) -> lockdep::LockResult {
    reset_reports();
    lockdep::AcquiredLockEntry outer{first, Class::id(), first_order};
    lockdep::AcquiredLockEntry inner{second, Class::id(), second_order};
    g_thread_state.acquire(outer);
    g_thread_state.acquire(inner);
    auto const result = g_thread_state.last_result();
    g_thread_state.release(inner);
    g_thread_state.release(outer);
    return result;
}

auto same_class_rules() -> bool {
    fake::BareLock a;
    fake::BareLock b;
    using Plain = ClassOf<fake::OwnerA>;
    using Nestable = ClassWith<struct NestableOwner, lockdep::ValidationOptions::Nestable>;
    using Multi = ClassWith<struct MultiOwner, lockdep::ValidationOptions::MultiAcquire>;
    using Fatal = ClassWith<struct FatalOwner, lockdep::ValidationOptions::ReAcquireFatal>;

    using lockdep::LockResult;
    // The very same lock twice, whatever the class says.
    auto const reentrance = second_of_class<Multi>(&a, &a, 0, 0) == LockResult::Reentrance;
    // A second lock of a plain class.
    auto const already = second_of_class<Plain>(&a, &b, 0, 0) == LockResult::AlreadyAcquired;
    // A nestable class needs its order to rise, and then nests freely.
    auto const nesting = second_of_class<Nestable>(&a, &b, 2, 1) == LockResult::InvalidNesting;
    auto const nested = second_of_class<Nestable>(&a, &b, 1, 2) == LockResult::Success;
    // Any number of a MultiAcquire class, in any order.
    auto const multi = second_of_class<Multi>(&a, &b, 0, 0) == LockResult::Success;
    // ReAcquireFatal makes the plain mistake fatal.
    auto const fatal =
        second_of_class<Fatal>(&a, &b, 0, 0) == LockResult::AlreadyAcquired && g_fatal_count == 1;
    return reentrance && already && nesting && nested && multi && fatal;
}

auto leaf_must_be_last() -> bool {
    fake::BareLock leaf;
    fake::BareLock other;

    reset_reports();
    hold_as<ClassWith<struct LeafOwner, lockdep::ValidationOptions::Leaf>>(
        leaf, [&] { hold<fake::OwnerC>(other, [] {}); });
    // Reported, and fatal.
    return reports_of(lockdep::LockResult::AcquireAfterLeaf) == 1 && g_fatal_count == 1 &&
           g_last_result == lockdep::LockResult::AcquireAfterLeaf;
}

// An irq-unsafe lock taken while an irq-safe one is held. The environment
// keeps the two kinds on different states, so this can only be seen where it
// hands out one state for both -- the check is there for such environments.
auto irq_safety_is_checked() -> bool {
    fake::BareLock safe;
    fake::BareLock unsafe;

    reset_reports();
    hold_as<ClassWith<struct IrqSafeOwner, lockdep::ValidationOptions::IrqSafe>>(
        safe, [&] { hold_as<ClassOf<struct IrqUnsafeOwner>>(unsafe, [] {}); });
    return reports_of(lockdep::LockResult::InvalidIrqSafety) == 1;
}

// While a ReportingDisabled lock is held nothing is reported, and reporting
// resumes when it is released.
auto reporting_disabled_suspends_reports() -> bool {
    fake::BareLock quiet;
    fake::BareLock a;
    fake::BareLock b;
    using Quiet = ClassWith<struct QuietOwner, lockdep::ValidationOptions::ReportingDisabled>;
    using Plain = ClassOf<struct QuietPlainOwner>;

    reset_reports();
    hold_as<Quiet>(quiet, [&] {
        // A second lock of a plain class: would be AlreadyAcquired.
        hold_as<Plain>(a, [&] { hold_as<Plain>(b, [] {}); });
    });
    auto const suspended = g_report_count == 0;
    hold_as<Plain>(a, [&] { hold_as<Plain>(b, [] {}); });
    return suspended && g_report_count == 1;
}

// A reporter that takes a lock is not validated into a second report.
auto reporter_cannot_recurse() -> bool {
    fake::BareLock a;
    fake::BareLock b;
    fake::BareLock console;
    using Plain = ClassOf<struct RecursionOwner>;
    // The reporter's lock is of the same class, so were it validated it would
    // be one more AlreadyAcquired.
    lockdep::AcquiredLockEntry reporter{&console, Plain::id()};

    reset_reports();
    g_reporter_entry = &reporter;
    hold_as<Plain>(a, [&] { hold_as<Plain>(b, [] {}); });
    g_reporter_entry = nullptr;
    return g_report_count == 1;
}

// ActiveListDisabled: validated, but not kept on the list.
auto active_list_disabled() -> bool {
    fake::BareLock a;
    fake::BareLock b;
    using Hidden = ClassWith<struct HiddenOwner, lockdep::ValidationOptions::ActiveListDisabled>;

    reset_reports();
    auto hidden_kept = true;
    hold_as<ClassOf<struct HiddenOuterOwner>>(a, [&] {
        hold_as<Hidden>(b, [&] { hidden_kept = !g_thread_state.empty() && g_trigger_count == 1; });
    });
    // The outer lock is on the list; the hidden one is not, but its edge was
    // recorded all the same.
    return hidden_kept;
}

// TrackingDisabled: neither recorded nor validated.
auto tracking_disabled() -> bool {
    fake::BareLock a;
    fake::BareLock b;
    using Untracked =
        ClassWith<struct UntrackedOwner, lockdep::ValidationOptions::TrackingDisabled>;

    reset_reports();
    auto untracked = false;
    hold_as<Untracked>(a, [&] {
        untracked = g_thread_state.empty();
        hold_as<Untracked>(b, [] {});
    });
    return untracked && g_report_count == 0 && g_trigger_count == 0;
}

// One more distinct class after a hub than the hub's set has slots for: the
// last edge does not fit, and says so once.
template <usize... Indices>
auto fan_out(std::index_sequence<Indices...>) -> int {
    fake::BareLock hub;
    fake::BareLock leaf;
    reset_reports();
    hold_as<ClassOf<struct FanOutHub>>(hub, [&] {
        (hold_as<lockdep::LockClass<struct FanOutLeaf, fake::BareLock, Indices>>(leaf, [] {}), ...);
    });
    return reports_of(lockdep::LockResult::MaxLockDependencies);
}

auto full_set_is_reported() -> bool {
    return fan_out(std::make_index_sequence<lockdep::DEPENDENCY_SET_CAPACITY + 1>{}) == 1;
}

auto no_locks_held_is_asserted() -> bool {
    fake::BareLock a;

    reset_reports();
    lockdep::assert_no_locks_held();
    auto const clean = g_fatal_count == 0;
    hold_as<ClassOf<struct AssertOwner>>(a, [] { lockdep::assert_no_locks_held(); });
    return clean && g_fatal_count == 1;
}

// A Guard records an irq-safe lock on the CPU's state and anything else on the
// thread's, as system_get_thread_lock_state() hands them out.
auto states_follow_irq_safety(fake::SpinLock &irq_safe, fake::BareLock &plain) -> bool {
    auto on_cpu = false;
    {
        lockdep::Guard guard{irq_safe};
        on_cpu = !g_cpu_state.empty() && g_thread_state.empty();
    }
    auto on_thread = false;
    {
        lockdep::Guard guard{plain};
        on_thread = g_cpu_state.empty() && !g_thread_state.empty();
    }
    return on_cpu && on_thread;
}

// --- driver ------------------------------------------------------------------

namespace {
int g_failures = 0;

auto check(char const *name, bool ok) -> void {
    if (!ok) {
        std::printf("  FAIL: %s\n", name);
        ++g_failures;
    }
}
} // namespace

auto main() -> int {
    fake::SpinLock a;
    fake::BareLock bare;
    fake::StrictLock strict;
    fake::AnnotatedLock annotated;

    check("class_ids_are_distinct", class_ids_are_distinct());
    check("named_classes", named_classes());
    check("class_names", class_names());
    check("annotated_through_lock", annotated_through_lock());
    check("declared_locks_nest", declared_locks_nest());
    check("write_under_guard", write_under_guard());
    check("assert_held_checks", assert_held_checks());
    check("call_unlocked_drops_and_retakes", call_unlocked_drops_and_retakes(a));
    check("call_untracked_keeps_the_lock", call_untracked_keeps_the_lock(a));
    check("ordered_guards", ordered_guards());
    check("scoped_guard_takes_address_order", scoped_guard_takes_address_order());
    check("scoped_guard_with_option", scoped_guard_with_option());
    check("plain_guard", plain_guard(a));
    check("irq_guard", irq_guard(a));
    check("combined_guard", combined_guard(a));
    check("tag_object_guard", tag_object_guard(a));
    check("bare_lock_guard", bare_lock_guard(bare));
    check("strict_guard", strict_guard(strict));
    check("early_unlock", early_unlock(a));
    check("annotated_guards", annotated_guards(annotated));
    check("unguard_keeps_saved_state", unguard_keeps_saved_state(a));
    check("policy_order", policy_order());
    check("failed_try_lock", failed_try_lock());
    check("void_guard", void_guard());

    check("savers unwound", fake::g_irq_depth == 0 && fake::g_preempt_depth == 0);
    check("context drained", !recorded());

    // The graph only exists when validation is compiled in.  Everywhere else
    // these paths must still run and simply observe nothing.
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        check("records_edges_in_one_direction", records_edges_in_one_direction());
        check("release_need_not_nest", release_need_not_nest());
        check("catches_transitive_cycle", catches_transitive_cycle());
        check("same_class_rules", same_class_rules());
        check("leaf_must_be_last", leaf_must_be_last());
        check("irq_safety_is_checked", irq_safety_is_checked());
        check("reporting_disabled_suspends_reports", reporting_disabled_suspends_reports());
        check("reporter_cannot_recurse", reporter_cannot_recurse());
        check("active_list_disabled", active_list_disabled());
        check("tracking_disabled", tracking_disabled());
        check("no_locks_held_is_asserted", no_locks_held_is_asserted());
        check("full_set_is_reported", full_set_is_reported());
        check("states_follow_irq_safety", states_follow_irq_safety(a, bare));
        // Run last: it leaves a cycle in the graph on purpose.
        check("catches_direct_inversion", catches_direct_inversion());
    } else {
        reset_reports();
        check("detector is inert", lockdep::loop_detection_pass() == 0);
        check("nothing reported", g_report_count == 0);
    }

    if (g_failures != 0) {
        std::printf("  %d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
