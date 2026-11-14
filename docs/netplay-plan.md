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
- Played session with the Odin hosting (2026-11-12): ~10 min / 34,700 frames, 580 checks, 0 desyncs;
  ~8-9 ms per frame on both phones (full 60 fps with headroom); total stall ~1.5% of the time. Lag "feels better".

#### Join failure / freeze investigation (2026-11-12)
- Every netplay session is recorded (debug builds) to files/netplay-test/netrec: start states + the inputs each
  console received, replayable with mirror-replay. The link logs forced releases and "host left" events.
- A failed join (S23 joining an Odin-hosted group) happened once; a later attempt joined fine. Replaying the
  good session with the joiner's taps shifted by -60..+60 frames (13 variants) always joined, so tap timing is
  not the trigger. Not yet reproduced; the freeze after a cup also still needs a recorded occurrence.

#### Phase 4: start from the game's own menus (2026-11-12)
- No session: the console's wireless goes to LobbyMP, which only watches. Beacons sent = the game created a group
  -> advertise on UDP 7065 and accept a joiner on 7064. Radio on without beacons = the game is searching ->
  connect to an advertised host with the same game.
- Each device sends its own console (savestate + firmware, zlib) to the other; both start a lockstep session with
  the host console as session id 0 on both devices (DeterministicMP maps local instance ids to session ids).
  Player 2 keeps its own save, nickname and MAC. When the connection ends, each device drops back to its own console.
- Fixes found while testing: searching -> hosting switch; consoles whose radio was already on at session start
  are treated as listening (otherwise the first join failed); host gives up after 15 s if the joiner's console
  never arrives; cheats off during sessions; desync snapshots written to netrec on the first desync.
- The two devices need different MAC addresses (the Odin was changed to ...:AF:57).
- Test automation: `press` debug command injects DS keys/touch inside the emulator for N frames.
- Verified on S23 + Odin: group appears in the joiner's list, first join works, sessions stay in sync.
- MAC clashes are handled automatically: the host rejects the joiner with a "MAC clash" code (keeping the connection
  open 1 s so the message arrives), the joiner picks a random 00:09:BF:xx:xx:xx MAC, patches its firmware, waits
  until the game turns its radio off and on (player reopens the multiplayer menu), and the app saves the new MAC
  (built-in firmware). Verified S23 + Odin with a forced clash (2026-11-12).
- Open: in that verified run, console 1 (the joiner) differed at the first three hash checks (frames 0-120) and was
  in sync afterwards; its firmware and start state were identical on both devices. Not seen without a clash.
- **No more "reopen the menu" on a MAC clash** (supersedes the earlier MAC note): if both consoles have the same MAC, the
  deterministic link shows each console a *different* address for the other one, by rewriting the three 802.11 address
  fields of every frame passing between them (sender's own MAC -> alias, the receiver's alias -> its real MAC). Both
  devices derive the same aliases from the shared MAC, so the result stays deterministic. The game is never restarted
  and nothing is saved. Payload-level MAC copies, if a game has any, are not translated.
- The brief early desync seen once during the clash test (console 1, frames 0-120) did not come back in a later run
  (snapshots of frames 0/60/120/180 byte-identical on both devices); that run still used the old change-the-MAC path,
  which is now removed.

#### Cleanup (2026-11-12)
- All test and debug tooling was removed from the app and the core: record/replay harness and self test, RAM dumps and
  desync/early snapshots, per-session recording, the JIT trace hook (core), debug flags and JIT experiments, frame
  timing and link statistics, the spin-wait in the link, the frame-limiter bypass, the in-emulator input injection and
  the whole `NETPLAY_TEST` broadcast receiver.
- Kept: `NetplayCommon` (input record, silent microphone flag, state hash for desync detection), the lobby, the
  deterministic link (with MAC translation), the session, and the Local multiplayer screen (manual Host / Join as a
  fallback). The `-PoptimizedNative` build switch stays: it makes debug builds ~3x faster.
- The full tooling, with the replay/diff workflow described above, is preserved at the git tag `netplay-debug-tools`
  (in the app repo and in the core library); check it out to bring it back.

