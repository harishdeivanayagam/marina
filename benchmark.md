# Benchmark: todos-http-sqlite — marina vs Node.js vs Bun vs Go

Same HTTP API (`examples/todos-http-sqlite.tsm`) benchmarked
on a Vultr cloud instance, then the instance was deleted.

## Test environment

| | |
|---|---|
| Instance | Vultr Cloud Compute `vc2-1c-1gb` ($5/mo), region **blr** (Bangalore) |
| OS | Ubuntu 24.04 LTS x64, 1 vCPU (Intel Xeon Cascadelake), 1 GB RAM + 2 GB swap |
| Marina | v0.2.0 (commit `af5022e` + runtime fixes, see below), AOT binary 2.5 MB, evented server (benchmarked 2026-10-19 on `vc2-1c-1gb`) |
| Node.js | v22.23.3 (built-in `node:sqlite`) |
| Bun | 1.4.3 (built-in `bun:sqlite`) |
| Go | go1.23.3 (module `modernc.org/sqlite`, pure-Go driver) |

Each implementation serves the identical API on port 8080:
`GET /todos`, `POST /todos` (validated, 4–20 chars), `POST /todos/done-all`,
backed by SQLite at `/tmp/todos-http-sqlite.db` (fresh DB, seeded with 10 rows).

## Methodology

- Load generated **locally on the instance** (loopback) with `wrk`, so the
network path is identical for all four (a public-internet load test from the
dev machine proved impossible — the user's ISP path blackholes large packets
and filters the ports, see caveats).
- Per implementation: fresh DB → 10s warmup → **30 s** `wrk -t4 -c64` GET
→ **20 s** `wrk -t4 -c64` POST → also a `wrk -t2 -c2` GET run.
- Memory = process `VmRSS`, sampled every 50–100 ms; peak taken over the
whole run (idle = right after startup, peak = under the c64 load).
- POST inserts a row per request (table grows during the 20 s run for all).

## Results

### Throughput — max requests

| Runtime | GET req/s (c=2) | GET req/s (c=64) | **GET total in 30 s (c=64)** | GET p50 | GET p99 | POST req/s (c=64) | POST total in 20 s |
|---|---|---|---|---|---|---|---|
| **Bun 1.4.3** | 19,829 | **19,903** | **597,567** | 3.03 ms | 6.72 ms | 964 | 19,293 |
| **Marina 0.2 (evented)** | 10,079 | 11,102 | 333,580 | 5.71 ms | 10.02 ms | **1,400** | 28,032 |
| **Go 1.23** | 10,742 | 10,566 | 317,317 | 7.22 ms | 55.27 ms | 3,015 | 60,334 |
| **Node.js 22.23** | 9,609 | 10,163 | 305,387 | 5.82 ms | 15.19 ms | 922 | 18,468 |

### Memory (resident set)

| Runtime | Idle RSS | **Peak RSS under load** |
|---|---|---|
| **Marina** (AOT binary, evented) | **4.2 MB** | **7.1 MB** |
| **Go** | 11.8 MB | 20.0 MB |
| Bun | 23.1 MB | 40.7 MB |
| Node.js | 60.2 MB | 84.5 MB |

### Marina concurrency sweep (GET /todos, wrk, 5 s each)

| connections | 2 | 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|
| req/s (evented) | 10,999 | 11,827 | 12,193 | 13,086 | 12,990 | 12,469 |

The sweep is smooth at every concurrency level — no collapse.

## Summary

- **Max requests/sec**: Bun wins on GET (~19.9k req/s, ~597k requests in
30 s). The evented marina build is in the same league as Go and Node —
~11.1k req/s at c=64 (~334k requests in 30 s), beating both at c=64 with
tighter p99 latency (10 ms vs 15–55 ms). POST (write + validation) at
1,400 req/s beats Bun and Node; Go keeps a ~2× edge there.
- **Memory**: Marina's AOT binary is the lightest by a wide margin
(4.2 MB idle / 7.1 MB peak) — a third of Go's peak, ~12× lighter than Node.
- Architecture no longer bottlenecks: the poll(2) event loop + keep-alive +
promise-based async handlers scale flat across concurrency; the remaining
~1.8× gap to Bun on GET is per-request allocation churn in the runtime
(string/json handling), not the server core.

