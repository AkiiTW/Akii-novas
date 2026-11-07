# Command list writer targets

`EDEN_NX2_LIST_TARGET` is an opt-in experiment for the split GPU front's command
list fetches. Set it before launching the emulator:

- Unset or `0`: existing behavior.
- `1`: wait for `ListTarget` on mapped, non-sparse pages never marked GPU-written
  or command-list pages. Other entries retain the conservative path.
- `2`: use the same target, read guest words, wait for all compute/copy writers,
  then reread and count differences. Parse the post-wait words; if the page became
  GPU-written, use the existing list-copy/flush path instead. This mode measures
  correctness, not the speed of mode 1.

When enabled, this takes precedence over `EDEN_NX2_LIST_PAGES` and
`EDEN_NX2_LIST_FAST`. It applies to NX2 split rendering, not only Ocarina.

The target is the maximum of the last unknown write, compute/copy work up to the
last host-level synchronization, and explicit writes overlapping the list's
device pages. Unlike LIST_PAGES, this uses the upstream model that command fetch
may run ahead of engines until the guest synchronizes. Verification does not
prove that every game uses that model correctly.

The port retains NovaS's #243 `FrontDmaReadQueued`, queued-write overlays and
async-report checks. It does not add `FrontDmaReadEarly` or replace the MME DMA
read policy. For #231, successful fast semaphore acquires and resumed acquires
also record a host synchronization boundary while this experiment is enabled.

Unclassified functions remain unknown. Pending functions are settled before
translating list addresses. Translation caching includes the memory manager's
map serial; missing, sparse, out-of-range or excessively large write ranges
become unknown. Tracker overflow also becomes an unknown write.

At Info level, `List target` reports cumulative counts per channel:

- `checks`, `waits`, `unknown`, `fallback`: attempted fetches and their waits.
- `shorter_pending`: the selected target preceded a still-pending all-writer
  target. This is an opportunity to save a wait, not a measured time saving.
- `verified`, `changed_entries`, `changed_words`, `became_written`: comparisons
  in mode 2. Counters keep growing after the first 16 detailed mismatch logs.
- `snapshot_misses`: eligible submit-time snapshots that differ from the
  post-wait words. Mode 2 discards these snapshots too.

`EDEN_NX2_GPU_PROFILE=1` also reports list target checks/waits per second. Compare
mode 1 against disabled with all other settings unchanged, after checking mode 2
for nonzero differences. CPU writes concurrent with verification can also cause
differences; investigate them before enabling mode 1 by default.

Sources: Lichii [daca1c754](https://github.com/zortos293/Lichii/commit/daca1c754856fbccb270fb57a8aebe7a96f0c5eb)
and the tracker/effect annotations from
[e691cc3f2](https://github.com/zortos293/Lichii/commit/e691cc3f221b2bc0dbe0494a696819c698fbed89),
by zortos293, co-authored by capy-ai[bot]. The Maxwell3D optimization in
4a0bc9a00 is not part of this port.

Tests are tagged `[split_writes]` and `[list_target]`; restart coverage remains
in `[gpu_split_dependencies]`. They were added/reviewed without compiling or
executing them in the porting worktree.
