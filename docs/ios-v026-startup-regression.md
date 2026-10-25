# v0.26/build33 startup regression — 2026-10-25

The owner reports a black screen after Run, without a loading screen.
The v0.26 package passed build checks but failed this device acceptance attempt.

## Private input and observations

The supplied 13,632-byte NFSU2Runtime.log has SHA-256
`1b6c88cc25999c0e5665909f5370c1ed3c1234538eb35f57f3a164891f220088`.
Both sessions identify v0.26.0/build33, source
`0c1218a9044e456f798d2a4b4ce421ef76e8819d`. The second observation spans
10.454 seconds before backgrounding. The raw device log remains private.

- One committed empty frame; zero triangles and zero main GPU/CPU draws.
- Zero VS libraries or PSO lookups, so no vertex shader execution is evidenced.
- One 256-byte upload; zero ReadFile calls and no later Presents.
- No Present/host wait, dropped frame or GPU-ring-busy evidence.
- 24,170 guest acquisitions, three queued entries, zero fair handoffs.
  Acquisitions include lifecycle/self reacquisitions; queued entries include
  handoff sleepers. These values do not establish distinct-thread counts.
- Five audio buffers register (617,344 mirrored bytes). Two writes publish
  52,416 bytes with zero rejections. Audio callbacks run 1,294 times, maximum
  duration 23 microseconds, without late/over-budget callbacks.

## Confirmed source defect

The v0.26 iOS import path polls for handoff every 128 imports, requiring a
continuous two-millisecond guest-lock hold (`now - tl_gil_t0`). But its lifecycle
checkpoint releases/reacquires that mutex every 1024 imports, calling
`gil_took()` and resetting the same eligibility clock. A fast import loop can
finish each 1024-call interval below two milliseconds, preventing explicit
handoff indefinitely while a worker remains queued. The lifecycle path also
removed v0.25's `sched_yield()`.

Ordinary unlock/relock does not establish that a queued peer acquired the mutex:
selection follows the host [scheduling policy](https://pubs.opengroup.org/onlinepubs/009604499/functions/pthread_mutex_lock.html).
This proves a source-level lost progress opportunity. The device log fits it,
but contains no exact spinning guest stack; this is not proof of the sole cause.

## Targeted correction: v0.26.1/build34

`guest_poll_handoff` uses a separate per-thread monotonic timestamp, updated at
handoff-attempt completion. Lifecycle/ordinary reacquisition no longer resets
eligibility. The first queued poll attempts promptly, with later attempts
throttled from completion by two milliseconds. The existing single absolute
two-millisecond condvar deadline, ownership/critical-section guards, generation
predicate, guest CPU restoration and bounded retry policy remain.

The 1024-import boundary again calls `sched_yield()` outside the guest mutex
before the lifecycle checkpoint, restoring v0.25's host-yield ordering.
No sleep, guest-clock change, global mute or GPUVS rollback was introduced.
Desktop scheduling and the other v0.26 optimizations are retained.

Independent scheduling and audio reviews found no concrete blocker in this
patch. No local native tests or device execution were run; `git diff --check`
checks source formatting. Existing CI/build/package gates remain necessary.

## Other leads and limits

The missing `foobar` and `NFSUNDER/ZDIR.BIN` opens do not prove incomplete game
data. The supplied game ZIP has no ZDIR or NFSUNDER entry, and no authored guest
fallback establishes whether that probe is mandatory. Do not request reimport
or remove user game/save data based only on these ENOENT results. The original
renderer submits an empty frame without an evidenced renderer wait/ring stall.

The new device run must progress past the empty frame into guest IO, repeated
Presents and scene draws. The fair-handoff counter identifies explicit peer
acquisitions; its wait time includes deliberate handoff sleep and is not pure
mutex contention. Startup recovery, sound quality and 60 FPS remain device
acceptance items. Raw logs, game data, generated shards and IPA remain private.
