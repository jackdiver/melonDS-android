# Netplay (lockstep) plan for melonDS-android

Goal: two Android devices on the same Wi-Fi play DS local-wireless games together
(first target: Mario Kart DS multi-card), tolerating 10–120 ms of Wi-Fi delay.

## Why not LAN mode

Tested on Galaxy S23 Ultra (Android 17) + Odin 2 Portal (Android 13), Mario Kart DS:

- The LAN link works and the client sees the host's group list.
- The join fails: the client's single join frame reaches the host 10–120 ms late,
  outside the host's short emulated RX window, so it is never read
  (`LAN.cpp` stale-drop logs: ages ~133 ms). Raising timeouts/stale limits did not help.
- DS wireless expects replies in microseconds. LAN mode only works on near-zero-latency links.

## Approach: mirror lockstep

Each device runs **every** player's console:

- Instance 0 = my console (shown, audio on). Instance 1 = mirror of the other player's console (hidden, muted).
- The two instances on one device talk through an **in-process, deterministic** wireless link, so the DS radio timing is exact.
- Only **button/touch inputs** go over the network, tagged with a frame number and applied a few frames late (input delay, ~4 frames ≈ 67 ms).
- Both devices run identical emulation, so they stay in sync. A periodic state hash detects desync.

Network delay then only adds input lag. It no longer breaks the wireless protocol.

## Starting point (what exists)

- `melonDS-android-lib/src/net/Netplay.cpp`: an **unfinished upstream prototype** (mostly `#if 0`; desktop menu entries are commented out). Useful as a sketch only: mirror instances, input frames, blob transfer.
- `net/LocalMP.cpp`: in-process multi-instance wireless, but it uses **wall-clock timeouts** (`Semaphore_TryWait(..., RecvTimeout)`) → not deterministic across devices.
- `app/src/main/cpp/MelonInstance.*`: already has `instanceId`, `saveState`/`loadState`, and `RewindManager`.
- `MelonDS.cpp` holds a single global `instance`. The emulation loop is in `MelonDSAndroidJNI.cpp` (~line 618).
- Input is applied immediately from the UI thread (`pressKey`/`touchScreen` → `nds->SetKeyMask`), so it is not frame-aligned.
- Savestates include cart SRAM (`NDSCart.cpp` `DoSavestate`).

## Phases (each ends with a test gate)

### Phase 0: Clean up the LAN experiment (≈0.5 day)
- Revert experiments in `LAN.cpp` (100 ms recv timeout, 120 ms stale limit, debug logging).
- Keep: `ACCESS_LOCAL_NETWORK` in the manifest, plus a runtime permission request (Android 17+).
- Keep the JNI host/join plumbing only if LAN mode stays as an option for wired/very-low-latency links. Remove the debug broadcast receiver.
- **Gate:** clean build; LAN link still connects.

### Phase 1: Determinism check (≈1–2 days) — de-risks everything
- Frame-aligned input: buffer UI input and apply it once per frame, before `runFrame()`.
- Input recorder/replayer + per-frame state hash (hash of a savestate, or of main RAM + CPU regs).
- Netplay config profile forced on both devices: FreeBIOS + generated firmware, fixed MAC, fixed RTC start time, same JIT settings, no cheats, no RetroAchievements.
- **Gate:** replay the same 10-minute input recording on S23 and Odin and get identical hashes every frame. Test JIT on and off. If JIT diverges, netplay forces the interpreter.

#### Phase 1 results (2026-11-11) — gate passed
Tool: `app/src/main/cpp/netplay/DeterminismHarness.*` (debug broadcast `me.magnum.melonds.NETPLAY_TEST`,
cmd = record | replay | stop | selftest). Hash = main RAM + shared/ARM7 WRAM + ARM9/ARM7 registers.

- Interpreter, same device, rewind and re-run each frame: 300/300 frames identical.
- JIT, same device, rewind and re-run: every frame differs (a warm JIT cache vs a cold one changes timing).
- Interpreter, S23 recording vs S23 replay vs Odin replay (2,738 frames): all identical.
- JIT, S23 replay vs Odin replay, both starting cold from the same snapshot: all 2,738 identical.
- The interpreter runs Mario Kart DS at only ~36 fps on the S23, so netplay must use the JIT.

