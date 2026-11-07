# Ocarina Dynamic FPS add-on

`Dynamic FPS by Dark (built-in)` is an opt-in NovaS game Add-On for Ocarina.
It is applied at module load and takes effect on the next game launch.
It targets title `0400486024DEC000`, module `main`, build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348` only. Max Resolution remains a
separate, default-on Add-On. Either can be disabled independently in Add-Ons;
the existing disabled-add-on configuration persists that choice. Disabling an
Add-On takes precedence over the requested frame mode.

## Credit and source

Ported from [Lichii PR #240](https://github.com/zortos293/Lichii/pull/240),
commit `18639c138d9668db184df8f1ed0730ad4ddc6a01`.
Dark, an Eglantine developer, wrote the routine. This port preserves the call and
120-byte routine from Frizti's `oot-gather-fix` commit
`765659af4ccb7201d1145c57f3b45ce84653cea0`. That commit explicitly states that
Dark gave permission to bundle it. Frizti supplied the built-in integration.

The routine's SHA-256 is
`3c15dfb57f748328ad7d1c1675941dc2ace32a5e7b10907368ceac6a9a8c5ee1`.
The call's SHA-256 is
`e4d3f65ad1b568b557f02b9a087a9c304240ac70d6ef474dac83a1f97737515e`.
Lichii added original-byte and memory-layout guards and transactional validation
before writing either record. NovaS additionally rejects changes to the three
known fixed-30 controller/time-step instructions and gates the timer settings.
Conversely, fixed 30 on the update build requires the original dynamic-hook
instruction, so it defers to an external dynamic mod at that site.

## Verified original layout

The upstream investigation reports an independently extracted original `main.text`
with SHA-256
`f71eb7b75b11dcc8d955f3f6e987a098573288d9ce41aa22a96272541e858c5f`, matching
the text hash in its NSO header. Its instruction at module offset `0x6E6CC4`
is `28 18 20 1E`, or `fdiv s8, s1, s0`. The patch replaces it with
`bl 0x3C46200` and executes the original division first in the new routine.
The enclosing function saves and restores its return address. The continuation
does not consume the routine's clobbered scratch registers or flags before
intervening calls or redefinition.

The original text ends at `0x3C461D0`; read-only data starts at `0x3C47000`.
The routine occupies `0x3C46200` through `0x3C46277`, in executable alignment
padding, not original text. `AppLoader_NSO::LoadModule` zero-initializes the
module image before copying segments and rounds the text mapping to a page.
The loader checks that this space is still zero-filled executable padding
before installing the routine.

Data ends at `0x4D46B80` and BSS ends at `0x4EA0000`. The routine stores its
previous timestamp and smoothed delta at `0x4E9FE00` and `0x4E9FE08`. The
loader requires both words to fit BSS and remain zero-initialized. These
checks establish addressability and initialization, not exclusive ownership
of that storage throughout gameplay.

Any changed user-mod bytes in either patch record or the timer state take
precedence over the entire built-in patch. An incompatible build, original
instruction, layout, or truncated module leaves both patch records untouched.

## Timing limits

The routine reads `CNTPCT_EL0` and `CNTFRQ_EL0`. After its first call, it caps
the timestamp difference at `CNTFRQ / 20`, or 50 ms, then smooths it by adding
one eighth of the signed difference from the previous smoothed value.
It calculates `s2 = (smoothed_ticks / CNTFRQ) * s1`. It replaces the original
`s8 = s1 / s0` only when `s2 > 1.03 * s8`.
The original code selects `s1` as 30 or 60 and converts the integer field at
`x21 + 0x2C8` into `s0`, so the result is a frame-step scale, not seconds.

This is not a promise of correct game speed at every frame rate. It retains
the original step as a floor, caps long frames, and smooths changes. Actual
gameplay speed and the runtime value of that frame-rate field still require
live validation.

In Lichii, `CoreTiming::GetClockTicks` scales the counter for CPU Boost and
Overclock, while Dynarmic's advertised counter frequency stays unchanged.
Boost uses a 1.7 multiplier and Overclock uses 2. Enabling core-speed
synchronization can further divide the counter by the configured speed limit.
Those settings alter the elapsed seconds this routine calculates. CPU Clock
Normal and an unscaled counter are prerequisites for evaluating its timing;
NovaS rejects the patch unless CPU Clock is Normal, core-speed synchronization
is off, multicore is on, and the backend is Dynarmic. No global clock settings
are overridden. These checks run at load: keep them unchanged while playing.
Use the normal 100% speed limit, without turbo/unlimited mode. Gameplay speed
and animation/physics correctness still need Oscar's validation.


## Launcher configuration

Portable CLI: `user/config/sdl2-config.ini` (or the INI passed with `-c`).
Write the mode before launching the game; changing the INI does not repatch a
running process.

| Renderer key `oot_frame_mode` | Behavior |
| --- | --- |
| -1 (default) | Legacy: use `oot_30fps` (false = original, true = fixed 30). |
| 0 | Original timing, nominal 60 FPS. Overrides the legacy bool. |
| 1 | Existing Orchid fixed-30 controller + double step. Overrides the bool. |
| 2 | Dark's continuous time-step adjustment. Does not apply fixed 30. |

Suggested launcher label for 2: **Ocarina: FPS dinámicos (experimental)**.
It compensates the step at intermediate frame rates; it does not make the PC
render 60 frames when it lacks the throughput, or eliminate shader stutters.

```ini
[Renderer]
oot_frame_mode\default=false
oot_frame_mode=2

