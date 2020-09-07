# Succinct RINEX File Format

## Format Version

This file describes revision 1 of the Succinct RINEX (SRNX) file format.

## Introduction

The SRNX file format is a binary format that allows fast sequential
access to RINEX-like observation data on a per-signal basis, while still
being small compared to either raw RINEX or Hatanaka-compressed RINEX
files.

An SRNX file consists of a sequence of chunks, optionally followed by a
file-level digest to detect accidental corruption of the file.

Each chunk is identified by a four-byte tag (FOURCC).
The length of the chunk payload follows the FOURCC tag, allowing for
easy processing and walking of the file.
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

Conversion to and from SRNX format often changes the file contents, but
preserves the semantics.
For example, it ensures newlines are in Unix format, does not preserve
the order of satellites observed in each epoch, and does not preserve
leading zeros in observation values.

## SRNX FOURCC Codes

In the following table, Count indicates how many times the chunk may
occur in a single SRNX file.

| FOURCC | Count | Description |
| :----: | :----------: | :---------- |
| [SRNX](#srnx) | 1 | Succinct RINEX header |
| [RHDR](#rhdr) | 1 | RINEX (2.x or 3.x) file header |
| [SDIR](#sdir) | 0..1 | Satellite directory |
| [EPOC](#epoc) | 0..1 | Epoch record information |
| [EVTF](#evtf) | Any | Special event flag and data |
| [SATE](#sate) | Any | Satellite observation metadata |
| [SOCD](#socd) | Any | Satellite observation code data |

The first chunk of an SRNX file MUST be `SRNX`.
The second chunk of an SRNX file MUST be `RHDR`.
If an `EPOC` chunk is not present, the file MUST not have any `EVTF`,
`SATE` or `SOCD` chunks (it has no special events or observations).

Additional chunks, if any are present, MAY appear in any order, but:
 - If an `SDIR` chunk is present, it SHOULD be the third chunk.
 - The `EPOC` chunk SHOULD follow `SDIR` (or `RHDR`).
 - The file SHOULD store `EVTF` chunks consecutively.

# Chunk Payload Definitions

## <a name="srnx"></a>SRNX: Succinct RINEX Header

The `SRNX` payload consists of four ULEB128 values, in this order:

1. A major version identifier, currently 1.
1. A minor version identifier, currently 0.
1. A per-chunk digest identifier, from [below](#digests).
1. A file-level digest identifier.

The per-chunk and file-level digest identifiers identify a message
digest or checksum function.
The per-chunk digest is described above; the file-level digest is
calculated over the entire rest of the file.
(The length of the file for the file-level digest should be derived from
an external source, such as the file system that holds the SRNX file.)

## <a name="rhdr"></a>RHDR: RINEX Header

The `RHDR` payload is simply the RINEX 2.x or 3.x file header, including
the newline following the END OF HEADER label.
Each newline is represented as a single line feed (LF, '\n'; Unix
style).

## <a name="sdir"></a>SDIR: Satellite Directory

The `SDIR` payload consists of the file offset of the `EPOC` chunk,
followed by the file offset of the first `EVTF` chunk, followed by a
sequence of satellite directory entries.
If the `EVTF` file offset is zero, no special events are present.

Each satellite directory entry contains the three-character satellite
identifier, followed by the file offset of its `SATE` chunk.

## <a name="evtf"></a>EVTF: Event Flag or Special Record

The `EVTF` payload consists of an epoch index, followed by the event
record in standard RINEX format, including all newlines.

The epoch index is a ULEB128 indication of where the event record
exists relative to epochs: 0 indicates before the first epoch, 1
indicates between the first and second epoch, and so forth.

As in the `RHDR` chunk, each newline is represented as a single LF.

## <a name="epoc"></a>EPOC: Epoch records

The `EPOC` payload consists of a ULEB128 count of epochs, one or more
run-length-encoded (RLE'd) epoch spans, followed by RLE-encoded receiver
clock offsets.
For a RINEX 2.x-based file, the clock offsets should all be zero.

Each epoch span consists of a SLEB128 interval between epochs, the
ULEB128 count-minus-1 of epochs in the span, the date, and the initial
time-of-day, in that order.

A negative interval represents the negated number of seconds between epochs.
A positive interval represents the interval in seconds times 1e7.

The ULEB128 for date is year times 10000 plus month times 100 plus day.
(Years less than 100 are interpreted as in RINEX 2.x.)

The ULEB128 for the initial time-of-day is hours times 1e11 plus minutes
times 1e9 plus seconds times 1e7.

When adding the interval to the previous epoch's timestamp, minutes and
seconds reset to zero when the new sum is exactly 60, but hours do not
wrap.
Thus, leap seconds and day boundaries must be represented in a new RLE
entry.
Sub-second intervals during a leap second must not reset to 0 seconds.

The RLE-encoded receiver clock offsets are represented by a receiver
clock offset, represented as SLEB128 of the original value times 1e12,
followed by a ULEB count-minus-1 indicate the repeat count.
If not all receiver clock offsets are set, the remainder are zero.

The number of satellites observed in each epoch is not directly
represented in the SRNX file.

## <a name="sate"></a>SATE: Satellite observations

The `SATE` payload lists the observation times and codes for a single
satellite.
It consists of the satellite name with a trailing '\0' byte, SLEB128
file offsets for each observation code's `SOCD` block (relative to the
start of the `SATE` block), and epoch presence data, in that order.
The number of signals is controlled by the `RHDR` payload.
A zero file offset means that signal was never observed.

`SATE` chunks at different places in a file MUST NOT have the same name.

### Signal epoch presence

The epoch presence data is represented as a ULEB128 count-minus-1 of
runs, followed by interleaved counts-minus-1 of how many epochs the
satellite was absent and present, in that order.

For example, if this consists of the values 0 2 5, the satellite was
observed for six (5+1) epochs, starting at the third (2+1) epoch
described in the `EPOC` chunk.

## <a name="socd"></a>SOCD: Satellite observation code data

The `SOCD` payload holds the observations for a single combination of
satellite and observation code.
It consists of the observation name, a ULEB128 count-minus-1 of
observations, loss-of-lock indicators, signal-strength indicators, and
packed observation data.

### Observation name

The observation name is stored as an eight-byte name, with the satellite
name in the first three bytes, a zero ('\0') pad, the signal name in the
next two or three bytes (depending on RINEX version), and zero ('\0')
padding to fill out the eight bytes.

### Loss-of-lock indicators

Loss-of-lock indicators (LLIs) are represented with a ULEB128 length of
the compressed LLIs, in bytes of file content.
The compressed LLIs are run-length encoded, with the LLI character
(typically ' ', or '0' through '9') followed by a ULEB128 count-minus-1.

If the total number of encoded LLIs is smaller than the number of
observations given for the signal, the remainder are spaces (' ').

### Signal-strength indicators

Signal-strength indicators are represented using the same RLE encoding
as loss-of-lock indicators.

### Packed observation data

Packed observation data is represented as delta encoding of scaled
observation data.
Conceptually, the observation data is divided by the scale, then
its nth-order deltas are calculated (for 0 <= `n` < 8), and the deltas
are block-encoded.

The packed observation data begins with a ULEB128 value that identifies
the encoding schema: `n` for the delta order, plus 8 if an explicit
scaling value is present.

If an explicit scale is used, it is stored as a ULEB128 indicating the
scale times 1000.
For example, an explicit scale of 500 indicates that observation values
are quantized to half-unit values.
If no explicit scale is used, it is the same as 1: observation values
are multiplied by 1000 before delta encoding.
The scaled values MUST all be integers.

Following this are `n` SLEB128 values representing the initial state of
the delta coder.

Following this are zero or more blocks of packed last-level delta
measurements.
Each block begins with one byte that identifies the block packing.

| Block Header | Description |
| :----------: | :---------- |
| `000kkkkk` | 8*`(k+1)` bit matrix |
| `001kkkkk` | 16*`(k+1)` bit matrix |
| `010kkkkk` | 32*`(k+1)` bit matrix |
| `011kkkkk` | 64*`(k+1)` bit matrix |
| `11111110` | ULEB128 count-minus-1 of empty values |
| `11111111` | ULEB128 count-minus-1 of SLEB128 values |
| others | reserved |

An `m*(k+1)` bit matrix is stored as `m` values, each with `k+1` bits in
the file, in bitwise transposed order.
That is, the least significant bit from each value is stored first, with
the LSB of the first value in the LSB of the first byte and continuing
for `m/8` bytes.
The `k+1` bits are in two's-complement format, so the `k+1`'th bit may
need to be repeated to sign-extend the values.

The use of this bit matrix representation was inspired by
[Lemire and Boytsov, 2013](https://onlinelibrary.wiley.com/doi/full/10.1002/spe.2203).

# <a name="digests"></a>Digest Identifiers

## Table of Digest Functions

| Identifier | Length | Description |
| :--------: | :----: | :---------- |
| 0 | 0 | Null digest |
| 2 | 4 | CRC32C |
| 6 | 32 | SHA-256 |

Other digest values are reserved for future definition.
However, excluding the null digest, the four LSBs of the identifier are
inteneded to indicate the digest length in bytes:

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

## <a name="digest-0"></a>Digest 0: Null digest

The null digest has length zero.

## <a name="digest-2"></a>Digest 2: CRC32C

The CRC32C digest is a 32-bit cyclic redundancy checksum using the
generator polynomial given by G. Castagnoli et al.
A formal definition is given in [RFC 3720](https://tools.ietf.org/html/rfc3720), section 12.1.

## <a name="digest-6"></a>Digest 6: SHA-256

The SHA-256 digest is a cryptographic message digest defined by the
United States government.
A formal definition is given in [FIPS 180-4](https://csrc.nist.gov/publications/detail/fips/180/4/final).

## Rationale for digest choices

The selection of defined digest functions was a balance between a
variety of lengths, robustness to random errors, and the availability
and efficiency of off-the-shelf implementations.
