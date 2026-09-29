# Streaming Demo Background I/O (2026-09-05)

## Scope And Ownership

The fixed height-world demo now uses `StreamingDemoIo`, one persistent CPU-only
worker with one outstanding request, including unconsumed results. F5 snapshots
the current edit overlay and queues codec/write/flush/close/rename. F9 queues a
bounded read, validation, and construction of a replacement WorldStreamer.
Repeated save/load requests while busy are rejected, not silently queued.
Editing remains available during a save; the saved snapshot does not change.
Editing is rejected during reload preparation to avoid silently losing new edits.
The HUD displays I/O busy/idle status.

The live world, GPU meshes, and camera remain untouched until a successful load
result is polled on the main thread. A failed load retains them. On success,
the old CPU streamer transfers exclusively to the worker for destruction and
job shutdown. New-world publication, camera reset, GPU retirement, and subsequent
saved-edit replay stay on the main thread. At most one old and one new CPU world
overlap; busy rejection prevents an unbounded retirement queue. Graphics APIs
are never called by this worker. Mutex sections contain only request/result
transfers; disk work and world destruction happen outside the lock.

Shutdown drains accepted writes and joins the worker. This can wait on exit,
not in a live frame. Worker exceptions travel through the result and are reported;
an unconsumed failure remains recoverable after explicit shutdown. Command-line
`--load` still validates before window/output creation rather than asynchronously.

## File Safety

The save format is unchanged. Reading is bounded to 128 KiB plus one probe byte,
avoiding a stale pre-read file-size check. Demo save paths use filesystem-aware
streams. The demo writer checks stream state after write, flush, and close before
renaming its same-directory temporary file over the prior save. On failure it
cleans up only the temporary file it opened. Failed encoding, opening, or rename
does not remove the existing destination. The shared engine file helper, which
does not check `fclose`, was not changed as part of this scoped demo fix.

This is not crash-durable persistence: there is no filesystem/device durability
flush, arbitrary corruption checksum, multi-process writer coordination, general
multi-region save format, or cancellation of an OS file operation already running.
Existing capture-path limits are unchanged.

## Measurements And Gates

Two GCC Release routes at 1280x720/swap interval zero are retained in
`build/release/streaming-async-io-{1,2}`. Events distinguish main-thread enqueue,
worker duration, request-to-polled-completion, and reload publication; these
intervals overlap and must not be added as serial frame costs. `frames.csv`
adds `io_poll_us` and `io_busy`. Verification renders frames while operations
are pending and still saves twice before testing separate-process reload.

| Measurement (milliseconds) | Run 1 | Run 2 |
| --- | ---: | ---: |
| Save enqueue, two requests | 0.0098 / 0.0079 | 0.0107 / 0.0079 |
| Save worker, two requests | 1.824 / 2.104 | 1.777 / 1.916 |
| Reload enqueue | 0.0159 | 0.0160 |
| Reload read/decode/construct worker | 0.907 | 0.761 |
| Main-thread reload publication | 1.457 | 1.186 |
| Old-world retirement worker | 6.723 | 3.792 |
| Frame-work max while reload/retirement pending | 2.700 | 2.122 |

For context, the preceding shared-budget runs had synchronous saves of
1.488-1.745 ms and reload/decode/reset calls of 3.900-7.489 ms. These are local
observations, not a statistical speedup or hard-budget claim. Saved-edit replay
still reached 2.095 ms of combined stream/sync work during the first reload.
GPU publication, replay validation, and individual uploads remain non-preemptible.

Release: 8/8 CTest targets, 473 core cases / 110,789 assertions. MSVC ASan:
6/6, 423 core cases / 109,446 assertions, existing 17 native-driver exclusions.
Ten portable fixture cases cover 4,984 assertions. New I/O tests cover immutable
snapshots, busy rejection, replacement, ownership transfer with in-flight jobs,
bad data, missing/oversized input, failed open/rename, recovery, and shutdown.
Actual disk-full/close failures are not fault-injected. ASan is not a race detector.

Gallery: `build/release/streaming-demo-captures/68994896b31d/index.html`.
All five settled images match the prior shared-budget baseline exactly. Both
visual gates pass (38 streaming / 156 existing-gallery BMPs); reloaded and inbound
images were inspected. Sparse captures do not prove absence of every transient.
WebGPU smoke passes but does not exercise this native I/O helper. No new Linux,
macOS, Metal-hardware, or ThreadSanitizer acceptance is claimed. M16 stays partial.

