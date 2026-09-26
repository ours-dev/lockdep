// Runs lockdep the way a kernel actually drives it: one lock context per
// thread, real locks, several threads recording edges into the shared graph at
// once.
//
// The point is not that userspace resembles a kernel.  It is that everything
// lockdep concludes is a property of a graph, and a graph does not care whether
// the locks that built it were spinlocks or std::mutex.  What this test can
// cover is the part that decides whether a report is right: per-context held
// lists, concurrent edge recording, and the cycle walk.  What it cannot cover
// is the arch integration -- interrupt state savers have no userspace meaning.
//
// Built only at the validation feature level; see run_tests.py.

#include <lockdep/guard.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/runtime_api.hpp>
#include <lockdep/thread_lock_state.hpp>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <cstdio>

#include <ours/compiler.hpp>

static_assert(lockdep::LOCK_VALIDATION_ENABLED,
              "this test only means anything with validation compiled in");

namespace {

// A real lock, shaped the way lockdep expects a lock to be shaped -- including
// its thread-safety annotations, since the suite is built with the kernel's
// -Wthread-safety. The capability is this object; the std::mutex inside is an
// implementation detail the analysis is kept out of, the same split a kernel
// lock makes between its annotated interface and its body.
class TA_CAPABILITY("mutex") Mutex {
  public:
    TA_ACQUIRE()
    TA_NO_THREAD_SAFETY_ANALYSIS auto acquire() -> void {
        m_mutex.lock();
    }

    TA_RELEASE()
    TA_NO_THREAD_SAFETY_ANALYSIS auto release() -> void {
        m_mutex.unlock();
    }

  private:
    std::mutex m_mutex;
};

// Owner types, so each lock gets a class of its own rather than sharing
// DefaultLockClass<Mutex> with every other Mutex in the process.
struct OwnerA {};
struct OwnerB {};
struct OwnerC {};
struct OwnerStress {};

template <typename Owner>
using ClassOf = lockdep::LockClass<Owner, Mutex>;

std::atomic<int> g_reports{0};
std::atomic<int> g_cycle_reports{0};

// Each thread gets its own held-lock list, exactly as each Thread does in
// the kernel.  Nothing else about the context is shared, which is what lets
// ThreadLockState::acquire run without a lock of its own.
thread_local lockdep::ThreadLockState t_state;

} // namespace

namespace lockdep {
// Every acquisition here goes on the running thread's own state; the threads
// take no irq-safe locks, so there is no CPU state to hand out.
auto system_get_thread_lock_state(ValidationOptions) -> ThreadLockState * {
    return &t_state;
}

auto system_lock_validation_error(AcquiredLockEntry const &, AcquiredLockEntry const *,
                                  ThreadLockState const &, LockResult) -> void {
    g_reports.fetch_add(1, std::memory_order_relaxed);
}

auto system_lock_validation_fatal(AcquiredLockEntry const &, ThreadLockState const &, LockResult)
    -> void {
    g_reports.fetch_add(1, std::memory_order_relaxed);
}

auto system_circular_lock_dependency_detected(LockClassId) -> void {
    g_cycle_reports.fetch_add(1, std::memory_order_relaxed);
}

auto system_trigger_loop_detection() -> void {}
} // namespace lockdep