Rules learned for netplay:
1. Every player, the host included, must load the same snapshot at session start, so all JIT caches start cold.
   Any resync must reload the snapshot on all sides.
2. Same BIOS (built-in FreeBIOS), same generated firmware, including the MAC and `wfcsettings.bin`
   (each install stores its own Wi-Fi settings; the host must send its copy), same JIT setting.
3. Input must be frame-aligned (done: `MelonInstance::applyFrameInput`), mic fed silence, no cheats.
4. Do not call `MelonInstance::loadState` for netplay: it resets the RTC to the device clock. Load via `NDS::DoSavestate`.

### Phase 2: Two consoles on one phone (≈3–5 days) — hardest part
- Support 2 `MelonInstance`s at once: per-instance threads, audio only from instance 0, rendering/presentation only for instance 0 (mirror uses the software renderer or skips presentation).
- New `DeterministicLocalMP` (an `MPInterface`): no wall-clock timeouts. Waits are ordered by emulated timestamp, so both instances exchange identical frames at identical emulated times. Needs deadlock handling when one instance is not in MP mode.
- **Gate:** Mario Kart DS multi-card race between instance 0 and instance 1 on ONE phone (mirror driven by recorded input), and the same input recording gives identical hashes on both devices.

#### Phase 2 progress (2026-11-11) — two consoles race on one phone
Code: `netplay/DeterministicMP.*` (deterministic in-process wireless link), `netplay/MirrorSession.*`
(hidden second console on its own thread, lockstep frames, focus switch), `MelonInstance::cloneFrom/runFrameHeadless`,
`MPInterface::SetCustom` (small core change). Debug broadcast cmds: `mirror-start`, `mirror-stop`, `focus --ei index 0|1`.

- Works on the S23: console 1 sees console 0's group, joins (2/8 racers), both go through game select,
  character select and settings, and a VS race starts with both karts visible to each other.
- Bugs found and fixed on the way: the mirror was never powered on (`NDS::Start`); the clone must follow the normal
  launch path (`Reset` + `SetupDirectBoot` + `Start`, then load the state) because direct boot sets up state that
  savestates do not carry; JIT fast memory is process-wide, so it is turned off for every console in a session.
- Requires the software renderer (enforced) so 3D output is identical on every device.

Update (2026-11-11, later): two-console record/replay added (`mirror-record`, `mirror-replay`, `mirror-recstop`).
- With JIT on (fast memory off) the pair runs ~55 fps on the S23.
- Odin replay matched the S23 recording for 5,105 frames (through the menus); S23 replays differ from frame 0
  by 8 bytes of main RAM (a 3D fixed-point vector at 0x0216FF98), varying per run. Not threaded rendering, not
  the GBA slot, not the mic, not the link. Reset-before-load did not fix it. Next: find which unsaved state differs
  (dump GPU3D/registers on both phones at frame 0).
- Link made time-based (reply rounds, membership, host-left check). Debug RAM dump code is temporary.

Still open in phase 2:
1. Speed: with the interpreter the pair ran at ~20 fps. Must re-test with the JIT on (fast memory off).
2. Cross-device gate: extend the harness to record/replay both consoles' inputs and hash both, then compare S23 vs Odin.
3. Remove the temporary progress logging in `MirrorSession::runFrame`.

### Phase 3: Network lockstep (≈2–3 days)
- Session protocol over ENet (already linked): host/join, ROM hash check, netplay config sync.
- Initial sync: host sends its savestate (includes SRAM) to the client. The client never writes this session to its own save file.
- Per-frame input packets with frame numbers, a configurable input delay, stall when remote input is missing, catch-up after stalls.
- Desync detection: compare state hashes every N frames. Resync = host re-sends a savestate.
- **Gate:** S23 + Odin finish a 3-race Mario Kart cup with no desync, on the home Wi-Fi.

### Phase 4: App integration (≈2–4 days)
- Host/Join screen: LAN discovery (UDP broadcast) + manual IP. Game must match.
- Runtime `ACCESS_LOCAL_NETWORK` request; bind sockets to the Wi-Fi network so a VPN does not swallow the traffic (seen on the Odin).
- Low-latency Wi-Fi lock during sessions; pause/background handling (pausing one side stalls both, with a clear message).
- Performance: two consoles per device; check frame time on the Odin 2 Portal.
- **Gate:** start a session from the UI alone, no adb.

