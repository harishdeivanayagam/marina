# Benchmark: todos-http-sqlite — marina vs Node.js vs Bun vs Go

Same HTTP API (`examples/todos-http-sqlite.tsm`) benchmarked
on a Vultr cloud instance, then the instance was deleted.

## Test environment

| | |
|---|---|
| Instance | Vultr Cloud Compute `vc2-1c-1gb` ($5/mo), region **blr** (Bangalore) |
| OS | Ubuntu 24.04 LTS x64, 1 vCPU (Intel Xeon Cascadelake), 1 GB RAM + 2 GB swap |
| Marina | v0.2.0 at commit `bac9717` plus the socket-leak fix below, C++ runtime, AOT binary 274 KB, evented server (benchmarked 2026-10-21 on `vc2-1c-1gb`) |
| Node.js | v22.23.3 (built-in `node:sqlite`) |
| Bun | 1.4.3 (built-in `bun:sqlite`) |
| Go | go1.23.3 (`modernc.org/sqlite` v1.38.2, one connection) |

Each implementation serves the identical API on port 8080:
`GET /todos`, `POST /todos` (validated, 4–20 chars), `POST /todos/done-all`,
backed by SQLite at `/tmp/todos-http-sqlite.db` (fresh DB, seeded with 10 rows).

## Methodology

- Load generated **locally on the instance** (loopback) with `wrk`, so the
network path is identical for all four (a public-internet load test from the
dev machine proved impossible — the user's ISP path blackholes large packets
and filters the ports, see caveats).
- Per implementation: fresh DB, seed 10 rows, then **10 s** `wrk -t2 -c2`
GET, **30 s** `wrk -t4 -c64` GET, **20 s** `wrk -t4 -c64` POST.
- Memory = process `VmRSS`, sampled every 100 ms; peak taken over the
whole run (idle = after the seed, peak = under the load).
- POST inserts a row per request (table grows during the 20 s run for all).
- The Go server uses one SQLite connection (`SetMaxOpenConns(1)`), the same
shape as Marina, Node, and Bun. The default `database/sql` pool returns
`SQLITE_BUSY` under this write load; those responses are HTTP 500, and
`wrk` still counts them.

## Results

### Throughput — max requests

| Runtime | GET req/s (c=2) | GET req/s (c=64) | **GET total in 30 s (c=64)** | GET p50 | GET p99 | POST req/s (c=64) | POST total in 20 s |
|---|---|---|---|---|---|---|---|
| **Bun 1.4.3** | 19,660 | **19,285** | **579,090** | 3.10 ms | 7.35 ms | 990 | 19,848 |
| **Go 1.23** | 10,635 | 10,883 | 326,888 | 7.03 ms | 49.59 ms | 913 | 18,293 |
| **Marina 0.2 (evented)** | 10,261 | 10,879 | 326,831 | 5.85 ms | 9.79 ms | **1,403** | **28,097** |
| **Node.js 22.23** | 10,530 | 9,889 | 297,094 | 5.95 ms | 15.95 ms | 942 | 18,870 |

### Memory (resident set)

| Runtime | Idle RSS | **Peak RSS under load** |
|---|---|---|
| **Marina** (AOT binary, evented) | **5.3 MB** | **7.9 MB** |
| **Go** | 10.3 MB | 18.4 MB |
| Bun | 22.3 MB | 39.4 MB |
| Node.js | 57.9 MB | 81.1 MB |

### Marina concurrency sweep (GET /todos, wrk, 5 s each)

| connections | 2 | 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|
| req/s (evented) | 11,487 | 12,033 | 12,362 | 13,057 | 13,352 | 11,425 |

The sweep stays between 11.5k and 13.4k req/s, peaking at 32 connections.

## Summary

- **Max requests/sec**: Bun leads GET at 19,285 req/s (579,090 requests in
30 s). Marina and Go are level at 10,879 and 10,883 req/s; Node.js is at
9,889. Marina's p99 on that run is 9.79 ms, against 15.95 ms for Node.js
and 49.59 ms for Go. Bun's p99 is 7.35 ms. POST (write + validation) is
led by Marina at 1,403 req/s (28,097 inserts in 20 s), ahead of Bun (990),
Node.js (942), and Go (913).
- **Memory**: Marina's AOT binary is the lightest (5.3 MB idle / 7.9 MB
peak) — about 2.3× lighter than Go at peak, and about 10× lighter than Node.
- The concurrency sweep stays in a band from 2 to 64 connections. GET
throughput is about 1.8× behind Bun.

## Fixes required to get marina running

This run is the C++ compiler and runtime. `http_respond` leaked two
descriptors per request: the dispatch path `dup`s the connection for the
worker, and the write path `dup`s that again and drops both without
`close`. The process hit `RLIMIT_NOFILE` after about 500 requests and
stopped accepting. The write path now takes the existing dup and closes
it after the response. Linux builds also needed `<optional>` and
`<cstring>` included where libstdc++ does not pull them in transitively,
and the link line uses `-lstdc++` on Linux (`-lc++` on macOS).

The notes below are from the 2026-10-19 Rust runtime (commit `af5022e`).
They are not part of this binary.

Runtime bugs in `crates/marina-runtime/src/lib.rs` were found and fixed
during the October 19 benchmarks (otherwise the example segfaults
non-deterministically on both macOS and Linux):

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

On the October 19 Rust runtime, soak tests after those fixes: 200k GETs on Vultr → **RSS 4.4 MB** (before the
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

Raw outputs: `bench/raw/2026-10-21/`. Scripts: `bench/` (ports in `bench/{node,bun,go}/`).
