# Generation Boundary for Multi-Field Atomic Updates

## Problem

The current WS `set` handler processes one field at a time under the
`runtime_control` internal mutex.  Between two consecutive `set` calls,
a reader can observe a partially-updated configuration — field A has
the new value, field B still has the old value.

This is not a bug when fields are independent, but it becomes one when
introducing coupled fields (e.g., a rate limit pair `pps_rate` +
`pps_burst` that must change together to avoid transient starvation).

## Why a generation counter on plain fields does not work

A naive protocol that uses a generation counter to protect **plain**
(non-atomic) fields is fundamentally unsound in the C11 memory model:

```c
/* Writer (holds mutex — writes plain fields) */
bconf->pps_rate   = new_pps;        /* plain (non-atomic) write */
bconf->pps_burst  = new_burst;      /* plain (non-atomic) write */
atomic_store(&generation, gen + 2, release);
```

```c
/* Reader (lock-free — reads plain fields) */
do {
    gen1 = atomic_load(&generation, acquire);
    read_fa = bconf->pps_rate;      /* plain (non-atomic) read — DATA RACE */
    read_fb = bconf->pps_burst;     /* plain (non-atomic) read — DATA RACE */
    gen2 = atomic_load(&generation, acquire);
} while (gen1 != gen2);
```

Even though `generation` is atomic, the fields `pps_rate` and
`pps_burst` are plain memory.  The C11 standard classifies a
concurrent plain read and plain write on the same object as a *data
race*, which is undefined behavior regardless of any surrounding
atomic operations (§5.1.2.4/25).  TSan will flag this — correctly.

A generation counter can coordinate access ordering between threads,
but it cannot retroactively make non-atomic memory accesses safe.

## Correct approaches

### Approach A: Mutex-protected reads (recommended for get handler)

The `get` handler already holds the `runtime_control` internal mutex,
which serialises it against the `set` writer.  Under the mutex, both
the writer and reader access plain fields safely — no data race, no
generation counter needed.

```c
/* Writer: set handler */
runtime_control_lock(&bconf->runtime);
bconf->pps_rate = new_pps;
bconf->pps_burst = new_burst;
runtime_control_unlock(&bconf->runtime);

/* Reader: get handler */
runtime_control_lock(&bconf->runtime);
read_fa = bconf->pps_rate;
read_fb = bconf->pps_burst;
runtime_control_unlock(&bconf->runtime);
```

This is correct, simple, and already the current behaviour.  When a
multi-field batch `set` command is added, the writer holds the mutex
across the entire batch, and the `get` handler sees all-or-nothing.

### Approach B: Atomic-shadow-only reads (for hot path)

The worker TX loop reads individual atomic shadow fields with relaxed
ordering:

```
pps = runtime_control_load_pps_rate(ctrl);   // relaxed atomic read
```

Each shadow is an independent `_Atomic` variable owned by
`runtime_control`; the plain mirrors remain in `bless_conf`.  Production
publish and load accessors both use `memory_order_relaxed`.  This is
sufficient for the current single-field hot path because it only needs
an indivisible scalar value and does not use that value to publish or
order any other memory.  The worker never reads the corresponding plain
field, so it cannot race with the mutex-protected plain-field write.

If a future feature requires the hot path to read *multiple* coupled
fields consistently, it cannot read plain fields lock-free (data
race).  The correct lock-free approach is one of:

- **Atomic snapshot pointer**: the writer builds a complete immutable
  config snapshot, then publishes it with a release store to an atomic
  pointer. Readers acquire-load that pointer and read one coherent
  object. The implementation must also keep the referenced snapshot
  alive until every reader that could have loaded it has finished.

- **Fall back to the mutex**: accept that multi-field coupled reads
  require the mutex, same as the `get` handler.  The hot-path reader
  is single-field by design; if it ever needs multiple coupled
  fields, that code path is no longer "hot" enough to justify
  lock-freedom.

### Why a generation counter on plain fields is still a data race

A common misconception is that a seqlock-style retry loop (read
generation, read fields, re-read generation, retry on mismatch) makes
plain-field access safe.  It does not.

Consider the writer-core's store buffer and the reader-core's cache:

```
Writer (core 0):                  Reader (core 1):
  bconf->pps_rate = NEW;             gen1 = load(generation);    // old
  atomic_store(&gen, gen+2, rel);    fa = bconf->pps_rate;       // ???
```

The `release` store on `generation` orders the writer's prior stores
(including `bconf->pps_rate = NEW`) before the generation store — so
on core 0's side, the plain write "happens before" the generation
bump in C11 terms.  However, the reader's plain read on core 1 has no
corresponding atomic operation to establish a *synchronizes-with*
edge with the writer's release.  An `acquire` load that reads the new
generation value would synchronize-with the writer's release store,
establishing a happens-before between the plain write and the
subsequent code — but the reader's *first* generation load reads the
*old* value, not the new one, so no synchronizes-with edge is created
for the interleaved plain read.

The result: the compiler and hardware are free to reorder, cache, or
tear the plain read.  In practice, on x86-64 with TSO, this may
appear to work; on ARM64 with a weaker memory model, it can fail
silently.  TSan reports the data race regardless of architecture.

## Recommended implementation path

1. **Keep the mutex-protected get path as-is** — it is already correct
   for multi-field consistency.

2. **Keep atomic-shadow single-field hot-path reads as-is** — they are
   already correct for independent fields.

3. **When multi-field batch set is added**, the writer extends its
   mutex critical section to cover all fields in the batch.  The get
   handler sees a consistent batch automatically.  No generation
   counter needed.

4. **If a lock-free multi-field reader is ever required** (e.g., a
   stats snapshot that cannot block on the mutex), implement an
   atomic snapshot pointer rather than a generation counter on plain
   fields:

   ```c
   const struct config_snapshot * _Atomic snapshot;

   /* Writer publishes a fully initialized immutable object. */
   atomic_store_explicit(&snapshot, next, memory_order_release);

   /* Reader acquires one coherent object. */
   const struct config_snapshot *current =
       atomic_load_explicit(&snapshot, memory_order_acquire);
   ```

   Pointer publication alone is not a reclamation protocol. Before
   replacing and freeing an old snapshot, choose an explicit lifetime
   scheme: retain snapshots for process lifetime, use DPDK QSBR/RCU, or
   use a two-slot pin/retry guard analogous to `stats_guard`.

## Alternatives considered

- **Odd/even generation counter (seqlock) on plain fields**: rejected.
  Reads of plain fields concurrent with writes are data races
  regardless of generation checks.  C11 §5.1.2.4/25.

- **Coarser lock**: hold the mutex across the entire batch.  Accepted —
  the get handler already uses this approach.

- **Copy-on-write config (atomic snapshot pointer)**: accepted as the
  fallback for lock-free multi-field reads only when paired with an
  explicit reader-lifetime/reclamation protocol.

## Acceptance criteria

- The get handler continues to read plain fields under the mutex
  (no change needed — already correct).
- The hot path continues to read individual atomic shadows
  (no change needed — already correct).
- Multi-field batch `set` holds the mutex across the batch
  (to be implemented with the batch command).
- If lock-free multi-field reads are ever added, they use an atomic
  snapshot pointer plus a documented lifetime/reclamation protocol,
  not a generation counter on plain fields.
- This document reflects the correct C11 concurrency model;
  no generation counter is added to the codebase.
