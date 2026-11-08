# Bananza draw-cost experiments

All four switches default off. Only the exact value `1` enables them. They are
function-local lazy reads in `nx2_draw_experiments.h`, not global initializers;
`nx2_switches.txt` can therefore set them before the GPU paths first run. Restart
the process between cases. These are experiments, not defaults for any title.

## Suggested A/B order

Use the same new binary with all four off as the control, then enable one at a
time: vertex stores, partial read traps, clean words, layout interval. Finally
combine only the individually useful changes and rerun the control. The existing
bank can run, for example:

```powershell
.\run.ps1 -Label vertex-stores -Env @{ NOVAS_DKB_VERTEX_STORES = '1' }
```

Keep DKB frame tracing and all other settings identical. For timing, leave
`EDEN_NX2_GPU_PROFILE` absent in both environment and switch file: its existing
setting reader treats even `0` as enabled. If profiling is active when Maxwell3D
is constructed, it disables `fast_maxwell3d`; profiling can thus change method
dispatch as well as add measurement overhead. Use separate profiled runs for
attribution. Do not compare a profiled baseline against an unprofiled candidate.

## NOVAS_DKB_VERTEX_STORES

Port of the method-elision part of [Lichii #488](https://github.com/zortos293/Lichii/pull/488).
Drops unchanged `vertex_buffer.first/count` executable methods on the split
front, saving a ring entry and the back's dispatch. Their draw-manager handlers
do nothing beyond the register store, and neither has a dirty flag. Index-buffer
writes and topology overrides remain executable.

Elision also requires unchanged shadow RAM, the existing same-value policy and
an empty pending method sink. State writes set `sink_pending`; direct macro
writes, executable methods, method runs and the queued macro finish consume it.
This preserves state that must be applied before another engine executes. The
existing loop-skip experiment also respects a pending sink while this switch is
on. Leave `EDEN_NX2_LOOP_SKIP=0` for an isolated A/B.

The upstream global initializers for `SkipSameValueWrites` and `Nx2Perf::Verify`
were deliberately not imported. Their existing lazy readers remain unchanged.

Test: `[dkb_vertex_stores]` in `maxwell_fast_method.cpp` uses the real split back
and checks ring elision, a preceding state write, multi-method consumption,
Track/Passthrough/Replay and non-elidable methods. Run it in separate processes
with this switch at `0` and `1`, with the existing same-value switches enabled.
Native macro-heavy gameplay should additionally be checked with
`EDEN_NX2_SPLIT_VERIFY=1`; that verification run is not a speed measurement.

## NOVAS_DKB_PARTIAL_READ_TRAPS

Port of [Lichii #264](https://github.com/zortos293/Lichii/pull/264). Instead of
rearming an entire partly armed range, notify the core only of its unarmed gaps.
The union of armed pages is unchanged. Consumed and unmapped pages are forgotten
and are eligible to be armed again.

Includes the associated texture-read fix: if the texture cache answers a CPU
read, forget the entire returned area in the buffer trap tracker. Otherwise a
later GPU write could incorrectly regard a consumed trap as still armed. Cache
mutexes are acquired sequentially, never nested by this path. Both this fix and
partial arming are gated by the same switch; the off path retains old behavior.

Tests: `[dkb_partial_read_traps]` includes the upstream gap/rearm scenario and a
randomized independent page oracle, including overlapping writes, zero sizes
and whole-area consumption. Also run existing `[cpu_readback]` and `[read_traps]`
tests. A renderer/guest integration test for a texture-consumed area subsequently
rewritten through a buffer remains desirable; unit tests cover the tracker, not
the GPU texture/buffer alias lifecycle.

## NOVAS_DKB_CLEAN_WORDS

Port of [Eglantine 172565989](https://github.com/Djihads80/Eglantine/commit/17256598955eabf1f9e986536eb982d989f31d39).
In `WordManager::ForEachModifiedRange`, skips an empty masked word only if no
untracked CPU pages still require a notification. No tracked state changes on
that branch, so NovaS's extra `clean_words` summary remains valid. The original
path is used with the switch off.

Test: `[dkb_clean_words]` compares returned ranges, all tracker state, callbacks
and `clean_words` against the original path across 2,000 randomized states and
an explicit empty-CPU/nonempty-Untracked case. The test passes the policy
explicitly so both branches are covered in one process. Also run the existing
memory-tracker and `nx2_perf_draw_binds` tests.

NovaS already skips whole clean tracker words in `SynchronizeBuffer`, so expect
incremental rather than the full upstream benefit.

## NOVAS_DKB_LAYOUT_INTERVAL

Adaptation of [Eglantine d713f4462](https://github.com/Djihads80/Eglantine/commit/d713f4462a3ffdc8fad8a0f8cf816998956fdb85).
NovaS's hot path searches an immutable snapshot, not the original RangeMap tree.
Cache the last valid interval of that snapshot to avoid another binary search
for addresses inside it. Preserve the original binary search with the switch off.

The cache is thread-local and keyed by the MemoryManager's unique identifier and
the serial stored in the actual snapshot being queried. A map/unmap replacement
or a different address space cannot reuse the interval; it is not keyed by a
recyclable object pointer. Negative addresses, gaps and interval ends retain the
original answers. `EDEN_NX2_PERF_VERIFY=1` still compares to the locked kind map.

Tests: `[dkb_layout_interval]` compares against a linear reference across random
queries, boundaries, signed extremes, changed snapshots, distinct address spaces
at the same serial and complete unmapping. Run existing `[memory_manager]` tests
with this switch enabled to exercise real snapshot publication and remapping.

## Deliberately excluded

- [Lichii #481](https://github.com/zortos293/Lichii/pull/481): replacing the shared
  RangeSet backend is a wider experiment; its published measurements did not
  distinguish an FPS improvement. Revisit if sampling identifies those tree
  operations as a material fraction of Bananza's back-thread cost.
- Cross-draw handle/image-view caching based only on register dirty flags:
  constant/storage-buffer contents can change independently of the registers.
  `VisitImageView` also prepares/synchronizes the image. Skipping that work needs
  content/invalidation evidence, not merely the same binding address.

No changes to present pacing, `frames_ahead`, `nx2_gsv.h` or LIST_TARGET.

## Validation of this patch

- MSVC syntax checks passed for the affected memory manager, Maxwell3D, Vulkan
  and OpenGL rasterizer translation units and the new/updated tests.
- A focused executable compiled the updated MemoryManager, Maxwell3D and test
  objects and linked against the existing build's other libraries. The filter
  `[dkb_draw_experiments],[memory_manager],[gpu_split_dependencies],[list_target]`
  passed 616,303 assertions in 12 cases with all four switches at `0`, and again
  with all four at `1`, in separate processes.
- The normal `compilar.cmd tests` path could not run in this shell because
  `cmake` was unavailable. This is not a full rebuild of either renderer or the
  emulator, nor a full-suite run. The installed emulator was not changed and the
  interactive benchmark was not launched.