#### GPU renderer experiment, control test and pause test (2026-11-13)
- Control test done: Odin on OpenGL, `debug.melonds.allowgpu 0`, S23 hosting Mario Kart -> the Odin logged no
  "looking for groups" line and showed no group (refuses, as designed). Switch on, restart game, join -> session
  running, 390 checks in sync, 0 desyncs. So the Odin really rendered with OpenGL and the VRAM hash agreed with the
  S23's software renderer. Only OpenGL was tried (not other GPU renderers).
- Pause test done: S23 pause menu -> Local multiplayer for ~35 s -> Close. Odin waited 34.7 s for input, then both carried
  on, 0 desyncs. The "Waiting for the other player" toast was NOT seen by the user (code exists in
  LocalMultiplayerController.kt, "stalled" status) -> open: find out why it does not show.
- Odin renderer restored to software, switch off. Saves verified 262144 bytes after the run; backups in
  `C:\Users\jackd\Downloads\netplay-save-backups\2026-11-13\{odin,s23}`.
- Decision (user): keep the GPU-renderer allowance and build the temporary switch (not built yet).

#### GPU renderer made permanent (2026-11-13)
- Removed `debug.melonds.allowgpu`; any renderer is allowed for a device's own console (hidden copy stays software; the
  hash covers VRAM). Settings import no longer forces the renderer or rewind (still forces built-in BIOS + DS mode).
- Rewind is blocked while a session runs (`MelonInstance::setRewindBlocked`) and restored on stop. Toast warns about
  custom BIOS / DSi mode when hosting or searching starts.
- Test: Odin OpenGL + S23 software, no property: 100 checks, 0 desyncs. Rewind blocking not exercised in a run yet.
  Odin renderer put back to software afterwards. Not committed.

#### Stalled toast and hidden-console rendering experiment (2026-11-13)
- Toast: the status path was fine; the one-shot toast lasted 3.5 s while the user was looking at the other phone. It now
  repeats every 3.5 s while stalled. Verified on the S23 (screenshot) while the Odin process was frozen with
  `run-as ... kill -STOP` (a freeze of ~14 s ends the session: expected, ENet keep-alive stops).
