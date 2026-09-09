# kvstore

A persistent key-value storage engine written from scratch in C++20, with a TCP
server in front of it.

There are two storage engines behind one interface, with genuinely different
tradeoffs.

**Bitcask**: an append-only log plus an in-memory hash index mapping every key to
the offset of its most recent record. A read is one hash lookup and one seek — as
fast as a point read gets — but the whole keyspace must fit in memory, and the
records are in write order, so there are no range queries.

**LSM tree**: a sorted memtable fronted by a write-ahead log, flushed to
immutable sorted tables, with bloom filters and leveled compaction. Only a sparse
index and a filter stay resident, so the keyspace is bounded by disk rather than
RAM, and everything on disk is ordered, so range scans are a merge. A read may
consult more than one file, which is the price.

The emphasis throughout is **systems engineering rather than algorithms**. The
data structures involved are ordinary. The interesting questions are when a write
is actually durable, what the file looks like after the machine loses power
mid-append, who owns a file descriptor and when it closes, and what a reader does
with the half-record it finds at the tail.

No third-party storage or networking libraries. zlib is used for `crc32` and
GoogleTest for the test suite; everything else is built directly on `pread`,
`pwrite`, `fsync` and the BSD sockets API.

---

## What's implemented

**Storage — Bitcask engine**

- Append-only log with CRC-framed, self-describing records
- In-memory hash index (key → file id, offset, length, timestamp)
- Tombstone deletes — an append-only file cannot erase, so "deleted" is a record
- Configurable durability: `fsync` per write, or let the OS flush
- **Crash recovery** — replays the log in order, cuts a torn tail off the active
  file, and refuses to open a sealed file that fails its checksum rather than
  serving wrong data
- **File rotation** — the active log is sealed at a size threshold
- **Compaction** — merges sealed files, dropping superseded values and
  tombstones, via write-temp → fsync → atomic rename, so there is no window in
  which data is unreachable
- **Hint files** — index-only sidecars that let recovery skip reading the data

**Storage — LSM engine**

A second engine alongside the first, not a replacement. Both implement the same
`KVStore` interface and both are run through the same contract suite.

- **Sorted memtable** fronting the write path, so a flush is a single forward
  pass with no sorting step in it
- **Write-ahead log** — every write is durable before it is visible, so an
  unflushed memtable survives a crash. Unlike Bitcask's log, it is a recovery
  scratchpad: it is never read except by recovery, and is deleted on flush
- **SSTables** — immutable, block-structured, sorted files with a per-block CRC,
  a sparse index (one key per block), a bloom filter and a fixed-size footer
- **Bloom filters** — one per table, consulted before any disk access, so an
  absent key is rejected from RAM. No false negatives, ~1% false positives at ten
  bits per key
- **Leveled compaction** — L0 merges into levels of disjoint key ranges, so a
  lookup consults at most one table per level and space amplification stays near
  one copy. Runs automatically in bounded steps, or on demand in full
- **A manifest** describing which tables live at which level, rewritten whole and
  replaced atomically. Its rename is the engine's only commit point, which
  reduces crash recovery to one rule: delete every table the manifest does not
  name
- **Range scans** — `scan(begin, end)` merges the memtable and every SSTable in
  key order, newest version winning and deleted keys skipped
- **Memory-mapped reads** (opt-in) over the immutable tables — no syscall and no
  copy, with the `pread` path kept alive and tested to return byte-identical
  results
- **Reader/writer concurrency** — readers run in parallel with each other and
  with a writer; flushes and compactions happen on a background thread and are
  installed by swapping an immutable version object, so a reader never blocks on
  one and never sees a half-installed file set
- Crash-safe throughout: write-temp → fsync → atomic rename → commit the manifest
  → delete what it no longer names, in that order, so a crash at any point
  recovers to the same contents