### Phase 5: Polish (ongoing)
- More games (Mario Party DS, Pokémon trades/battles, TrackMania DS).
- 3–4 players (more mirror instances).
- Optional later: rollback to hide input delay (RewindManager already snapshots state).

## Main risks
1. **Cross-device determinism** (JIT, renderer readback, RTC/MAC, uninitialised state). Phase 1 answers this early.
2. **Deterministic in-process MP** without real-time timeouts. This is likely why upstream stopped. Phase 2 is the go/no-go.
3. **CPU cost** of two consoles per device (both test devices are Snapdragon 8 Gen 2, so probably fine).
4. Upstream drift: keep changes in the core small and isolated so they can be rebased onto new melonDS versions.

## Rough size
About 2–3 weeks of focused work. Phases 1 and 2 decide whether it is feasible; stop and reassess after each gate.

### S23 JIT divergence: checkpoint result
Session replay (JIT on) on the S23 vs the Odin, console 0 main RAM diffed at frames 0, 1000 and 5000:
8 bytes differ at every checkpoint, always at 0x0216FF99-A6 and nowhere else. The difference never spreads,
so it is treated as dead memory the game never reads. Desync checks should skip this range (or hash
gameplay state only) until the root cause in the JIT is found.

**Correction (longer run):** skipping those bytes is not enough. A full 8509-frame replay with the skip still
diverges: brief one-frame mismatches (frames 573, 4089, 4833), then a lasting split from frame 5409.
Dumps at those frames show:
- 573: only the known stray bytes, plus ARM9 R12 (S23 0x07FFBB50, Odin 0). The stray stack bytes hold the
  same 0x07FFBB50, so they are pushed host-dependent register values, not dead memory.
- 5408/5409: real game state differs: main RAM 0x021E743C and 0x023FFF98, and a group of ARM7 WRAM
  counters (offsets 0xAEB8-0xBA1A).
Next step: find the ARM9 JIT path that leaves R12 holding a host-dependent value on the S23 (Android 17)
but not on the Odin (Android 13), e.g. compare JIT vs interpreter R12 per block around frame 573.
Debug dumps currently fire at recFrame 573/5408/5409 (MirrorSession.cpp).

### Repeatability findings (two-console session, full 8509-frame replays)
- Running the two consoles concurrently (worker thread) is **not repeatable on either phone**: the same
  phone gives different hashes run to run (Odin: from frame 4934/5967/6946 in different runs; S23 similar).
  There is a timing race between the console threads (likely in DeterministicMP or shared state).
- Running them **one after the other (serial)** is repeatable: two Odin runs matched for all 8509 frames,
  and S23 vs Odin matched everywhere except 3 single frames (4089, 4180, 4833) that line up again the
  next frame. Serial is now the default (debug flag 32 = old concurrent mode). Cost: ~10% slower.
- Those S23-only one-frame blips are the remaining JIT/host-state issue (the earlier stray stack bytes at
  0x0216FF98 no longer appear with the current build).
- Test tooling: replays now skip the frame limiter and write `done-<tag>.txt` when finished; a frame-0..2
  per-block ARM9 trace hook (`melonDS::JitTraceHook9`) exists for debugging.

### S23 one-frame blips: what we know (investigation parked)
- The blips are not dead memory. Game code at 0x02146C24 (ARM9, IRQ mode) reloads R12 from a saved CPU
  context at 0x0216FF98. On some runs that context holds a junk value (seen: 0x07FFBB50, 0x0009A000, 0x666)
  and the other device holds zeros. It is not always the S23 that has the junk.
- Everything the savestate covers is identical after loading, so the junk most likely comes from state that
  neither Reset nor the savestate covers and that leaks in from what the console ran before the snapshot was
  loaded (or from a JIT path that writes a host value into a guest register).
- The state hash now covers all of main RAM again (no skip), plus DTCM (ARM9 stack) and banked registers.
- Tooling left in place: reference-driven capture (push another device's `hashes-ref.txt` or a
  `dumpframes.txt` into files/netplay-test; opt-in, ~8 fps while active).