- Experiment (uncommitted, `adb shell setprop debug.melonds.nullmirror 1` before starting the game on both devices):
  the hidden console uses a `NullRenderer3D` that draws nothing (`MelonInstance::useNullRenderer`).
  Result in a real race, Odin + S23, software primary: 170 checks, 0 desyncs (hash covers VRAM).
  CPU over 30 s: baseline Odin 44+31+34+43 = 152% of a core, S23 159%; with the experiment Odin 112%, S23 120%
  (about 25% less total CPU; the mirror's own 3D thread disappears, the mirror thread itself barely changes).
  Risk: anything that reads 3D output back (display capture into VRAM) would differ; Mario Kart DS did not trip it.
- Made permanent (no property): the hidden console uses `MirrorRenderer3D` (MelonInstance.cpp). It draws no 3D until the
  game enables display capture (DISPCAPCNT bit 31 of engine A); then it renders in software (unthreaded) for the rest
  of the session, and a capture that begins in an unrendered frame is rendered on demand at its first scanline
  (log: "hidden console: screen capture seen"). Mario Kart DS never triggered it. Test: Odin + S23 in a race,
  150 checks, 0 desyncs. The capture fallback itself has not been exercised by any game yet.

#### Latency logging, Wi-Fi lock, more CPU experiments (2026-11-14)
- Log line every 10 s now also shows how many frames the remote input was late, the worst wait and ENet's rtt.
  ENet's rtt (~27 ms +-7 here) is only sampled with reliable packets and the link is serviced once per frame, so it
  overestimates; "input late in N of 600 frames" is the real measure. Runs: late in 0-42 of 600 frames, worst wait
  <= 11 ms with a 2-frame (33 ms) delay -> the delay has plenty of slack; try 1 frame (adaptive delay).
- `WIFI_MODE_FULL_LOW_LATENCY` WifiLock is held while the lobby/session is active (`LocalMultiplayerController`),
  released when idle. Verified held on the Odin (`dumpsys wifi`: type=4).
- Experiment flags (setprop debug.melonds.mirrorexp, since removed): 1 = hidden console skips 2D drawing, 2 = lower priority (nice 10). Result: 1 was made permanent (below), 2 dropped; details:
  drawing (engine B, and engine A unless display capture is latched; `MirrorRenderer2D`), 2 = hidden console thread at
  lower priority (nice 10). Race, Odin + S23: hidden-console thread 41-46% -> 26-30% of a core with flag 1, 0 desyncs in
  both runs. Flag 2 changed total CPU not at all; stall numbers were better in that run but it was a different run, so
  inconclusive. Not committed; flags still behind the property.
- Made permanent (2D skip, `MirrorRenderer2D`); the priority flag and the property were removed.

#### 1-frame input delay trial (2026-11-14)
- Trial via a temporary property (`debug.melonds.inputdelay`, removed again), Odin + S23 on home Wi-Fi with the
  low-latency lock, in a race: stays in sync (90 checks, 0 desyncs) but remote input was late in ~20-65% of frames
  (worst wait 55 ms), about 1 s of stall per 10 s. With a 2-frame delay it was late in 0-7% of frames.
  Conclusion: 1 frame does not work on this network (one-way ~13 ms is close to a frame, plus jitter); keep 2.
  Next ideas if wanted: Wi-Fi Direct / hotspot and re-test, or adaptive 1-3 frames chosen from measured lateness.

#### Adaptive input delay (2026-11-14)
- **Decision: not committed, code reverted.** The full change is kept as `docs/netplay-adaptive-delay.patch` (apply with `git apply`, against commit 7fd736e8).
- Each device watches how often the other device's input is late (window of 30 frames) and asks it, with a reliable
  `MsgDelay`, to publish its input 2-4 frames ahead (`netAdaptDelay`). The delay is a property of the sender only, so no
  agreement is needed: raising it fills the gap with the last input, lowering it drops the live input for a few frames;
  both devices still apply the same stream. Up: >= 6 of 30 frames late. Down: only after ~30 s (backoff doubles after a
  failed try) with 10 calm windows, one window skipped after every change.
- Tested with forced start delays (temporary property): started at 1 it climbed to 2-3 within seconds, 0 desyncs.
- First version allowed 1 frame as the floor: on this Wi-Fi the downward probes failed every time and the player felt
  hitching (user report). Floor is now 2, so the delay only goes up (poor networks) and back down to 2.
- Default session on this network: stays at 2, no changes, user reports smooth play. Not tested on a poor network.

#### Settings import dropped (2026-11-14)
- The first-launch "Import your melonDS settings?" prompt (RomListActivity, SettingsBackupManager wrapper, strings) is
  removed; those files are back to upstream. Reason: the stock melonDS 2.0.1 on the Odin has no Backup option (upstream
  flag `settings_backup_restore_enabled` = false since commit 0a991dc5, "no versioning of the backups"), so there was never
  a real backup to import. Only the hand-made backup had ever been tested.
- Consequence: the multiplayer requirements (built-in BIOS, DS mode) are no longer forced by an import; the in-game toast
  warns when a custom BIOS or DSi mode is set, and rewind is blocked live during a session.

#### Settings are not forced (2026-11-14)
- Tried forcing the built-in BIOS and DS mode at game launch (so DS games ignored a stored custom BIOS / DSi mode). Dropped
  by decision: players must be able to keep using a custom BIOS. The warning toast stays: when hosting or searching starts
  with a custom BIOS or DSi mode set it says both devices need the built-in BIOS and DS mode (change in Settings, restart).
  Rewind is blocked live during a session; the renderer is free.

#### Wi-Fi Direct experiment (2026-11-14)
Code: debug-only `app/src/debug/.../P2pTestActivity.kt` (uncommitted; forms a group with a fixed name/passphrase and pings
at 60 Hz). The native hooks used for the session test (lobby over 192.168.49.x, trial input delay) were temporary and reverted.
- **Setup**: host calls `WifiP2pManager.createGroup(config with fixed network name + passphrase, 5 GHz at the home AP's
  frequency)`; the other device calls `connect()` with the same name/passphrase. No invitation dialog, no user taps; about 1 s
  to create, 1-5 s to join. Needs the NEARBY_WIFI_DEVICES runtime permission (location below Android 13). Both phones stayed
  on the home Wi-Fi at the same time (STA + P2P concurrency worked on both).
- **Compatibility**: S23 (Android 17) joined the Odin's group fine. The Odin (Android 13, Qualcomm) could NOT join the S23's group
  by credentials ("P2P: Failed to find the group with network name ...", also with a frequency hint and a scan first), but hosts fine.
  So who hosts matters, and joining by credentials is not safe everywhere; fallbacks (service discovery, invitation dialog) untested.
- **Lobby**: the game lobby's 255.255.255.255 broadcast does not use the p2p interface. Over Wi-Fi Direct the adverts had to be sent
  to 192.168.49.255 plus both group addresses; then the group showed in the join list and a session formed (in sync, 150 checks).
- **Latency** (60 packets/s UDP, 45 s, screens on, low-latency lock): LAN (home Wi-Fi) rtt p50 10.8-11.9 ms, p90 13.5-14.7, p99 17.7-19.1,
  p99.9 25-41, max 33-71, 2-4% of packets above 16.7 ms. Wi-Fi Direct: p50 7.0, p90 10.5-10.6, p99 13.3-13.9, p99.9 17-18, max 20-25,
  0.2% above 16.7 ms, no loss. About 4 ms lower median and far fewer spikes. (First LAN run was invalid: the S23 screen was off and
  Android throttled the app; always keep the screen on.)
- **1-frame delay over Wi-Fi Direct (real session)**: still late input in 25% (S23) to 75-90% (Odin) of frames, ~22% of the time lost
  to stalls. So the transport is no longer the limit: with a 1-frame buffer the other device's input must arrive within 16.7 ms of
  its own frame start, and the two devices' frame clocks are not phase-aligned (offset up to a full frame), so it is late about
  half the time on any network. Using 1 frame would need phase-locking the frame starts (follower starts its frame when the
  leader's input arrives), not a faster link.

#### Status summary (2026-11-14)
- Committed up to `0f53d48e`; nothing pushed. Netplay works between the S23 and the Odin on a shared Wi-Fi network with a
  2-frame input delay, any video renderer, no settings import, custom BIOS allowed (with a warning toast).
- Performance: the hidden console draws neither 3D nor 2D unless the game captures the screen; the stalled toast repeats; the
  session log shows late-input counts and rtt every 10 s.
- Decisions: keep the 2-frame delay (1 frame hitches; adaptive delay and Wi-Fi Direct shelved, notes above); no forced BIOS/DS
  mode; settings import dropped because the stock app cannot make a backup.
- Open: the display-capture fallback is untested by any game; only Mario Kart DS tested; the freeze after a whole cup was seen
  once; phase-locking the frame starts is the next lever for lower latency.

#### More players: local benchmark on the Odin (2026-11-14)
Question: does the game drop below 60 fps with more consoles per device? (Each device runs every player's console, so
N players = N consoles on every device.) Temporary benchmark (patch: `docs/netplay-nplayer-bench.patch`, not committed):
`MirrorSession::start(..., extras)` runs extra hidden consoles (clones of the primary, neutral input, not wireless-linked, no 3D/2D
drawing) on their own threads in lockstep; `setprop debug.melonds.bench N` starts N hidden consoles while a single-player race runs.
Run on a separate copy of the ROM (own save), real save untouched.
- Odin 2 Portal, Mario Kart single-player race, compute + barrier time per frame (budget 16.7 ms):
  3 consoles 7.2 ms, 4: 7.1, 5: 7.3, 6: 7.0, 7: 7.1, 8: 8.4, 9: 9.2 (averages of the steady 10 s windows; first window of each run
  has a start-up hitch of 40-280 ms). Never above 16.7 ms in steady state, so no fps drop up to 9 consoles: the hidden consoles
  run in parallel on the other cores, so frame time stays that of the slowest console.
- CPU: each hidden console is ~18-21% of a core (so 8 hidden ~ 1.6 cores); the primary stays ~30% + render threads.
- Memory is the real cost: native heap +~125 MB per hidden console (PSS 571 MB with 1 console, 991 MB with 4, 1.86 GB with 9).
  Likely a private 32 MB copy of the ROM per console plus per-console RAM; sharing the ROM buffer is the obvious saving.
- Not covered: the extras were not part of the wireless group (no packets), only 2 physical devices exist for a real test,
  thermals/battery over a long session, weaker phones.
- What N players would need: the session (MirrorSession) is hard-wired to primary + one mirror (worker, hashes, input, focus);
  the lobby accepts one joiner; the link (DeterministicMP) already supports 16 instances but its MAC-clash aliasing is 2-way;
  networking must carry N-1 input streams (star through the host is simplest), per-console hashes; Mario Kart DS supports up to 8.