## Replay And GPU Cleanup Follow-up (2026-09-06)

Further instrumentation separated replay and GPU cleanup. The helper used to
regenerate and serialize a scratch chunk just to validate each overlay, and
readiness checks copied full chunk snapshots. It now preflights all values
before mutation using the shared `validHeightSample` rule from ChunkStore:
finite height, byte weights summing to 255, correct policy and sample bounds.
Metadata checks replace snapshot copies and reject insufficient target-version
headroom. Empty overlays, unsorted edits, duplicates (last value wins), negative
finite heights, and biome ids retain their existing behavior. Save wire format,
codec validation, and border/persistence propagation are unchanged.

Reload publication now detaches old entities immediately and queues their mesh
handles for main-thread destruction during sync, rather than deleting all 25
at once. At most two old meshes retire per sync call; an expired deadline still
allows one to guarantee progress. Normal residency unload behavior is unchanged.
Another reload is rejected until this queue drains, and convergence includes
GPU retirement. Explicit clear/destruction drain everything immediately. The
maximum is 25 active plus 25 detached meshes plus one replacement, below the
renderer's existing 64-slot limit. No old-world draws survive detachment. The
HUD reports pending retirement, and CSV adds `replay_us`, `reload_gpu_retired`,
and `reload_gpu_pending`; `reload_gpu_detach` replaces the bulk-clear event.

Instrumented baselines: `build/release/streaming-replay-baseline-{3,4}`.
Earlier attempts `{1,2}` ran the previous binary while compilation was still
pending and do not contain the new timing fields; they are not used below.
Updated routes: `build/release/streaming-replay-bounded-{1,2}`. Local Release,
1280x720, swap interval zero, milliseconds:

| Measurement | Before 1 | Before 2 | After 1 | After 2 |
| --- | ---: | ---: | ---: | ---: |
| Replay helper | 1.504 | 1.084 | 0.0744 | 0.0967 |
| Reload publication | 2.465 | 1.394 | 0.0140 | 0.0085 |
| GPU bulk-clear / detach portion | 2.433 | 1.351 | 0.0038 | 0.0032 |

All 25 old meshes retired across 13 frames in each updated run. Cleanup was
spread, not eliminated: sync maxima during that interval were 1.099/0.896 ms
and combined stream/sync maxima were 2.117/1.534 ms. Individual operations and
progress exceptions remain non-preemptible; these observations do not close
the hard 2 ms gate. Baseline replay times exclude the caller's readiness copy.

Final verification: Release 8/8 (475 core cases / 110,922 assertions), MSVC ASan
6/6 (425 / 109,579 with the same 17 native-driver exclusions), WebGPU 1/1.
Twelve portable fixture cases cover 5,117 assertions. New cases reject NaN,
infinities, malformed weights, out-of-range indices and wrong-policy edits
before any version/value changes; they test duplicates and persistence. GPU
tests cover zero old draws after detach, count/deadline limits, repeated-reload
rejection, convergence, clear and destructor recovery. No timing threshold is
used for correctness. The shared height rule also rebuilt in WebGPU.

Final gallery: `build/release/streaming-demo-captures/a0c48054b692/index.html`.
All five settled images match baseline 4 exactly; both visual suites pass
(38 streaming / 156 gallery BMPs). Reloaded and outbound images from updated
run 2 were inspected. Sparse-image and hardware-acceptance caveats still apply.
Next: broaden voxel/hex and live-region stress coverage, then use those profiles
to prioritize remaining non-preemptible work and the large shared-map edit cost.

## Research

- [Condition-variable predicate waits](https://eel.is/c++draft/thread.condition.condvar) handle spurious wakeups; all shared state is mutex-protected.
- [Thread join semantics](https://eel.is/c++draft/thread.thread.member) underpin draining shutdown before worker storage is destroyed.
- [Stream flush](https://eel.is/c++draft/ostream.unformatted) and [ofstream close](https://eel.is/c++draft/ofstream.members) report stream failures that must precede replacement.
- [Filesystem rename](https://eel.is/c++draft/fs.op.rename) provides same-directory replacement without deleting the destination first.
- [Floating-point classification](https://eel.is/c++draft/c.math.fpclass) supports retaining the existing finite-height validity rule.
- [OpenGL buffer deletion](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDeleteBuffers.xhtml) remains on the context-owning thread; only scheduling changed, with no new wait or backend deletion semantics.
