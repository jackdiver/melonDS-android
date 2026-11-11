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
