# kvstore

A persistent key-value storage engine written from scratch in C++20, with a TCP
server in front of it.

The storage layer is **Bitcask**: an append-only log on disk plus an in-memory
hash index mapping every key to the offset of its most recent record. Writes are
sequential appends. Reads are one hash lookup and one seek. The whole keyspace
must fit in memory; the values need not.

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

**Storage**

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

**The engine is single-threaded, and the server says so honestly.** All access is
serialised behind one mutex at the server boundary. This makes the server exactly
as concurrent as the engine — which is to say not at all — and that is a
deliberate, temporary trade. Real read concurrency requires the index to be safe
to read while a writer appends, which is an engine change, not a lock change.

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

208 tests. The suite covers the record codec, recovery from hand-corrupted logs,
compaction interrupted at each stage it can fail at, the wire codec against
literal expected bytes, framing against messages deliberately split and glued,
and the server end to end over real loopback sockets.

A single contract suite defines what a `KVStore` must do, and every
implementation is run through it — the in-memory store, the disk engine, the lock
wrapper, and a client talking to a live server over TCP. A network hop is not
allowed to quietly change the semantics.

Verified on MSVC 2019 and GCC 13, clean under AddressSanitizer,
UndefinedBehaviorSanitizer, LeakSanitizer and ThreadSanitizer.

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
tests/             GoogleTest suites and shared fixtures
docs/              protocol specification
```

## Roadmap

Implemented: append-only log with recovery, rotation, compaction, hint files,
TCP server with a framed protocol.

Next: sorted string tables, bloom filters, leveled compaction, and reader/writer
concurrency inside the engine.

## Licence

MIT