## Fixes required to get marina running

Runtime bugs in `crates/marina-runtime/src/lib.rs` were found and fixed
during these benchmarks (otherwise the example segfaults non-deterministically
on both macOS and Linux):

1. **Uninitialized-memory UB in heap allocation** — `marina_object_new`,

   `marina_array_new` and `marina_promise_create` assign `Vec`/`Mutex` fields
    into memory returned by `alloc()` without initialization; Rust then drops
    the garbage "old value" and frees a bogus pointer (e.g. it crashed freeing
    `0x62642e6574` = leftover bytes of the string `"…todos-http-sqlite.db"`).
    Fixed by using `std::ptr::write` for the non-trivial fields.
2. `lto = true` **in** `[profile.release]` aggravated the same UB (heap-layout

   dependent crashes). With the UB fixed, LTO builds are stable again.
3. **Promise ref leaked per await** (evented rerun) — `marina_await` consumed

   the caller's reference but nothing ever released it; one promise leaked
    per request. The waiter task now inherits the consumed ref and releases
    it after resume.
4. **Array/Object element buffers + Promise Mutex leaked on free** —

   `free_obj` released element payloads but never dropped the `Vec` itself
    (nor the promise's `Mutex`). Fixed with `drop_in_place` before `dealloc`.
5. **Await results leaked for non-string types** — `materialize_await_value`

   retained strings but raw-copied arrays/objects into the frame slot; now
    retains uniformly and the waiter's copy is released after resume.

## The evented server (architecture)

Thread-per-connection and the serial accept loop were both rejected in favor
of an evented design:

- **Single** `poll(2)` **event loop** on the main thread: listener + self-pipe
wake fd + every connection; no thread per connection.
- **Non-blocking sockets for life** (accepted sockets do not inherit
`O_NONBLOCK` on Linux — set explicitly on every accept).
- **Buffered incremental request parsing** (`try_parse_http`: partial reads
OK, 64 KB header / 16 MB body caps) instead of one byte per syscall.
- **HTTP keep-alive** — connections flip back to reading after a response
(this also eliminated server-side TIME-WAIT churn that poisoned SYN
retransmits under `Connection: close`).
- **Async handlers as promises** — `Request -> Server -> Promise [todosRouter.get()]`: routes are `async`, handlers run to first await on
the event loop, blocking I/O (SQLite, fs, net) runs on a worker pool, and
the resolved promise resumes the handler.
- Responses are written with a poll(`POLLOUT`) loop so worker threads never
flip the shared `O_NONBLOCK` flag against the event loop.

Soak tests after the fixes: 200k GETs on Vultr → **RSS 4.4 MB** (before the
refcount fixes the same route leaked ~2.5 KB/request → 724 MB); local
40-connection mixed GET/POST/404 run → 60k requests, zero errors,
RSS 3.5 MB.

## Caveats

- Loopback load generation shares the single vCPU between `wrk` and the
server, so absolute numbers are lower than true capacity — identical for
all four, so the comparison holds.
- An attempt to generate load from a second Vultr instance in the same
datacenter (0.28 ms RTT) was made, but Vultr's edge filtered/throttled
instance-to-instance public traffic on port 8080 (SSH tunnel also collapsed
under wrk load); the loadgen instance was deleted unused.
- The user's ISP path to Vultr blr drops 1472-byte packets (MTU blackhole)
and filtered HTTP ports during testing, so remote load generation from the
dev machine was not viable.

Raw outputs: `bench/raw/`. Scripts: `bench/` (ports in `bench/{node,bun,go}/`).
