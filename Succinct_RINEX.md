# Succinct RINEX File Format

## General Overview

### Format Version

This file describes revision 1 of the Succinct RINEX (SRNX) file format.

### Introduction

The SRNX file format is a binary format that allows fast sequential
access to RINEX-like observation data on a per-signal basis, while still
being small compared to either raw RINEX or Hatanaka-compressed RINEX
files.

An SRNX file consists of a sequence of chunks, optionally followed by a
file-level digest to detect accidental corruption of the file.

Each chunk is identified by a four-byte tag (FOURCC).
The length of the chunk payload follows the FOURCC tag, allowing quick
seeks to chunks.
The chunk payload follows the payload length.
After the chunk payload is an optional per-chunk digest; when present,
this is calculated over the concatenation of the FOURCC, the payload
length, and the payload.

In most contexts, integers are encoded using little-endian base-128
representation.
When the integer is inherently unsigned, such as when it represents the
length of a chunk's payload, this is called ULEB128.
In a ULEB128 byte sequence, the seven least significant bits of each
byte correspond to the seven next least significant bits of the integer.
The most significant bit of each non-terminal byte is set, and the most
significant bit of the final byte is cleared.

To encode a signed integer in SLEB128, it is shifted left one bit and
xor'ed with a value consisting of the sign bit shifted right infinitely,
and the result is encoded as a ULEB128 value.
Odd ULEB128 values correspond to negative SLEB128 values, and the upper
bits indicate the magnitude of the value.
Google's Protocol Buffers refers to this as ZigZag encoding.

Many chunks contain an "offset" of a different chunk within the SRNX file.
Each offset is encoded in ULEB128 as the byte offset from the start of
the file.

Conversion to and from SRNX format often changes the file contents, but
preserves the semantics.
For example, it ensures newlines are in Unix format, does not preserve
the order of satellites observed in each epoch, and does not preserve
leading zeros in observation values.

### Mandatory and Prohibitive Words