- Mitigation for Phase 3: desync detection by periodic hash exchange plus resync (host sends a full state).
  When starting a session, both sides should load the snapshot into a freshly created console, not one that
  ran something else first.

#### Phase 3 progress (2026-11-12) — first network session works
- Debug commands: `net-host --ei delay N` (S23), `net-join --es ip 192.168.0.4 --ei delay N` (Odin), `net-stop`,
  after `mirror-start` on both. Host = console 0, joiner = console 1 (joiner's screen follows console 1).
- Start-up: host snapshots both consoles, reloads them fresh, sends both states zlib-compressed
  (2 x 19.5 MB raw -> 4.9 MB); joiner loads them fresh. Inputs go unreliable with 16-frame redundancy, input
  delay 4 frames; state hashes are compared every 60 frames.
- First idle run over home Wi-Fi: connected, synced, ran ~1800 frames; all hash checks after frame 0 in sync.
  One mismatch at frame 0 (console 0) that did not persist. The S23 is the slower side (~55 fps with two
  consoles), so the Odin waits ~25% of the time.
- Not done yet: resync on desync, playing a real cup (gate), UI.

#### Speed work (2026-11-12)
- Debug builds compiled the core at -O0. Build with `-PoptimizedNative` (added to app/build.gradle.kts) to get
  an optimised core in debug builds: replays went from ~285 s to ~85 s (8509 frames, ~100 fps with both
  consoles in parallel). Netplay title screen ~49 fps with the consoles taking turns.
- Frame time breakdown (S23, title screen, serial): ~8 ms per console, network wait negligible. Running the
  consoles in parallel is the main remaining speed win.
- Fixed three timing races in parallel mode: late MP replies counted, late host packets counted, and a console
  briefly looking "parked" at the previous frame end when a frame started (the deadlock rule released the other
  console early). With these, the Odin's parallel replays are repeatable for all 8509 frames.
- Still open: on the S23, parallel replays differ from frame 0 on both consoles (serial is fine). Something
  S23-specific remains racy; capture mode (slow) hides it. Debug flag 64 (no-op hook between JIT blocks) did
  not change it, so it is not the JIT trace hook effect.

#### Root cause of the "junk value" blips found (2026-11-12)
- melonDS did not save (or reset) the hardware divider and square-root unit values (DivNumerator/Denominator/
  Quotient/Remainder, SqrtVal, SqrtRes). Mario Kart DS saves/restores them on thread switches, so after a state
  load they leaked whatever the console computed before. Reproduced reliably by playing 20 s before the replay.
  Fixed in the core: saved from savestate version 13.1 and cleared on reset.
- Result: S23 and Odin match on all 8509 frames with both consoles in parallel (full RAM + DTCM + banked
  registers hashed); each phone is also repeatable run to run.
- Consoles now run in parallel by default, netplay included (debug flag 2 = serial). Netplay title screen:
  ~10-11 ms per frame on both phones (well under the 16.7 ms budget), 0 desyncs.

#### First played cup (2026-11-12)
- ~10 minutes / 35,000+ frames of a played Grand Prix over home Wi-Fi: 590 hash checks, 0 desyncs.
- Speed: ~19 ms per frame on both phones during races (~52 fps). On the S23 the two consoles take ~9.4 ms
  each and do not overlap (mirror wait ~9.3 ms after the primary), so during wireless play they effectively run
  one after the other: the in-process link makes them wait on each other packet by packet.
- Latency: 4 frames of input delay at ~52 fps plus Wi-Fi felt too high.
- Freeze after the cup: both phones froze identically (still in sync, emulation running, ARM9 in its idle loop),
  so the emulated wireless session hung, not the netplay sync. Needs investigation.

#### Lag and speed (2026-11-12)
- During wireless play the two consoles wait on each other ~5 / ~33 times per frame (one MP exchange each);
  that ordering is inherent to the protocol, so they can overlap only a little. Spinning before sleeping did
  not change it (kept: harmless, results identical).
- Threaded 3D rendering is deterministic after the divider/sqrt fix (replays identical to non-threaded and
  S23 = Odin on all 8509 frames). It is the default again: race frames ~18 ms -> ~10 ms on the S23.
- Default input delay lowered from 4 to 2 frames (~33 ms).
