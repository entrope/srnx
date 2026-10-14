# Succinct RINEX File Format

## General Overview

### Format Version

This file describes revision 1.2 of the Succinct RINEX (SRNX) file
format.
Revision 1.1 adds the [`SOC2`](#soc2) chunk, an alternative to `SOCD`
that adds cross-signal prediction, signal-strength indicators predicted
from signal-strength observations, delta-coder resets, and split
Golomb-Rice coding of observation values.
Revision 1.2 adds the [`EPO2`](#epo2) chunk, an alternative to `EPOC`
that holds epoch times with the picosecond resolution of RINEX 4.02.
A file that uses only the chunks of an earlier revision remains a valid
file of that revision.

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
| [RHDR](#rhdr) | 1 | RINEX (2.x, 3.x or 4.x) file header |
| [EPOC](#epoc) | 0..1 | Epoch record information |
| [EPO2](#epo2) | 0..1 | Epoch record information, with picoseconds |
| [EVTF](#evtf) | Any | Special event flag and data |
| [SOCD](#socd) | Any | Satellite observation code data |
| [SOC2](#soc2) | Any | Satellite observation code data, version 2 |
| [SATE](#sate) | Any | Satellite observation metadata |
| [SDIR](#sdir) | 0..1 | Satellite directory |

The first chunk of an SRNX file MUST be `SRNX`.
The second chunk of an SRNX file MUST be `RHDR`.
A file MUST NOT have both an `EPOC` and an `EPO2` chunk.
In the rest of this document, the *epoch chunk* is whichever of the two
is present.
If neither is present, the file MUST NOT have any `EVTF`, `SATE`,
`SOCD` or `SOC2` chunks (it has no special events or observations).

Additional chunks, if any are present, MAY appear in any order, but:

- The epoch chunk, if present, SHOULD be the third chunk.
- The file SHOULD store `EVTF` chunks consecutively.
- The order of the table above means most offsets point "backwards"
  in the file, simplifying the creation of the SRNX file.
- Chunks with different FOURCC codes MAY be interleaved.
  This is helpful in reducing the offsets stored in the `SATE` chunks.

### Zero Values

Generally, values that are explicitly zero in an input file SHOULD be
represented as explicit zeros, rather than absent or missing values,
in SRNX.
The RINEX specifications say that missing or unknown data may be
represented as zero, but zero values are often meaningful for LLIs and
for Doppler measurements (especially for geostationary satellites).

## Chunk Payload Definitions

### <a name="srnx"></a>SRNX: Succinct RINEX Header

The `SRNX` payload consists of five ULEB128 values and optional padding,
in this order:

1. A major version identifier, currently 1.
1. A minor version identifier: 0 if the file uses only revision 1.0
   chunks, 1 if it contains any `SOC2` chunk but no `EPO2` chunk, or 2
   if it contains an `EPO2` chunk.
1. A per-chunk digest identifier, from [below](#digests).
1. A file-level digest identifier.
1. The file offset of the `SDIR` chunk, or zero if there is no such chunk.
1. Optional padding, which MUST be ignored when reading.

The per-chunk and file-level digest identifiers identify a message
digest or checksum function.
The per-chunk digest is described above; the file-level digest is
calculated over file from the start of the SRNX FOURCC until just before
the file-level digest.
(The length of the file for the file-level digest should be derived from
an external source, such as the file system that holds the SRNX file.)

A reader MUST reject a file whose minor version is greater than the
greatest minor version it supports, because the file may contain
chunks that the reader cannot decode.

When creating the SRNX file, the offset of the SDIR chunk might be
unknown until all the other chunks are written.
Padding in this chunk provides for enough space to store that offset
once it is known.

### <a name="rhdr"></a>RHDR: RINEX Header

The `RHDR` payload is simply the RINEX 2.x, 3.x or 4.x file header, including
the newline following the END OF HEADER label.
Each newline is represented as a single line feed (LF, '\n'; Unix
style).

### <a name="sdir"></a>SDIR: Satellite Directory

The `SDIR` payload consists of the file offset of the epoch chunk,
followed by the file offset of the first `EVTF` chunk, followed by a
sequence of satellite directory entries.
If the `EVTF` file offset is zero, no special events are present.

Each satellite directory entry contains the three-character satellite
identifier as used in RINEX, followed by the file offset of the
satellite's `SATE` chunk.

### <a name="evtf"></a>EVTF: Event Flag or Special Record

The `EVTF` payload consists of an epoch index, followed by one complete
record with epoch flag 1 through 6 in standard RINEX format, including
all newlines. Flags 1 and 6 use the observation-record layout: the epoch
header includes the satellite list where required, and all satellite
observation or cycle-slip fields follow, including continuation lines,
receiver clock offset, LLI and SSI fields. CRINEX input MUST be decoded
to RINEX text before storing it here.

A flag-1 or flag-6 record MUST NOT also be represented in the epoch
chunk, SATE, SOCD or SOC2. Its values do not participate in SOCD or SOC2 scaling,
delta encoding, cross-signal prediction or predictor state updates.
Reading EVTF does not reset a SOCD or SOC2 predictor.

The epoch index is a ULEB128 indication of where the event record
exists relative to flag-0 observation epochs: 0 indicates before the
first flag-0 epoch, 1 indicates between the first and second, and so
forth. A value equal to the epoch chunk's epoch count indicates a
trailing event.
Multiple EVTF records may share an index and timestamp; their original
relative order MUST be preserved. EVTF records themselves do not
increment this index.

`EVTF` chunks SHOULD be stored in order of increasing epoch index.

As in the `RHDR` chunk, each newline is represented as a single LF.

### <a name="epoc"></a>EPOC: Epoch records

The `EPOC` payload consists of a ULEB128 count of flag-0 epochs, epoch
spans covering that count, and run-length-encoded (RLE-encoded) receiver
clock offsets. A zero count has no following spans or clock offsets;
such a file may still contain EVTF records.
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

### <a name="epo2"></a>EPO2: Epoch records with picoseconds

RINEX 4.02 allows an epoch record to append five more digits to its
seconds field (which is then floored, rather than rounded, to 1e-7
seconds), giving picosecond resolution.
The `EPO2` payload represents such epochs.
It is exactly like the `EPOC` payload except for the units of time:

- A positive interval represents the interval in seconds times 1e12.
  (A negative interval still represents the negated number of seconds.)
- The ULEB128 for the initial time-of-day is hours times 1e16 plus
  minutes times 1e14 plus seconds times 1e12.

The earliest and latest epochs in an `EPO2` chunk MUST be less than 2^63
picoseconds (about 106.75 days) apart, so that the difference between
any two of its timestamps fits in a signed 64-bit integer.

An encoder SHOULD write an `EPO2` chunk only when some epoch in its
input has the extra digits of seconds, and an `EPOC` chunk otherwise.
When converting to RINEX, a reader SHOULD write the extra digits for
every epoch of an `EPO2` chunk (zero digits for an epoch that did not
have them), and SHOULD NOT write them for an `EPOC` chunk.

### <a name="sate"></a>SATE: Satellite observations

The `SATE` payload lists the observation times and codes for a single
satellite.
It consists of the satellite name with a trailing '\0' byte, SLEB128
file offsets for each observation code's `SOCD` or `SOC2` chunk
(relative to the start of the `SATE` chunk's FOURCC), and epoch presence
data, in that order.
The number of signals is controlled by the `RHDR` chunk payload.
A zero file offset means that signal was never observed.
A reader distinguishes `SOCD` from `SOC2` by the FOURCC at the offset;
one satellite MAY use both chunk types.

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
  epoch chunk, and
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

### <a name="soc2"></a>SOC2: Satellite observation code data, version 2

The `SOC2` payload holds the observations for a single combination of
satellite and observation code, like `SOCD`, but it can predict its
values from another observation code of the same satellite, predict
its signal-strength indicators from a signal-strength observation, and
code observation values with split Golomb-Rice blocks.

It consists of the following, in this order:

1. The observation name, exactly as in `SOCD`.
1. A ULEB128 count-minus-1 of observations, exactly as in `SOCD`.
1. Loss-of-lock indicators, exactly as in `SOCD`.
1. A ULEB128 [mode](#soc2-mode) value, which selects the SSI reference
   and the predictor kind.
1. [Signal-strength indicators](#soc2-ssi).
1. The [predictor](#soc2-predictor) fields, if any.
1. [Packed observation data](#soc2-packed).

In this section, the *satellite epochs* are the epochs in which the
satellite is present according to its `SATE` epoch presence data, in
time order; `k` indexes them from zero.
A *reference* is another observation code of the same satellite,
identified by its zero-based index in the `RHDR` list of observation
codes for the satellite's system (the same order as the `SATE` file
offsets).
Reference values are the reference's decoded observation values in
milli-units, after scaling and after any prediction of its own.

#### <a name="soc2-mode"></a>Mode

The mode value is `(s << 2) | k`:

- `s` selects the SSI reference: zero means there is none, and a
  nonzero value `j + 1` names reference `j`, which MUST be a
  signal-strength (`S`) observation code.
- `k` is the predictor kind (0, 1 or 2) described
  [below](#soc2-predictor).
  The value 3 means a ULEB128 predictor kind follows the mode value;
  every kind it can name (3 and above) is currently reserved.

The common cases, including an SSI reference to any of the first 31
observation codes, fit in one byte.

#### <a name="soc2-ssi"></a>SSI coding

- Without an SSI reference, the SSIs are coded as in `SOCD`: an
  eleven-symbol RLE, with each run encoded as `(count - 1) * 11 + idx`.
- With an SSI reference, the SSIs are a twelve-symbol RLE, with each
  run encoded as `(count - 1) * 12 + idx`.
  Symbols 0 through 10 are as in `SOCD`.
  Symbol 11 means the *predicted SSI* at that satellite epoch: the digit
  `max(1, min(9, floor(s / 6000)))`, where `s` is the reference value
  in milli-units (thousandths of dB-Hz for RINEX 3).
  Symbol 11 MUST NOT be used at a satellite epoch where the reference
  value is absent.

In both cases, the RLE is preceded by its ULEB128 length in bytes, and
SSIs beyond the encoded runs are spaces, exactly as in `SOCD`.

The predicted SSI follows the RINEX 3 definition of signal strength
indicators in steps of 6 dB-Hz.
Receivers that use a different mapping simply do not benefit from an
SSI reference; an encoder SHOULD use one only when it is smaller.

#### <a name="soc2-predictor"></a>Predictor

The predictor kind comes from the mode value, and its fields follow the
signal-strength indicators.

| Kind | Fields | Prediction `p[k]` |
| :--: | :----------------- | :---------------- |
| 0 | none | none: every present value is coded directly |
| 1 | ULEB128 reference `j`, SLEB128 `num`, ULEB128 `den` | `R[k] * num / den` |
| 2 | ULEB128 reference `j`, SLEB128 `num`, ULEB128 `den`, ULEB128 stencil, ULEB128 `max_span` | `(R[b] - R[a]) * U * num / ((t[b] - t[a]) * den)` |
| 3 and above | reserved | |

`R[k]` is the value of reference `j` at satellite epoch `k`.
`den` MUST be at least 1.
Kind 1 predicts from the reference's value; kind 2 predicts from the
reference's rate of change.

For kind 2, the stencil selects satellite epochs `a` and `b`: 0 means
`a = k - 1, b = k` (backward difference), 1 means `a = k - 1, b = k + 1`
(central difference), and 2 means `a = k, b = k + 1` (forward
difference); other stencil values are reserved.
`t[k]` is the timestamp of satellite epoch `k` from the epoch chunk, in
units of `1/U` seconds, counted continuously across day boundaries,
where `U` is 10^7 with an `EPOC` chunk and 10^12 with an `EPO2` chunk.
`max_span` is in the same units; zero means unlimited.

A prediction is *available* at satellite epoch `k` when:

- for kind 1, `R[k]` is present;
- for kind 2, `a` and `b` are both satellite epochs, `R[a]` and `R[b]`
  are both present, `t[b] > t[a]`, either `max_span` is zero or
  `t[b] - t[a] <= max_span`, and `(R[b] - R[a]) * U * num` fits in a
  128-bit signed integer.

Predictions MUST be computed exactly, with intermediate products held in
at least 128-bit signed integers, and the division rounded to the
nearest integer, with ties rounded away from zero.
The prediction is then reduced to the observation's scale (see below):
`q[k] = p[k] / scale`, rounded the same way.

The reference MUST NOT be the observation code of this chunk.
If the reference's own chunk is a `SOC2` chunk with a nonzero predictor
kind, that chunk's reference MUST have predictor kind 0 (or be a `SOCD`
chunk), so a prediction chain is at most two steps long and cannot be
cyclic.
An SSI reference is not part of the prediction chain, because it does
not affect observation values.

The intended uses are carrier-phase observations predicted from a
reference carrier phase on another frequency (kind 1, `num / den` equal
to the frequency ratio), pseudoranges predicted from carrier phase
(kind 1, `num / den` equal to the reference's wavelength in metres per
cycle), and Doppler predicted from the rate of a carrier phase (kind 2,
`num / den` equal to minus the frequency ratio).
See the [encoder guidance](#soc2-guidance) below.

#### <a name="soc2-packed"></a>Packed observation data and split-Rice blocks

Packed observation data in `SOC2` begins exactly as in `SOCD`: a
ULEB128 length of the remaining packed data, a ULEB128 encoding schema
(delta order `n` plus 8 if an explicit scale is present), the optional
ULEB128 scale, and `n` SLEB128 values of initial delta decoder state.
Blocks follow, each beginning with a one-byte header, and together
cover the satellite epochs in order.

Split-Rice, zero-value and SLEB128-value blocks hold last-level deltas.
Through the same delta decoder as `SOCD`, they yield one value `y` for
each present observation that is not in an unpredicted-values block.
With a nonzero predictor kind, a prediction is available for each of
these, and the observation is `(y + q[k]) * scale`.
With predictor kind 0, the observation is `y * scale`, as in `SOCD`.

| Block Header | Description |
| :----------: | :---------- |
| `000rrrrr` | split-Rice block of 8 values, Rice parameter `r` |
| `001rrrrr` | split-Rice block of 24 values |
| `010rrrrr` | split-Rice block of 56 values |
| `011rrrrr` | split-Rice block of 144 values |
| `100rrrrr` | split-Rice block of 240 values |
| `101rrrrr` | split-Rice block of 640 values |
| `110rrrrr` | split-Rice block of 2048 values |
| `111xxxxx` through `11111010` | reserved |
| `11111011` | delta decoder reset |
| `11111100` | ULEB128 count-minus-1 of unpredicted SLEB128 values |
| `11111101` | ULEB128 count-minus-1 of absent values |
| `11111110` | ULEB128 count-minus-1 of zero values |
| `11111111` | ULEB128 count-minus-1 of SLEB128 values |

Absent-value, zero-value and SLEB128-value blocks have the same meaning
as in `SOCD`.

The split-Rice block sizes differ from the `SOCD` bit-matrix sizes.
A split-Rice block's fixed cost (its header byte and quotient padding)
is small and does not grow with the block's largest value, so long
blocks pay off, especially for 1 Hz data.
These sizes minimized the total size of a corpus of 1,703 daily files,
mostly 1 Hz, among sets of seven sizes up to 2048.

A split-Rice block of `m` values with parameter `r` contains:

1. If `r > 0`, a bit matrix of `m` unsigned `r`-bit remainders, laid out
   as the `SOCD` bit matrix (`r` rows of `m/8` bytes, most significant
   row first, big-endian bit and byte order) but without sign extension.
1. The quotients: for each of the `m` values in order, `q` one bits
   followed by a zero bit, packed most significant bit first, and then
   zero bits up to the next byte boundary.

Each value is `u = q * 2^r + remainder`, which MUST be less than 2^64,
and the last-level delta is `u` decoded as in SLEB128 (ZigZag): `u / 2`
if `u` is even, or `-(u + 1) / 2` if `u` is odd.

A delta decoder reset block (`0xFB`) is followed by a ULEB128 new delta
order `n'` (0 through 7) and `n'` SLEB128 values.
The decoder discards its state and continues exactly as at the start of
the packed data with order `n'` and those initial state values.
The scale does not change, and the block itself contains no
observations.

An unpredicted-values block (`0xFC`) holds observations at satellite
epochs where this chunk's prediction is not available.
Each value is the scaled observation itself (the observation divided by
the scale), not a delta.
Like absent values, unpredicted values do not advance the delta decoder.
With a nonzero predictor kind, every present observation at a satellite
epoch without an available prediction MUST be coded in an
unpredicted-values block, and only those observations may be.
With predictor kind 0, unpredicted-values blocks MUST NOT be used.

#### <a name="soc2-guidance"></a>Encoder and reader guidance

The encoding procedure for `SOCD` applies, with these changes:

1. Choose a reference carrier phase for each satellite, typically the
   one present at the most epochs, and code it without prediction.
   For each other observation code, compute the residual
   `x[k] / scale - q[k]` at satellite epochs where a prediction is
   available, and use the predictor only if the result is smaller.
   The scale is the greatest common divisor of the observation values
   themselves; rounding the prediction to that scale keeps the residuals
   multiples of it.
   (Some receivers quantize pseudoranges to 2 cm; omitting this rounding
   destroys that common factor.)
1. Suggested predictors, with frequencies in Hz:
   - Carrier phase `L` on band `b` from reference phase on band `a`:
     kind 1 with `num / den = f[b] / f[a]`.
   - Pseudorange `C` or `P` on band `b` from carrier phase `L` on band
     `b` (or the reference phase on band `a`): kind 1 with
     `num / den = 299792458 / f`, where `f` is the frequency of that
     phase.
   - Doppler `D` on band `b` from carrier phase on band `a`: kind 2,
     stencil 1 (central difference), with `num / den = -f[b] / f[a]`.
   - For GLONASS FDMA signals, the frequencies depend on the
     satellite's frequency channel (from the `GLONASS SLOT / FRQ #`
     header record, or estimated from the data).
     The G1/G2 frequency ratio is 9/7 for every channel.
1. Choose the Rice parameter for each split-Rice block, and the block
   sizes, with the same shortest-path search as for `SOCD` bit matrices.
   Blocks whose values would need `r > 31` should use SLEB128-value
   blocks instead.

Decoding a `SOC2` chunk with a predictor requires its reference's
decoded values (and, for a two-step chain, that reference's reference).
Encoders SHOULD write a satellite's referenced chunks before the chunks
that use them, so that a sequential reader can decode them in file
order.
Readers that decode one observation code at a time SHOULD cache each
satellite's decoded reference columns (usually its carrier phases) and
reuse them for that satellite's other observation codes.
A reader that decodes epoch by epoch needs one satellite epoch of
look-ahead on the reference for central-difference predictors.

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