This lifts Bitcask's two structural limits — only a sparse index and a bloom
filter stay in RAM rather than a pointer per key, and everything on disk is
ordered — at the cost of write amplification, which is the trade leveled
compaction makes deliberately.

**Network**

- TCP server, length-prefixed binary protocol, thread per connection
- Correct handling of partial reads and of multiple messages arriving together
- Frame size cap, so a 4-byte length prefix cannot be used to request a 4 GB
  allocation
- `kv-cli`, a one-shot client

**Throughout**

- Errors are returned as a `Status` value, never thrown — failure is routine in a
  storage engine, and a returned status is visible at the call site
- Every OS handle is owned by a move-only RAII type that closes it on every exit
  path
- Raw POSIX I/O, with a single shim file translating to Win32 where they differ

---

## Design notes

**Durability is a decision, not a side effect.** `write()` copies bytes into the
page cache and returns; at that moment the data survives the process dying but
not the machine dying. `fsync()` is what pushes it to the device and waits. The
default is to fsync every write, because a store that loses acknowledged writes
on a power cut should be something you opt into.

**A record proves its own integrity.** Every record carries a CRC covering
everything after it, so a crash mid-append leaves bytes that are detectably
inconsistent rather than silently wrong. Recovery uses this to tell a torn tail
apart from real damage — and the distinction is made not by inspecting the
damage but by knowing *where* the reader was when the data ran out.

**TCP has no message boundaries.** It guarantees the bytes arrive in order and
nothing else; a single `send` may surface as three `recv`s, or two messages may
arrive glued together. The protocol therefore carries its own framing, and the
reader keeps surplus bytes between calls because they are the start of the next
request. Losing them is a bug that only appears under load.

**The two engines are served differently, and the difference is the point.**
Bitcask is single-threaded, so every call goes through one mutex at the server
boundary — which makes the server exactly as concurrent as the engine, namely not
at all. The LSM engine is served directly. It splits its locking in two: writers
are serialised against each other, while readers hold a shared lock only long
enough to probe memory, then do every bloom check, index search and block read
with no lock held at all. The expensive part of a write — the `fsync` — happens
outside the lock readers contend on.

**A reader never waits for a flush or a compaction.** Those run on a background
thread and are installed by swapping a pointer to an immutable snapshot of the
file set, so a reader sees the whole old set or the whole new one and never a
blend. A retired file is not deleted while anyone is still reading it: the last
reference to go unmaps, closes and unlinks it.

**The server never names the storage engine.** It holds the abstract `KVStore`
interface and calls three virtual methods. The test suite exercises this by
running the identical round trips against an in-memory store, with no disk in the
process at all.

---

## Building

Requires CMake 3.16+ and a C++20 compiler. zlib and GoogleTest are fetched
automatically.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Sanitizer builds (GCC/Clang):

```sh
cmake -S . -B build-asan -DENABLE_SANITIZERS=ON    # ASan + UBSan
cmake -S . -B build-tsan -DENABLE_TSAN=ON          # ThreadSanitizer
```

`ENABLE_SANITIZERS` and `ENABLE_TSAN` are mutually exclusive and need separate
build trees.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The benchmark harness is a separate target and is not run by `ctest` — it
measures rather than asserts:

```sh
./build/kv_bench --help
```

378 tests. The suite covers the record codec, recovery from hand-corrupted logs,
compaction interrupted at each stage it can fail at, the SSTable and manifest
formats against flipped bytes and truncated files, bloom filters for false
negatives over 100k keys, flush and compaction interrupted before and after the
commit point, the wire codec against literal expected bytes, framing against
messages deliberately split and glued, the server end to end over real loopback
sockets, and concurrent readers against a writer while flushes and compactions
run underneath them.

A single contract suite defines what a `KVStore` must do, and every
implementation is run through it — the in-memory store, both disk engines, the
lock wrapper, and a client talking to a live server over TCP. A network hop is
not allowed to quietly change the semantics, and neither is a different storage
design.