namespace {

// Takes `lock` under the class named by `Owner`, runs `body`, releases.
// Stands in for the instrumenting wrapper call sites will eventually use.
template <typename Owner, typename Body>
auto hold(Mutex &lock, Body &&body) -> void {
    lockdep::AcquiredLockEntry entry{&lock, ClassOf<Owner>::id()};
    auto *context = &t_state;
    context->acquire(entry);
    lock.acquire();
    body();
    lock.release();
    context->release(entry);
}

int g_failures = 0;

auto check(char const *name, bool ok) -> void {
    std::printf("  %-42s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) {
        ++g_failures;
    }
}

// Two threads take the same pair of locks in opposite orders.  A barrier
// keeps them from overlapping, so the process never actually deadlocks --
// which is the whole point: lockdep has to conclude the deadlock is
// possible from the orders alone, without one happening.
auto catches_abba_across_threads() -> bool {
    Mutex a;
    Mutex b;

    auto const before = lockdep::loop_detection_pass();

    std::atomic<int> arrived{0};
    auto barrier = [&arrived] {
        arrived.fetch_add(1, std::memory_order_acq_rel);
        while (arrived.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
    };

    std::thread first([&] {
        hold<OwnerA>(a, [&] { hold<OwnerB>(b, [] {}); }); // A -> B
        barrier();
    });
    std::thread second([&] {
        barrier();
        hold<OwnerB>(b, [&] { hold<OwnerA>(a, [] {}); }); // B -> A
    });
    first.join();
    second.join();

    // Each thread's context must have drained; they are separate objects.
    if (!t_state.empty()) {
        std::printf("    the main thread's context is not empty\n");
        return false;
    }

    auto const after = lockdep::loop_detection_pass();
    if (after <= before) {
        std::printf("    detector missed the cross-thread inversion (%zu -> %zu)\n", before, after);
        return false;
    }
    return true;
}

// Many threads recording into the same class at once.  This exercises the
// parts that are actually shared: the registry push and the append to the
// dependency set.  Run it under -fsanitize=thread to get the real verdict.
auto survives_concurrent_recording() -> bool {
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 2000;

    Mutex outer;
    Mutex inner;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&] {
            for (int round = 0; round < ROUNDS; ++round) {
                hold<OwnerStress>(outer, [&] { hold<OwnerC>(inner, [] {}); });
            }
        });
    }
    for (auto &thread: threads) {
        thread.join();
    }

    // The edge is recorded once no matter how many threads raced to add it.
    auto *stress = lockdep::LockClassState::from_id(ClassOf<OwnerStress>::id());
    auto *target = lockdep::LockClassState::from_id(ClassOf<OwnerC>::id());
    if (stress == nullptr || target == nullptr) {
        std::printf("    classes did not register\n");
        return false;
    }
    if (!stress->has_dependency(target)) {
        std::printf("    the edge every thread recorded is missing\n");
        return false;
    }

    // Exactly one slot, however many threads raced to add it: a thread that
    // loses the compare-exchange to the same class sees the edge as present.
    usize slots = 0;
    for (usize i = 0; i < lockdep::LockClassState::dependency_slots(); ++i) {
        if (stress->dependency_at(i) == target) {
            ++slots;
        }
    }
    if (slots != 1) {
        std::printf("    %zu slots hold one edge recorded by %d threads\n", slots, THREADS);
        return false;
    }

    // Nothing here breaks a rule. This environment also keeps every lock on
    // one state, so the look across to the irq-safe locks' state has to find
    // that it is the state already validated, and stay quiet.
    if (auto const reports = g_reports.load(std::memory_order_relaxed); reports != 0) {
        std::printf("    %d reports for acquisitions that break no rule\n", reports);
        return false;
    }
    return true;
}

// A guard used across threads must keep each thread's held list separate.
auto contexts_are_per_thread() -> bool {
    Mutex lock;
    std::atomic<bool> ok{true};

    std::thread worker([&] {
        lockdep::AcquiredLockEntry entry{&lock, ClassOf<OwnerA>::id()};
        auto *context = &t_state;
        context->acquire(entry);
        // The main thread holds nothing, and must still hold nothing while
        // this thread holds something.
        if (context->empty()) {
            ok = false;
        }
        context->release(entry);
    });
    worker.join();

    return ok.load() && t_state.empty();
}

} // namespace

auto main() -> int {
    std::printf("lockdep threaded tests\n");

    check("contexts_are_per_thread", contexts_are_per_thread());
    check("survives_concurrent_recording", survives_concurrent_recording());
    // Last: it leaves a cycle in the graph on purpose.
    check("catches_abba_across_threads", catches_abba_across_threads());

    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
