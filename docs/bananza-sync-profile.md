# Bananza: list flush and fence release attribution

Enabled only by `Nx2Profile::Enabled()` (`EDEN_NX2_GPU_PROFILE`); no new switch.
The frontend's late setting read remains in `ThreadManager::StartThread`.

`DKB sync thread=front|back window_wall_ms=...` reports independent windows of at
least ten seconds, on the next measured operation. The final incomplete window
is not emitted at shutdown. These windows need not align with the existing GPU
profile or with each other. With the split disabled, both sets appear on `front`.

Counters are **raw totals for the window**, not per-second rates. Divide by
`window_wall_ms / 1000` for rates. Timers report `*_ms_per_s`, raw `*_calls`, and
`*_max_ms` (maximum single operation in that window).

## Front

- `list_fallbacks`: calls to `FlushGpuWrittenCommands` that pass its size/NX2 and
  GPU-written checks. Not the denominator of all command-list fetches.
- `list_pre_drain`: the first explicit drain, before inspecting dirty pages.
- `list_run`: the subsequent synchronous `Run`, including enqueue, callback and
  return to the front. `list_post_drain` measures its actual second drain, after
  enqueue. Its `list_queue` component ends when the back begins the
  callback; `list_callback` measures the entire flush callback, including locks,
  download preparation, GPU completion and copying into guest memory.
- `fence_run`, `fence_drain`, `fence_queue`, `fence_callback`: the same decomposition for the
  puller's synchronous `ReleaseFences` helper (`DrainSite::FenceRelease`).
  Asynchronous releases queued with `OnBack` are not included.

The second drain is inside `Run`; `list_run` includes its enqueue overhead too.
The post-enqueue drain timers have no calls when the split is disabled or the
operation is already on the back. The callback may begin during enqueue, so its
time can overlap enqueue as well as drain.
`run - queue - callback` includes completion publication, waking/resuming the
front and any idle-hook work awaited by `Drain`. It is not solely OS wake time.
Timings are inclusive: **do not add queue and callback to run**, or add these
values to the existing `SplitDrain`/by-site totals. The two sequential drains,
`list_pre_drain + list_post_drain`, can be added; use `list_pre_drain + list_run`
to include the second operation's enqueue overhead too.

## Back

- `list_batches`: calls to `Nx2FlushListPages`.
- `requested_new_pages`: distinct requested device pages within each batch whose
  tracked write generation differs from the synchronized generation. "New" means
  a version needing synchronization, not a newly allocated page.
- `requested_reused_pages`: distinct requested pages already synchronized at the
  same generation. This is not a count of fast-path list reads outside fallback.
- `requested_untracked_pages`: distinct requested pages absent from the list-page
  tracker. These three counters partition the requested pages for Vulkan.
- `extra_drawn_pages`: pending draw-written pages included by batching that were
  not already requested. Page counts can repeat across batches and windows.
- `downloads_nonempty`: list batches that actually selected at least one nonzero
  download range. Empty calls still count in `list_batches`.
- `download_copies`, `download_bytes`: selected ranges and their unpadded byte
  sizes, including mapped-buffer host reads. They are not Vulkan submission or
  physical transfer counts, nor bytes ultimately written after CPU-write conflict
  filtering. Ordinary CPU readbacks outside a list batch do not enter these totals.

## Contiguous translation reuse

Ported from Lichii [PR #580](https://github.com/zortos293/Lichii/pull/580),
commit `7083185c438e7929ba0d85cf317d7e4d91e6d821` (imports #478).
Only a cached single segment may satisfy a smaller size. Multi-segment entries
still require an exact size, and invalidation retains the longest cached extent.
The consumer ignores the segment contents when there are fewer than two.

The three upstream cases are in `src/tests/video_core/virtual_range_cache.cpp`,
tagged `[virtual_range_cache]`: alternating contiguous sizes, split ranges and
unmapping the tail of the retained longest range.
