# kvstore wire protocol, v1

A length-prefixed binary request/response protocol over TCP. One connection
carries any number of request/response pairs, in order.

The reasoning behind each decision is in `NOTES.md`, Phase 3. This file is the
reference: what the bytes are.

## Framing

TCP is a byte stream. It guarantees the bytes arrive, in order, and nothing
else — a sender's single `send()` of 40 bytes may surface at the receiver as one
`recv()` of 40, as three of 17/1/22, or glued to the front of the next message.
There is no message boundary below this layer, so every message carries its own:

```
+--------------------+---------------------------+
| length  (4B, BE)   | payload (`length` bytes)  |
+--------------------+---------------------------+
```

* `length` counts the payload only, not itself.
* Big-endian — network byte order. (The *on-disk* record format is little-endian;
  the two are deliberately separate and have separate encoders.)
* `length` of 0 is legal.
* **Maximum payload: 8 MiB** (`kMaxFrameSize`). A peer announcing more is refused
  and its connection is closed, before any buffer is sized from the number.

A reader must accumulate until it holds 4 bytes, decode the length, then
accumulate until it holds that many more. Bytes beyond the end of a frame are the
beginning of the next one and must be kept.

## Request payload

```
+---------+------------------+-------------+-----------------+
| op (1B) | key_size (4B BE) | key (bytes) | value (bytes)   |
+---------+------------------+-------------+-----------------+
```

| op     | value  | meaning                       |
|--------|--------|-------------------------------|
| `0x01` | GET    | read `key`                    |
| `0x02` | PUT    | write `value` at `key`        |
| `0x03` | DELETE | remove `key`                  |

* `key` is `key_size` bytes and may contain any byte, NUL included.
* `value` is **the remainder of the frame** and carries no length of its own —
  the frame length already bounds it. It may contain any byte and may be empty.
* GET and DELETE must carry no bytes after the key. Trailing bytes are rejected.
* `key_size` must not exceed the bytes remaining in the frame.
* An empty key is a well-formed *frame*. Whether it is a legal *request* is the
  storage engine's ruling, and it answers `InvalidArgument`.

## Response payload

```
+-------------+-----------------+
| status (1B) | body (bytes)    |
+-------------+-----------------+
```

| status | code              | meaning                                  |
|--------|-------------------|------------------------------------------|
| `0`    | `Ok`              | succeeded                                 |
| `1`    | `NotFound`        | GET on a key that is not there            |
| `2`    | `IOError`         | disk or socket failure                    |
| `3`    | `Corruption`      | stored bytes failed their checksum        |
| `4`    | `InvalidArgument` | the request broke the contract            |

The status byte maps one-to-one onto `kvstore::StatusCode`. There is no
network-specific code: a socket is an I/O device, and a failed one is `IOError`.

`body` is:

* the value, on a successful GET;
* the `Status` message text, on any non-`Ok` status;
* empty otherwise (successful PUT and DELETE).

## Errors and connection lifetime

Two kinds of failure, treated differently:

* **A malformed message** — unknown op, `key_size` past the end of the frame,
  trailing bytes on a GET — is answered with `InvalidArgument` and **the
  connection stays open**. The frame boundaries are still known, so the next
  message can be found.
* **A malformed stream** — a length above the cap, or the peer closing part-way
  through a frame — **closes the connection** without an answer. The position in
  the stream is no longer known, so there is nowhere to send a reply from and no
  way to find the next message.

Closing the connection at a frame boundary is an ordinary hang-up: the server
notices and reports nothing.

## Example

`PUT "ab" = "xyz"`

```
00 00 00 0A                 length = 10
02                          op = PUT
00 00 00 02                 key_size = 2
61 62                       "ab"
78 79 7A                    "xyz"
```

Response:

```
00 00 00 01                 length = 1
00                          Ok, empty body
```

`GET "ab"` on a missing key:

```
00 00 00 07                 length = 7
01                          op = GET
00 00 00 02                 key_size = 2
61 62                       "ab"
```

```
00 00 00 15                 length = 21
01                          NotFound
6E 6F 20 73 75 63 68 ...    "no such key: ab"
```