[System]
fast_cpu_time\default=false
fast_cpu_time=0

[Core]
use_multi_core\default=false
use_multi_core=true
sync_core_speed\default=false
sync_core_speed=false
use_speed_limit\default=false
use_speed_limit=true
speed_limit\default=false
speed_limit=100
```

The Windows CLI already uses Dynarmic. Merge these keys into their existing
groups. Keep FIFO/native rendering settings for the initial comparison. Mode 0
disables both built-in timing patches; mode 1 restores fixed 30; -1 restores the
old checkbox behavior. An external timing mod can still change original mode.
Do not combine with other timing mods. The known static-30 sites are guarded,
but arbitrary external hooks/cheats cannot all be detected.

Log confirmation: `Applying built-in patch "Dynamic FPS by Dark (built-in)"`,
followed by `Ocarina dynamic timing enabled`. Unsupported builds/settings or
conflicting bytes log a reason and leave the game's timing unchanged. Dynamic
currently supports only update build `E0F3...3348`; the base build
`33FA...5920` remains supported by fixed 30 only.

## Why not hot-switch the three fixed-30 instructions?

[Orchid's analysis](https://github.com/MythosOfs/Orchid/commit/3e6714241)
identifies a measured controller and `ForceInterval(1)` during boot. Its three
replacements embed immediate values (2/30), not a single shared mutable scalar.
The time manager reads an FPS field at `x21 + 0x2C8` at Dark's hook; identifying
that field is not proof that writing it from a host thread safely updates all
controller, presentation and animation state. No stable object address or
synchronized setter is established here.

Hot code writes would need coordinated guest-core suspension and JIT
invalidation, and would not replay the boot-time setter. The cheat engine's
write-and-invalidate path is not a transaction for three instructions plus live
game state. Therefore this port installs the known routine before execution and
lets the game thread update its own step. There are no host-side 30/60 switches,
no hysteresis thresholds, and no switch logs to imply otherwise. Host presents
also include pacing waits and do not directly provide the hypothetical 60-FPS
work budget while capped at 30.

The routine smooths elapsed time inside the guest instead. The emulator's
existing multicore counter is already wall-clock-based with the prerequisites
above; changing a limiter alone cannot replace the game's fixed per-frame step.

## Validation

Added production `PatchManager::PatchNSO` tests in
`src/tests/core/builtin_patches.cpp`, tagged `[oot_dynamic_fps]`: exact upstream
routine bytes, opt-in/Add-On disabling, full build/title/module gates, all-record
transactional rejection (header, code padding, BSS, bounds), IPS/IPS32 precedence,
known static-30 conflicts, legacy/new mode precedence, and incompatible clocks.
Existing `[oot_30fps]` and Max Resolution tests remain applicable. New tests
have not been compiled/run by the editing agent.

After compiling, run the test binary with `"[patch_manager]"`. For gameplay:
compare the same route at mode 1 and mode 2, with 100% speed, normal CPU clock,
no sampler/profilers, and warm shader caches. Time the same traversal/animation
against a wall clock; record actual FPS and check input, camera, collisions,
cutscenes and audio synchronization. Verify recovery after a slow area, loading,
and pause/resume. The 50 ms cap avoids a huge catch-up step after a pause but
cannot repay time lost during long stalls. Keep the dynamic option experimental
until those checks pass; stable 30 remains available.
