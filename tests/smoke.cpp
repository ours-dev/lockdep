// Compile-and-run smoke test for the lockdep headers.
// Built once per feature level; see run_tests.py.

#include <lockdep/config.hpp>
#include <lockdep/guard.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/report.hpp>
#include <lockdep/state_saver.hpp>
#include <lockdep/types.hpp>

#include <tuple>

#include <cstdio>

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

LOCK_DEP_TRAITS(SpinLock, lockdep::ValidationControl::IrqSafe);
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
LOCK_DEP_TRAITS(NestableLock, lockdep::ValidationControl::Nestable);

struct LeafLock {
    auto acquire() -> void {}
    auto release() -> void {}
};
LOCK_DEP_TRAITS(LeafLock, lockdep::ValidationControl::Leaf);

} // namespace fake

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

static_assert(lockdep::LockControlV<fake::SpinLock> == lockdep::ValidationControl::IrqSafe,
              "declared traits must be picked up through ADL");
static_assert(lockdep::LockControlV<fake::BareLock> == lockdep::ValidationControl::None,
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

auto early_unlock(fake::SpinLock &lock) -> bool {
    lockdep::Guard<fake::SpinLock, lockdep::IrqSave> guard{lock};
    guard.unlock();
    return !lock.held && !guard.held();
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

    return g_reacquire_reports == 1;
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

    return g_leaf_reports == 1;
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

    check("class_ids_are_distinct", class_ids_are_distinct());
    check("plain_guard", plain_guard(a));
    check("irq_guard", irq_guard(a));
    check("combined_guard", combined_guard(a));
    check("tag_object_guard", tag_object_guard(a));
    check("bare_lock_guard", bare_lock_guard(bare));
    check("early_unlock", early_unlock(a));
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
    }
    return g_failures == 0 ? 0 : 1;
}
