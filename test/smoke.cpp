// Compile-and-run smoke test for the lockdep headers.
// Built once per feature level; see run_tests.py.

#include <lockdep/config.hpp>
#include <lockdep/guard.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/lock_policy.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/report.hpp>
#include <lockdep/state_saver.hpp>
#include <lockdep/types.hpp>

#include <tuple>

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
    g_trace[g_trace_length] = TraceEntry{call, !lockdep::current_lock_context()->empty()};
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
lockdep::Violation g_last_violation{};
int g_report_count = 0;
int g_cycle_reports = 0;
int g_reacquire_reports = 0;
int g_leaf_reports = 0;

auto reset_reports() -> void {
    g_report_count = 0;
    g_cycle_reports = 0;
    g_reacquire_reports = 0;
    g_leaf_reports = 0;
}
} // namespace

namespace lockdep {
static LockContext g_context;

auto current_lock_context() -> LockContext * {
    return &g_context;
}

auto report_violation(ViolationReport const &report) -> void {
    g_last_violation = report.violation;
    ++g_report_count;
    switch (report.violation) {
        case Violation::CircularDependency:
            ++g_cycle_reports;
            break;
        case Violation::ReAcquire:
            ++g_reacquire_reports;
            break;
        case Violation::LeafAcquire:
            ++g_leaf_reports;
            break;
        case Violation::DependencySetFull:
            break;
    }
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

auto options_of(lockdep::LockClassId const id) -> lockdep::ValidationOptions {
    return static_cast<lockdep::MetadataLockClassState const *>(id)->options();
}

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
               options_of(one.m_first.id()) == irq_safe &&
               options_of(one.m_second.id()) == (irq_safe | leaf) &&
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
    auto const declared_clean = g_reacquire_reports == 0;

    fake::SpinLock a;
    fake::SpinLock b;
    reset_reports();
    {
        lockdep::Guard outer{a};
        lockdep::Guard inner{b};
    }
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        return declared_clean && g_reacquire_reports == 1;
    } else {
        return declared_clean && g_reacquire_reports == 0;
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

auto recorded() -> bool {
    return !lockdep::current_lock_context()->empty();
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
        kept = lock.held && !guard.held() && fake::g_irq_depth == 1 &&
               lockdep::current_lock_context()->empty();
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
        nothing_left = !guard.held() && !static_cast<bool>(guard) && fake::g_saved == 0 &&
                       lockdep::current_lock_context()->empty();
    }
    constexpr auto V = lockdep::LOCK_VALIDATION_ENABLED;
    bool const recorded[] = {false, V};
    return nothing_left && trace_is("pa", recorded);
}

auto scoped_guard(fake::SpinLock &a, fake::SpinLock &b) -> bool {
    lockdep::ScopedGuard guard{a, b};
    return a.held && b.held;
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

// A guard that names its class explicitly, standing in for the instrumenting
// wrapper that call sites will eventually use.
template <typename Owner>
auto hold(fake::BareLock &lock, auto &&body) -> void {
    lockdep::AcquiredLockEntry entry{ClassOf<Owner>::id()};
    auto *context = lockdep::current_lock_context();
    context->acquire(entry);
    lock.acquire();
    body();
    lock.release();
    context->release(entry);
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
    return lockdep::detect_lock_order_cycles() == 0;
}

auto catches_direct_inversion() -> bool {
    fake::BareLock b;
    fake::BareLock c;

    reset_reports();
    hold<fake::OwnerB>(b, [&] { hold<fake::OwnerC>(c, [] {}); });
    // Now the other way round: this closes a two-node cycle.
    hold<fake::OwnerC>(c, [&] { hold<fake::OwnerB>(b, [] {}); });

    if (g_cycle_reports == 0) {
        std::printf("  inversion was not reported at acquire time\n");
        return false;
    }
    // And the detector must see the same cycle in the graph.
    auto const cycles = lockdep::detect_lock_order_cycles();
    if (cycles == 0) {
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

    auto const before = lockdep::detect_lock_order_cycles();

    reset_reports();
    hold<fake::OwnerD>(d, [&] { hold<fake::OwnerE>(e, [] {}); }); // D -> E
    hold<fake::OwnerE>(e, [&] { hold<fake::OwnerF>(f, [] {}); }); // E -> F
    hold<fake::OwnerF>(f, [&] { hold<fake::OwnerD>(d, [] {}); }); // F -> D

    if (g_cycle_reports != 0) {
        std::printf("  acquire path claimed to see a transitive cycle (%d reports)\n",
                    g_cycle_reports);
        return false;
    }

    reset_reports();
    auto const after = lockdep::detect_lock_order_cycles();
    if (after <= before) {
        std::printf("  detector missed the transitive cycle (%zu -> %zu)\n", before, after);
        return false;
    }
    if (g_cycle_reports == 0) {
        std::printf("  detector found a cycle but reported nothing\n");
        return false;
    }

    // A second pass must reach the same conclusion; the walk marks nodes as it
    // goes and has to leave them usable for the next run.
    if (lockdep::detect_lock_order_cycles() != after) {
        std::printf("  a repeated pass disagreed with the first\n");
        return false;
    }
    return true;
}

// LockContext::release claims releases need not nest.  Take two, drop the
// outer one first, and the list must still end up empty.
auto release_need_not_nest() -> bool {
    using ClassA = lockdep::LockClass<fake::OwnerA, fake::BareLock>;
    using ClassB = lockdep::LockClass<fake::OwnerB, fake::BareLock>;

    auto *context = lockdep::current_lock_context();
    lockdep::AcquiredLockEntry outer{ClassA::id()};
    lockdep::AcquiredLockEntry inner{ClassB::id()};

    context->acquire(outer);
    context->acquire(inner);
    context->release(outer);
    if (context->empty()) {
        std::printf("  releasing the outer entry dropped the inner one too\n");
        return false;
    }
    context->release(inner);
    return context->empty();
}

auto nestable_may_recurse() -> bool {
    using Class = lockdep::LockClass<fake::OwnerNestable, fake::NestableLock>;

    reset_reports();
    lockdep::AcquiredLockEntry outer{Class::id()};
    lockdep::AcquiredLockEntry inner{Class::id()};
    auto *context = lockdep::current_lock_context();
    context->acquire(outer);
    context->acquire(inner);
    context->release(inner);
    context->release(outer);

    return g_reacquire_reports == 0;
}

auto non_nestable_recursion_is_reported() -> bool {
    using Class = lockdep::LockClass<fake::OwnerA, fake::BareLock>;

    reset_reports();
    lockdep::AcquiredLockEntry outer{Class::id()};
    lockdep::AcquiredLockEntry inner{Class::id()};
    auto *context = lockdep::current_lock_context();
    context->acquire(outer);
    context->acquire(inner);
    context->release(inner);
    context->release(outer);

    return g_reacquire_reports == 1 && g_last_violation == lockdep::Violation::ReAcquire;
}

auto leaf_must_be_last() -> bool {
    fake::LeafLock leaf;
    fake::BareLock other;
    using LeafClass = lockdep::LockClass<fake::OwnerLeaf, fake::LeafLock>;

    reset_reports();
    lockdep::AcquiredLockEntry leaf_entry{LeafClass::id()};
    auto *context = lockdep::current_lock_context();
    context->acquire(leaf_entry);
    leaf.acquire();
    hold<fake::OwnerC>(other, [] {});
    leaf.release();
    context->release(leaf_entry);

    return g_leaf_reports == 1 && g_last_violation == lockdep::Violation::LeafAcquire;
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
    fake::SpinLock b;
    fake::BareLock bare;
    fake::StrictLock strict;
    fake::AnnotatedLock annotated;

    check("class_ids_are_distinct", class_ids_are_distinct());
    check("named_classes", named_classes());
    check("annotated_through_lock", annotated_through_lock());
    check("declared_locks_nest", declared_locks_nest());
    check("write_under_guard", write_under_guard());
    check("assert_held_checks", assert_held_checks());
    check("call_unlocked_drops_and_retakes", call_unlocked_drops_and_retakes(a));
    check("call_untracked_keeps_the_lock", call_untracked_keeps_the_lock(a));
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
    check("scoped_guard", scoped_guard(a, b));
    check("void_guard", void_guard());

    check("savers unwound", fake::g_irq_depth == 0 && fake::g_preempt_depth == 0);
    check("context drained", lockdep::current_lock_context()->empty());

    // The graph only exists when validation is compiled in.  Everywhere else
    // these paths must still run and simply observe nothing.
    if constexpr (lockdep::LOCK_VALIDATION_ENABLED) {
        check("records_edges_in_one_direction", records_edges_in_one_direction());
        check("release_need_not_nest", release_need_not_nest());
        check("catches_transitive_cycle", catches_transitive_cycle());
        check("nestable_may_recurse", nestable_may_recurse());
        check("non_nestable_recursion_is_reported", non_nestable_recursion_is_reported());
        check("leaf_must_be_last", leaf_must_be_last());
        // Run last: it leaves a cycle in the graph on purpose.
        check("catches_direct_inversion", catches_direct_inversion());
    } else {
        reset_reports();
        check("detector is inert", lockdep::detect_lock_order_cycles() == 0);
        check("nothing reported", g_report_count == 0);
    }

    if (g_failures != 0) {
        std::printf("  %d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
