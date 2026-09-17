# lockdep

Lock ordering validation for the ours kernel: a header-only library that records
which locks are held when another is taken, and reports orderings that can
deadlock.

## What it does

Every lock type belongs to a **lock class**. When a context takes a lock of
class `B` while already holding one of class `A`, lockdep records the edge
`A -> B`. A cycle in that graph is an ordering that two contexts can run in
opposite directions, which deadlocks as soon as they interleave badly — whether
or not it has ever actually deadlocked.

Work is split by cost:

- **On every acquisition** (`LockContext::validate_acquire`) lockdep records the
  edges and checks what is cheap to see from there: a direct inversion of an
  ordering already recorded, a class acquired twice without `Nestable`, and
  anything taken while a `Leaf` lock is held. This is O(locks currently held)
  and allocates nothing.
- **On demand** (`detect_lock_order_cycles()`) it walks the whole graph with
  Tarjan's algorithm and reports every cycle, including transitive ones like
  `A -> B -> C -> A` that no single acquisition can see. This is
  O(classes + edges) and belongs on an idle thread or in a test, not on the
  acquire path.

## Using it

```cpp
#include <ours/lockdep.hpp>

// Interrupts really are disabled for as long as the guard is alive: the tag
// selects the saver the lock declared, and that saver is a member of the guard.
Guard<SpinLock, IrqSave> guard{lock};
```

A lock type opts into traits and state savers next to its own declaration:

```cpp
LOCK_DEP_TRAITS(MyLock, lockdep::ValidationControl::IrqSafe);
LOCKDEP_STATE_SAVER(MyLock, lockdep::SavedStateOption::IrqSave,
                    arch::AutoInterruptDisabler);
```

Traits are optional: a lock that declares none gets `ValidationControl::None`.
A saver is not: asking a `Guard` for an option the lock declares no saver for
does not compile, because the only other answer -- save nothing -- is the bug
that left every `Guard<SpinLock, IrqSave>` running with interrupts enabled.

## Feature levels

The level comes from the build, never from the source. `config("lockdep")` in
`//kernel/products/BUILD.gn` turns the `lockdep_*` GN arguments into
`CONFIG_LOCK_DEP` and `LOCK_DEP_ENABLED_FEATURE_LEVEL`:

| `lockdep_enable` | Level | What is compiled in |
| --- | --- | --- |
| unset | `Disabled` | Nothing. `Guard` is the lock reference and a flag; `LockContext` is empty. |
| `lockdep_metadata_only` | `MetadataOnly` | Lock classes and their traits, no tracking. |
| `true` | `Validation` | The graph, the acquire-time checks, and the detector. |

At `Disabled` every lockdep member vanishes — `Guard` costs no more than a hand
written scope guard, and `Thread` carries no lock context.

## What the environment must supply

Two functions, referenced only when validation is enabled, so a build with
`lockdep_enable=false` links without them:

- `lockdep::current_lock_context()` — the lock context of the running execution
  context, or `nullptr` before there is one.
- `lockdep::report_violation()` — how this kernel logs or panics.

## Tests

```sh
tests/run_tests.py                    # everything
tests/run_tests.py --suite smoke      # logic, at all three feature levels
tests/run_tests.py --suite threaded   # real threads, plain and under the sanitizers
```

The headers lean on the ours tree -- the force-included prelude for `usize`,
ustl, and the thread-safety macros in `system/public` -- so the script finds
the ours checkout above this library, or takes one with `--ours`.

Lock validation turns out to be well suited to a host test, because everything
lockdep concludes is a property of a graph, and a graph does not care whether
the locks that built it were spinlocks or `std::mutex`. `tests/smoke.cpp` covers
that logic at every feature level with `-Wall -Wextra -Werror`, using fake locks:
that a consistent order stays clean, that an inversion is caught at acquire time,
that a three-node cycle is invisible to the acquire path but found by the
detector, that repeated passes agree, and that a guard costs nothing extra when
validation is compiled out.

`tests/threaded.cpp` drives it the way a kernel does — one `LockContext` per
thread, real locks, several threads recording into the shared graph at once —
and checks that two threads taking the same pair of locks in opposite orders are
reported even though the run never actually deadlocks. Its ThreadSanitizer
variant is what says the lock-free registry and the dependency set are safe to
record into from several CPUs.

What no host test can cover is the arch integration: interrupt and preemption
state savers have no userspace meaning, so `Guard<SpinLock, IrqSave>` is only
checked here for saver *ordering* against fakes, never against real hardware
state.

## Known limits

- A lock class is currently keyed on the owner type passed to `LockClass`, and
  a bare `Guard{lock}` falls back to `DefaultLockClass<Lock>`, which is shared
  by every lock of that type. Until call sites declare their locks through an
  instrumenting wrapper, two different spinlocks look like one class to the
  validator. The lookup is behind `DefaultLockClass` so that change stays local.
- `LOCKDEP_MAX_DEPENDENCIES` (default 32) bounds the successors one class can
  record. Overflow is reported and costs completeness, not soundness.
- `ValidationControl::IrqSafe`, `SingletonLock` and `ActiveListDisabled` are
  carried on the class but not yet acted on.
- Only one detector pass may run at a time; the walk keeps its marks in the
  graph nodes.
