# Notes from renut_nx_ultimate_base's docs (read 2026-11-21)

Source: https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base/tree/main/docs (a newer base of the same lineage:
katamari-nx / banjo-nx / sasrt-nx / blur-nx ...). Our tree lacks most of these keys (checked 2026-11-21: only
`clock_lock_free` and `banjo_renut_core_layout` exist here). Candidates for Tekken 6, by our bottlenecks:

- Presentation (our ~8 ms of render thread in bqDequeueBuffer / bqQueueBuffer): `banjo_async_present` (ultimate-fixes.md
  row 145, on there): the window queue / dequeue binder calls on a helper thread (`--wrap=nwindowDequeueBuffer` in
  sdk/src/ui/switch_perf.cpp; plume's swapchain cancels the buffer dequeued ahead). tw2de_nx: 2.7 ms a frame.
- Events on address arbitration: `thread_event_address_wait` (row 65, on): our semaphore change, for events (Horizon gives
  a condvar-woken thread the mutex before it runs). sasrt-nx: main thread in pthread_mutex_lock under PosixEvent::Set
  5.6 % -> 0.0 %; 100 ms re-check as a safety net. Files: threading_posix.cpp, threading_switch.h/.cpp,
  address_event_core.h. Tekken's main loses ~1 ms/frame in PulseEvent (more in Manji Valley).
- `thread_mutant_release_lockfree` (row 146): mutant release takes the condition mutex only with a blocked thread.
- Render thread: `banjo_renut_stream_redeem` (row 145) store-card check per dynamic-stream conversion (PagesClean), 7 ms in
  tw2de_nx; `dishonored_renut_vertex_readable_check` (row 101); render-thread-turn-offs.md's per-game rule.
- GPU resolves: `ultimate_deko3d_depth_resolve_lazy` (row 45): depth decompressed only when read.
- Allocator: `blur_small_alloc_mb` (row 151): small allocations off newlib's one malloc lock.
- Measurement cost: `switch_profile_stacks` pauses every game thread; `switch_perf_count_calls` off in players' tomls.
- Release builds: PGO retrained on the scenes that matter (Banjo lost 13 % on a stale profile).
- Freezes: nothing matches our 0 fps lost wake-up exactly; GPFIFO refusal (row 157) and the depth-resolve GPU hang
  (row 64) are the GPU-side hang fixes there.
