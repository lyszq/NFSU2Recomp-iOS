# DS4 gameplay rumble correction - v0.27.2 / build 37

The user confirms build 36's settings vibration test works on their DS4, while
gameplay is silent. This confirms the basic controller output path, not the
gameplay producer. Build 36's successful CI did not establish physical rumble.

## Findings and changes

- The old producer intercepts only `0x00406460`, the native Force Feedback path.
  Normal HumanDriver update `0x0041e2c0` can skip that nested branch. The new
  wrapper preserves its entry driver pointer, calls the original once, and then
  samples `driver+0x2a8 -> Vehicle*`, without native FF device/enable gates.
  The legacy FF entry remains for other driver types. Static evidence establishes
  the conditional path; it does not prove which gate was set on the user's iPad.
- The previous `ff-0x690` collision read assumes DragDriver. Ordinary HumanDriver
  embeds FF at `+0x220`. This speculative driver collision field was removed.
- Collision body ownership is `vehicle+0x2c -> physics+0x6f8 -> body`, then
  `body+0x1e0` is compared with the simulation tick. The old code incorrectly
  started at `vehicle+0x6f8`.
- Duplicate suppression now includes body contact, ground flags, surface and slip.
  An unchanged sample refreshes continuous output, and only distinct simulation
  ticks count toward airborne duration. Changing cars resets previous state.
- Bridge messages are packed into single atomic words. Consuming one impact
  cannot consume the next impact's intensity with the preceding ID/duration.
  Continuous TTL changes use compare/exchange against the complete message.
  Nonfinite inputs are rejected. Impact sequence wraps modulo 2^27.
- Swift uses finite 300 ms continuous patterns, renewed at 200 ms, with direct
  intensity/sharpness values. Temporary pause cancels players without an
  asynchronous engine stop/start race. Background/view teardown cancels rumble;
  stale queued requests expire. Failures and recovery are visible in settings.
- Settings include producer, bridge and accepted-playback counters. A successful
  Core Haptics API call is not proof that a physical motor actuated.
- The link verifier now requires both hooks, their generated originals, and real
  bridge symbols. The authoritative override recipe includes `0041e2c0`.

## Evidence boundary

The private original's normal driver vtable is `0x784fc8` (update entry +4);
DragDriver uses `0x785118` (update `0x004220a0`). Constructor/update analysis
establishes the vehicle and FF ownership. Original FF reads vehicle velocity,
wheel contact, material and slip at the same offsets used by this sampler.
No original executable, lifted source or other game data is included here.

Local validation on Windows:

- 261 authored Python contracts passed.
- 15 input-bridge tests passed, including 100,000 concurrent reports with
  correlated intensity/sharpness/duration and nonfinite input rejection.
- Three synthetic production-sampler scenarios passed: native FF disabled with
  original CPU/stack results preserved; unchanged-velocity same-tick body/contact
  transitions; surface/slip/landing and AI/pause/replay/race isolation.
- ARM64 cross-compilation passed for overrides, the changed generated shard and
  stubs using Zig's Darwin target and an iOS selection header. This is not an
  Apple SDK build or a full-runtime link.

Apple CI / IPA / physical acceptance: pending when this change was prepared.

## Device acceptance

Install the newly signed build 37, keep vibration enabled, and test a wall impact,
wall scrape, rough ground, drift and a landing. Verify pause/background/disconnect
stop the effect. If any effect is silent, open Controller Settings > Titreşim
tanısı and record the producer, bridge and playback values plus any visible error.
The old build 36 IPA does not contain these corrections.