Verified on MSVC 2019 and GCC 13, clean under AddressSanitizer,
UndefinedBehaviorSanitizer, LeakSanitizer and ThreadSanitizer. The ThreadSanitizer
run is the one that matters for the concurrent engine — it found two real data
races that every other instrument and all 378 tests missed.

## Try it

```sh
./build/kvstore_server --dir ./demo-db --port 7379

# elsewhere
./build/kv-cli --port 7379 put user:1 alice     # OK
./build/kv-cli --port 7379 get user:1           # alice
./build/kv-cli --port 7379 delete user:1        # OK
./build/kv-cli --port 7379 get user:1           # NotFound: no such key: user:1
```

Kill the server, restart it against the same directory, and the data is still
there — including the deletions.

A successful `get` writes the value to stdout as raw bytes with no trailing
newline, so `kv-cli get k > file` reproduces exactly what was stored. Values are
arbitrary byte strings and may contain NULs.

## Wire protocol

Length-prefixed binary framing over TCP:

```
+------------------+---------------------------+
| length (4B, BE)  | payload (`length` bytes)  |
+------------------+---------------------------+
```

Full specification in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Layout

```
include/kvstore/   public headers
src/               implementation, plus private headers
apps/              kvstore_server, kv-cli
bench/             kv_bench, the measurement harness
tests/             GoogleTest suites and shared fixtures
docs/              protocol specification
```

## Two engines, measured

The point of building both is that the trade between them is real and can be
shown rather than asserted. `kv_bench` reports throughput, latency and the three
amplification factors; 200,000 keys of 100 bytes:

```
./build/kv_bench --engine lsm --workload randread --keys 200000
./build/kv_bench --engine lsm --workload missing  --keys 200000
./build/kv_bench --engine bitcask --workload randread --keys 200000
```

|                        | Bitcask       | LSM (present keys) | LSM (absent keys) |
|------------------------|---------------|--------------------|-------------------|
| point reads            | 43,424 ops/s  | 16,674 ops/s       | 81,464 ops/s      |
| median latency         | 19.3 µs       | 53.9 µs            | —                 |
| read amplification     | 1 seek        | **1.000** blocks   | **0.008** blocks  |
| space amplification    | 1.19×         | 1.10×              |                   |
| RAM per key            | ~30 bytes     | ~1.25 bytes        |                   |

**Bitcask wins point reads by 2.6×, and that is the correct result.** One hash
lookup and one seek will always beat a bloom probe, an index search and a block
read. What it costs is a pointer in memory for every key in the database.

Read amplification of exactly **1.000** for present keys is the leveled invariant
paying out: below L0 the key ranges within a level are disjoint, so a lookup
considers at most one table per level and reads exactly one block.

Read amplification of **0.008** for absent keys is the bloom filters: of 199,987
consulted, 198,333 answered "definitely not here" without touching a disk. The
0.8% that got through is the false-positive rate, against a design target of about
1% at ten bits per key.

Memory-mapped reads are about 24% faster than `pread` on the same workload
(25,127 vs 20,237 ops/s) and are opt-in rather than default, because a read error
under a mapped page arrives as `SIGBUS` and kills the process, where `pread`
returns an error this engine can report.

*(Debug build on a laptop. The ratios are the meaningful part; the absolute
throughput is not.)*

## Roadmap

Implemented: append-only log with recovery, rotation, compaction, hint files,
TCP server with a framed protocol; and a second, LSM-style engine — sorted
memtable, write-ahead log, immutable block-structured SSTables, bloom filters,
leveled compaction behind an atomically-committed manifest, and range scans.

Both engines are complete. Possible next steps, none of them started: a
hand-written skip list for the memtable (it would remove the last lock a reader
takes), sequence numbers and snapshots, a block cache, and per-block bloom filters
so filter memory tracks the working set rather than the keyspace.

## Licence

MIT