The key words MUST, MUST NOT, SHOULD and MAY are intended to be
interpreted as described in [RFC 2119](https://tools.ietf.org/html/rfc2119)
when, and only when, they appear in all capitals.

### SRNX FOURCC Codes

In the following table, Count indicates how many times the chunk may
occur in a single SRNX file.

| FOURCC | Count | Description |
| :----: | :----------: | :---------- |
| [SRNX](#srnx) | 1 | Succinct RINEX header |
| [RHDR](#rhdr) | 1 | RINEX (2.x or 3.x) file header |
| [EPOC](#epoc) | 0..1 | Epoch record information |
| [EVTF](#evtf) | Any | Special event flag and data |
| [SOCD](#socd) | Any | Satellite observation code data |
| [SATE](#sate) | Any | Satellite observation metadata |
| [SDIR](#sdir) | 0..1 | Satellite directory |

The first chunk of an SRNX file MUST be `SRNX`.
The second chunk of an SRNX file MUST be `RHDR`.
If an `EPOC` chunk is not present, the file MUST NOT have any `EVTF`,
`SATE` or `SOCD` chunks (it has no special events or observations).

Additional chunks, if any are present, MAY appear in any order, but:

- The `EPOC` chunk, if present, SHOULD be the third chunk.
- The file SHOULD store `EVTF` chunks consecutively.
- The order of the table above means most offsets point "backwards"
  in the file, simplifying the creation of the SRNX file.
- Chunks with different FOURCC codes MAY be interleaved.
  This is helpful in reducing the offsets stored in the `SATE` chunks.

## Chunk Payload Definitions

### <a name="srnx"></a>SRNX: Succinct RINEX Header

The `SRNX` payload consists of five ULEB128 values and optional padding,
in this order:

1. A major version identifier, currently 1.
1. A minor version identifier, currently 0.
1. A per-chunk digest identifier, from [below](#digests).
1. A file-level digest identifier.
1. The file offset of the `SDIR` chunk, or zero if there is no such chunk.
1. Optional padding, which MUST be ignored when reading.

The per-chunk and file-level digest identifiers identify a message
digest or checksum function.
The per-chunk digest is described above; the file-level digest is
calculated over the entire rest of the file.
(The length of the file for the file-level digest should be derived from
an external source, such as the file system that holds the SRNX file.)

When creating the SRNX file, the offset of the SDIR chunk might be
unknown until all the other chunks are written.
Padding in this chunk provides for enough space to store that offset
once it is known.

### <a name="rhdr"></a>RHDR: RINEX Header

The `RHDR` payload is simply the RINEX 2.x or 3.x file header, including
the newline following the END OF HEADER label.
Each newline is represented as a single line feed (LF, '\n'; Unix
style).

### <a name="sdir"></a>SDIR: Satellite Directory

The `SDIR` payload consists of the file offset of the `EPOC` chunk,
followed by the file offset of the first `EVTF` chunk, followed by a
sequence of satellite directory entries.
If the `EVTF` file offset is zero, no special events are present.

Each satellite directory entry contains the three-character satellite
identifier as used in RINEX, followed by the file offset of the
satellite's `SATE` chunk.

### <a name="evtf"></a>EVTF: Event Flag or Special Record

The `EVTF` payload consists of an epoch index, followed by the event
record in standard RINEX format, including all newlines.

The epoch index is a ULEB128 indication of where the event record
exists relative to observation epochs: 0 indicates before the first
epoch, 1 indicates between the first and second epoch, and so forth.

`EVTF` chunks SHOULD be stored in order of increasing epoch index.

As in the `RHDR` chunk, each newline is represented as a single LF.

### <a name="epoc"></a>EPOC: Epoch records

The `EPOC` payload consists of a ULEB128 count of epochs, one or more
epoch spans, and run-length-encoded (RLE-encoded) receiver clock offsets.
For a RINEX 2.x-based file, the clock offsets SHOULD be omitted, which
(as described below) implies they are all zero.

Each epoch span consists of a SLEB128 interval between epochs, the
ULEB128 count-minus-1 of epochs in the span, the date, and the initial
time-of-day, in that order.

A negative interval represents the negated number of seconds between epochs.
A positive interval represents the interval in seconds times 1e7.

The ULEB128 for date is year times 10000 plus month times 100 plus day.
Years less than 100 are interpreted as in RINEX 2.x.

The ULEB128 for the initial time-of-day is hours times 1e11 plus minutes
times 1e9 plus seconds times 1e7.

When adding the interval to the previous epoch's timestamp, minutes and
seconds that are at least 60 MUST carry into the next higher unit,
except at the end of the day (to allow for representing 23:59:60 plus
any fraction as a leap second).
A timestamp greater than or equal to 23:59:61 is illegal.
The first epoch in each day MUST start a new span within the EPOC block.

The RLE-encoded receiver clock offsets are stored as a receiver clock
offset, represented as SLEB128 of the original value times 1e12,
followed by a ULEB count-minus-1 indicate the repeat count.
If there are fewer receiver clock offsets than epochs in the EPOC chunk,
the remainding receiver clock offsets are zero.

The number of satellites observed in each epoch is not directly
represented in the SRNX file.

### <a name="sate"></a>SATE: Satellite observations

The `SATE` payload lists the observation times and codes for a single
satellite.
It consists of the satellite name with a trailing '\0' byte, SLEB128
file offsets for each observation code's `SOCD` chunk (relative to the
start of the `SATE` chunk's FOURCC), and epoch presence data, in that
order.
The number of signals is controlled by the `RHDR` chunk payload.
A zero file offset means that signal was never observed.

A given satellite name MUST be used in at most one `SATE` chunk.

#### Satellite epoch presence

The epoch presence data within a `SATE` chunk is represented as a
ULEB128 count-minus-1 of runs, followed by that many interleaved pairs
of (a) a ULEB128 count of how many epochs the satellite was absent
before this run, and (b) a ULEB128 count-minus-1 of how many
consecutive epochs the satellite was then present.
The absent count MAY be zero (e.g., when the satellite is observed
starting at the first epoch); the present count is at least 1 by
construction.

For example, if this consists of the values `1 2 5 4 6`, the satellite
was observed during two (1+1) spans of epochs:

- six (5+1) epochs starting at the third (2+1) epoch described in the
  `EPOC` chunk, and
- seven (6+1) epochs starting at the 14th epoch (4+1 epochs after the
  last epoch in the first span).

### <a name="socd"></a>SOCD: Satellite observation code data

The `SOCD` payload holds the observations for a single combination of
satellite and observation code.
It consists of the observation name, a ULEB128 count-minus-1 of
observations, loss-of-lock indicators, signal-strength indicators, and
packed observation data.

#### Observation name

The observation name is stored as an eight-byte name, with the satellite
name in the first three bytes, a zero ('\0') pad, the signal name in the
next two or three bytes (depending on RINEX version), and zero ('\0')
padding to fill out the eight bytes.

#### Loss-of-lock indicators

Loss-of-lock indicators (LLIs) are represented with a ULEB128 length of
the compressed LLIs, in bytes of file content.
The compressed LLIs are run-length encoded.
The permitted LLI symbols are the eleven characters `" 0123456789"`.
Each run is encoded as a single ULEB128 value equal to
`(count - 1) * 11 + idx`, where `idx` is the zero-based index of the
symbol within `" 0123456789"` (0 for space, 1 through 10 for `'0'`
through `'9'`), and `count` is the run length.
An encoder MUST reject any LLI symbol that is not in this alphabet.

If the total number of encoded LLIs is smaller than the number of
observations given for the signal, the remainder are spaces (' ').

#### Signal-strength indicators

Signal-strength indicators are represented using the same RLE encoding
as loss-of-lock indicators (eleven-symbol alphabet, packed ULEB128 per
run).

#### Packed observation data

Packed observation data is represented as delta encoding of scaled
observation data.
Conceptually, the observation data is divided by the scale, then
its nth-order deltas are calculated (for 0 <= `n` < 8), and the deltas
are block-encoded.

The packed observation data begins with a ULEB128 value that gives the
length of the remaining packed observation data, followed by a ULEB128
value that identifies the encoding schema: the sum of the delta order `n`
and either 8 (if an explicit scaling value is present) or 0 (otherwise).

If an explicit scale is used, it is stored as a ULEB128 indicating the
value of the least significant bit in milli-units (equivalently, the
greatest common denominator of the observations in the `SOCD` chunk).
For example, an explicit scale of 500 indicates that observation values
are quantized to half-unit values.
If no explicit scale is used, it is the same as 1: observation values
directly represent milli-units before delta encoding.
The maximum scale is 1e6 units, represented as one billion in ULEB128.
The scaled values MUST all be integers.

Following this are `n` SLEB128 values representing the initial state of
the delta coder.

Following this are zero or more blocks of packed last-level delta
measurements.
Each block begins with one byte that identifies the block packing.

| Block Header | Description |
| :----------: | :---------- |
| `000kkkkk` | 8×`(k+1)` bit matrix |
| `001kkkkk` | 16×`(k+1)` bit matrix |
| `010kkkkk` | 24×`(k+1)` bit matrix |
| `011kkkkk` | 32×`(k+1)` bit matrix |
| `100kkkkk` | 40×`(k+1)` bit matrix |
| `101kkkkk` | 48×`(k+1)` bit matrix |
| `110kkkkk` | 112×`(k+1)` bit matrix |
| `111xxxxx` through `11111100` | reserved |
| `11111101` | ULEB128 count-minus-1 of absent values |
| `11111110` | ULEB128 count-minus-1 of zero values |
| `11111111` | ULEB128 count-minus-1 of SLEB128 values |

An absent-values block (`0xFD`) represents observations that were not
recorded for those epochs (encoded as `INT64_MIN` in the library).
Absent values MUST be excluded from GCD computation and from delta
encoding.
Accordingly, the delta accumulator is not advanced when decoding an
absent-values block; absent values are returned as `INT64_MIN` without
altering the decoder's running state.

An `m×(k+1)` bit matrix (where `m` is 8, 16, 24, 32, 40, 48, or 112)
is stored as `m` values, each with `k+1` bits in the file, in bitwise
transposed order.
That is, the most significant bit from each value is stored first, with
the MSB of the first value in the MSB of the first byte and continuing
for `m/8` bytes.
The full matrix is thus `k+1` rows, each `m/8` bytes long, in big-endian
bit and byte order, for a total of `(k+1) × m/8` bytes.
The `k+1` bits are in two's-complement format, so the first (most
significant) bit is repeated to sign-extend the values.

The use of this bit matrix representation was inspired by
[Lemire and Boytsov, 2013](https://onlinelibrary.wiley.com/doi/full/10.1002/spe.2203).

#### Encoder guidance

The variable block sizes (8, 16, 24, 32, 40, 48, 112) allow an encoder to adapt to local
variation in bit-widths.
Recommended encoding procedure for each (satellite, observation code):

1. Partition the observations into present and absent (`INT64_MIN`)
   sections.
   Absent values will be encoded with `0xFD` blocks and are excluded from
   all subsequent steps.

2. Calculate the scale factor as the greatest common denominator of the
   values that are present.
   (Absent values do not participate in this.)

3. Select delta order using some algorithm.
   For a given delta order, greedy block selection is suboptimal.
   Linear programming to find the true shortest path to the end of the
   observations is efficient.
   (Working backwards, choose the permissible header byte that results
   in the shortest coded form; this may be saved to guide the forward
   encoding pass.)

4. Encode the observations.
   Runs of absent values are encoded as absent-value (`0xFD`) blocks
   regardless of position in the sequence.
   If a region has `max_bw` > 32, or contains only a few values that
   would not fill an 8-element block (including at the end of the
   sequence), use zero-run (`0xFE`) or SLEB128-run (`0xFF`) blocks instead.

## <a name="digests"></a>Digest Identifiers

### Table of Digest Functions

| Identifier | Length | Description |
| :--------: | :----: | :---------- |
| 0 | 0 | Null digest |
| 2 | 4 | CRC32C |
| 4 | 16 | BLAKE3, 16-byte output |
| 5 | 32 | BLAKE3, 32-byte output |

Other digest values are reserved for future definition.
However, excluding the null digest, the four LSBs of the identifier are
intended to indicate the digest length in bytes:

| LSBs | Length |
| :--: | :----- |
| 0 | 1 |
| 1 | 2 |
| 2 | 4 |
| 3 | 8 |
| 4 | 16 |
| 5 | 32 |
| 6 | 64 |
| 7 | reserved |

### <a name="digest-0"></a>Digest 0: Null digest

The null digest has length zero.

### <a name="digest-2"></a>Digest 2: CRC32C

The CRC32C digest is a 32-bit cyclic redundancy checksum using the
generator polynomial given by G. Castagnoli et al.
A formal definition is given in [RFC 3720](https://tools.ietf.org/html/rfc3720), section 12.1.

### <a name="digest-4"></a>Digest 4: BLAKE3, 16-byte output

### <a name="digest-5"></a>Digest 5: BLAKE3, 32-byte output

The BLAKE3 digest is a cryptographic message digest from the
cryptographic research community.  It is based on the SHA-3
permutation with a keyless Merkle tree structure, providing high
throughput and parallelism.
A reference implementation and formal definition are available at
[github.com/BLAKE3-team/BLAKE3-specs](https://github.com/BLAKE3-team/BLAKE3-specs).
The `libblake3` library provides a convenient implementation.

The 16-byte and 32-byte output variants are produced by truncating
the first 16 or 32 bytes of the BLAKE3 output keying material.
As BLAKE3 targets 128 bits of security strength, using a longer output
should not provide any better detection of corrupted files.

### Rationale for digest choices

CRC32C is a short but fairly robust digest that is accelerated by
common CPUs, and is suitable as the per-chunk digest.
BLAKE3 is a high-speed cryptographically strong digest function, and
is suitable as the file-level digest; its length amortizes very well.
