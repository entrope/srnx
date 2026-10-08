# RINEX Decoding Utility

## File format notes

### Epoch flags

RINEX 2.11, 3.05 and 4.00 define seven epoch flags: 0, 1 and 6 use the
respective observation format; 2 through 5 use the special event format.

### Satellite names

RINEX 2.11 defines four satellite system identifiers; RINEX 3.05 and
4.00 define seven.  Each identifies up to 100 satellites, although 00 is
presumably reserved, and the typical count is much smaller.

For example, SBAS PRNs range from 120 through 158 inclusive, so never
use 00 through 19 or above 58; QZSS uses only 01 through 10; NavIC
(also called IRNSS) uses only 1 through 9.

The SBAS satellite mask from DFMC SBAS gives a dense assignment for
four constellations plus SBAS:

| Index   | Assignment |
| :-----: | :--------- |
| 1-32    | GPS PRN |
| 33-37   | GPS reserved |
| 38-69   | GLONASS ID number plus 37 |
| 70-74   | GLONASS reserved |
| 75-110  | Galileo space vehicle identifier plus 74 |
| 111     | Galileo reserved |
| 112-119 | Spare |
| 120-158 | SBAS PRN |
| 159-195 | BDS ranging code number plus 158 |
| 196-207 | Reserved |
| 208-214 | Spare |

The following additional assignments could extend it:

| Index   | Assignment |
| :-----: | :--------- |
| 216-224 | NavIC PRN ID plus 215 |
| 225-235 | NavIC reserved |
| 236-245 | QZSS, as RINEX 4.00 (Table 6) signal ID plus 235 |
| 246-255 | QZSS reserved |

Simplicity and expandability seems preferable, so this library keeps a
character to identify the system and a second byte (0 to 99) to identify
the satellite.

The seven satellite systems and their initial letter are:

G (ASCII 0b01000111) - GPS
R (ASCII 0b01010010) - GLONASS
S (ASCII 0b01010011) - SBAS payload
E (ASCII 0b01000101) - Galileo
C (ASCII 0b01000011) - BeiDou
J (ASCII 0b01001010) - QZSS
I (ASCII 0b01001001) - NavIC

Five LSBs are required to avoid conflicts.
However, we can multiply the system ID by 3. shift by 3, and mask by 7
to make them unique: E, G, J, R, S, I, C then map to 0..6.
This allows using a small(ish) table of satellites:

### Observation code

RINEX 2.11 defines 26 observation codes, out of about 40 potential names
using the general naming scheme.

Defined: {C,L,D,S}{1,2,5,6,7,8} plus P1 and P2
Pattern: {C,D,L,P,S}{1,2,3,4,5,6,7,8}

RINEX 3.05 defines 266 distinct observation codes, out of about 576
potential names (potentially 936 considering the whole alphabet as
candidates for the third character).
The number of observation codes defined for a given GNSS system is
smaller: at most 120 (for BeiDou, considering the 1I/Q/X synonyms for
2I/Q/X).

Defined: See the RINEX 3.0x and 4.00 specification.
Pattern: {C,L,D,S}{1,2,3,4,5,6,7,8,9}{A,B,C,D,E,I,L,M,N,P,Q,S,W,X,Y,Z}

RINEX 3.x and 4.x also allow receiver channel numbers to be recorded as
pseudo-observables "X1 " through "X9 ", with blank attributes.

### Indexing

A RINEX file is fundamentally a three-dimensional array of (value, loss
of lock indicator (LLI), signal strength indicator (SSI)) tuples,
indexed by epoch (time), satellite number and observable
code.
The sparsity of this array varies by dimension: a given satellite
typically has the same set of observables for almost all epochs where
it is observed, with variation only as the satellite rises and sets.
Those epochs each satellilte is visibile are normally contiguous within
a pass, where passes are separated by long intervals (on the order of
hours) where the satellite is not observed.
Some constellations have the same set of observables across all satellites,
and others have generations of satellites that feature new or changed
signals, such as GPS L5 and L2C.

The "observable code" arguably has further cardinality: beyond the SSI
and LLI indicators that go with every observable, there are typically
groups of four observables that go together for a single signal:
code-based pseudorange,
carrier-phase (or accumulated Doppler) range,
instantaneous Doppler shift,
and carrier-to-noise ratio.
Signals using (semi-)codeless processing, such as GPS P(Y), add more
complexity to this dimension.

Different analyses use different orders of these indices: For example,
most PNT solutions want to index by time and then satellite, whereas
time-series analyses index by satellite and then time.
Both of these work on a chosen subset of observable codes.
Similarly, the range of many observables can be compressed by storing
their differences relative to a reference observable for the same
satellite; for the case of carrier-phase vs pseudorange observations,
this may involve scaling before taking the differences.

### Satellite indexing

RINEX v2.11 defines four satellite system codes: G, R, S, E, and for
GPS-only observation files, blank.
RINEX v3.04 added J, C, and I.

Letters plus space are be distinct in the five LSBs, allowing for easy
lookup into a reasonably-sized table.

## In-memory storage

Rather than hashing sv+obs, it is better to use a lookup tree: Index by
system identifier & 31 to get an offset into a table of all satellites.

Each satellite could have per-signal tables:

- Number of observations used & allocated
- Number of runs
- Latest epoch # observed
- Interleaved gap/run length array
- Observations (across all runs)
- LLI (across all runs)
- SSI (across all runs)

## Build system

This uses [CMake](https://cmake.org/) to build. The commands below also
require Ninja, `pkg-config`, and the BLAKE3 C library providing the
`libblake3` pkg-config module.

```bash
cmake -G Ninja -B +debug -Wdev -DCMAKE_BUILD_TYPE=Debug
cmake --build +debug
ctest --test-dir +debug -L function --output-on-failure
```

The build generates integration-test fixtures from the sample data
tracked in this repository; no external RINEX corpus is needed for the
functional tests. Tests labeled `benchmark` are separate microbenchmarks
and are excluded by `-L function`.

For an optimized build, use something like:

```bash
cmake -G Ninja -B +release -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C +release
```

## Tools

### rnx2srnx

Converts RINEX or CRINEX observation data to SRNX. Complete epochs with
flags 1 (power failure) and 6 (cycle-slip records) are stored as RINEX
text in EVTF chunks, alongside special events with flags 2 through 5.
Only flag-0 epochs enter EPOC and the compressed observation columns;
EVTF records do not update or reset the delta predictor.

`srnx2rnx` restores these records in order. The streaming `rinex_parser`
API returns the satellite, observation and indicator arrays for flags
1 and 6 as it does for text input. The columnar `rinex_load` and
`srnx_load` APIs expose these records through `rinex_data.event`;
clients needing all observations must also process those events.
`rnx2srnx_search` includes their EVTF storage cost.

The SRNX major version remains 1. Older readers that assume EVTF only
contains flags 2 through 5 need updating to interpret the additional
record types.

By default, observation columns are written as revision 1.1 `SOC2`
chunks (minor version 1): each signal is predicted from a carrier phase
of the same satellite when that is smaller, signal-strength indicators
may be predicted from a signal-strength observation, and values use
split Golomb-Rice blocks.  Referenced chunks are written before the
chunks that use them.  `--socd` writes revision 1.0 `SOCD` chunks
instead, for readers that do not support revision 1.1.

The library reads both.  A `SOC2` column is decoded completely when it
is opened (its blocks hold up to 2048 values, and predictions need the
reference column), and each `srnx_reader` caches the reference columns
it decodes until it is closed.

### rnx2srnx_search

Compression-measurement clone of `rnx2srnx` used to evaluate which set
of SRNX bit-transposed block sizes minimizes the encoded byte count for
a corpus.  The format's block header byte affords up to seven distinct
matrix-block sizes; this tool lets each candidate set (any ascending
tuple of multiples of 8, must include 8) be measured.  Tuples longer
than seven (up to 64 sizes) are not encodable, but give lower bounds
and are the starting point for backward elimination (below).

It parses each input file once and then, for every requested tuple,
re-runs the block-selection DP and reports the would-be total file
size.  No SRNX bytes are written, so non-spec sizes (e.g. 24, 48) can
be tried freely.

```bash
rnx2srnx_search --tuple=8,16,32,64,128 \
                --tuple=8,16,24,32,64,128 \
                --tuples-file=tuples.txt \
                input.20o [input2.20o ...]
```

Output is tab-separated `<file>\t<tuple>\t<bytes>` on stdout.  Adding
`--timing` prints a `# ...` line per file on stderr with parse and
per-tuple evaluation times.

With `--soc2`, observation chunks are costed as revision 1.1 `SOC2`
chunks instead of `SOCD`: split-Rice blocks of the tuple's sizes,
cross-signal prediction from carrier phase, and SSI references, following
the `SOC2` encoder guidance in `Succinct_RINEX.md`.  For each signal it
keeps both the unpredicted and the predicted coding and counts whichever
is smaller under each tuple.  Costing `SOC2` is several times slower than
`SOCD`, so run large corpora with one process per file.

`--fixed-order` chooses each column's delta order once, under the first
tuple in the list, and reuses it for the rest.  This makes each later
tuple several times cheaper and rarely changes a result by more than a
few bytes, so it suits searches; confirm the final candidates without it.

### srnx_block_search

`srnx_block_search` (Go, in `srnx_block_search/`) runs `rnx2srnx_search`
over one or more corpus directories in parallel.  With `-tuples FILE`,
it ranks the tuples in that file per corpus and overall.  With
`-eliminate SIZES`, it searches for a good tuple by backward elimination:
starting from all the candidate sizes, it repeatedly removes the size
whose removal costs the fewest bytes until `-keep` sizes remain (8 is
never removed), printing every size's removal penalty at each step, then
tries single-size swaps until none helps, and finally compares the
result with `-baseline`.

```bash
srnx_block_search -tool +release/rnx2srnx_search \
    -tool-args "--soc2 --fixed-order" -sample 10 \
    -eliminate 8,16,24,32,40,48,56,64,96,128,192,256 \
    corpus_1hz/ corpus_30s/
```

`-sample N` uses every Nth file (by name) of each corpus, for faster
searches whose results can then be confirmed on the whole corpus.  The
objective is the total byte count over all the given corpora.

## Generating flame graphs

```bash
perf record -F399 -g ...
perf script | stackcollapse-perf.pl > out.perf-folded && flamegraph.pl out.perf-folded > perf-scan.svg
```

## Profiling via Instruments

```bash
xctrace record --template 'CPU Profiler' --launch -- ./+release/rinex_scan ...
```

Open the resulting *.trace folder in Instruments.app, via Finder or via
`open Launch_rinex_scan_*.trace`.

## Measuring performance

Measure RINEX v2 and v3, CRINEX, and SRNX separately. Also separate
streaming scans (`rinex_scan`), whole-file loading (`rinex_load_file`),
and conversion (`rnx2srnx` and `srnx2rnx`); they perform different work.
Digest and transpose microbenchmarks do not measure file parsing.

For each result, record:

- Git revision, compiler version, build flags, CPU, operating system,
  and SIMD selection (including any `RINEX_SIMD` override).
- Exact command, corpus identity or checksums, file formats, file count,
  and input bytes. For compression results, include output bytes and
  chunk/file digest settings.
- Whether input was cached in RAM or read from storage, storage device,
  warm-up procedure, and number of repetitions.
- Wall, user, and system times, and the median and range across runs.
  State whether throughput uses encoded input bytes or equivalent RINEX
  text bytes, and distinguish decimal MB/s from MiB/s.

Use equivalent observations across formats when comparing them. Keep
results for cached and uncached input separate, and include the revision
and measurement date with any published table.

### Historical throughput benchmarks: RINEX v2 scanning

The results below are retained for historical reference. They are
outdated measurements of the RINEX v2 reading path and do not describe
current RINEX v3, CRINEX, SRNX, whole-file loading, or conversion performance.
The original compiler details and cache conditions are included where
available; the results are not a controlled comparison of the current code.

For `./rinex_scan 2020_200/m*.20o`, with 42,091,216,806 bytes of input:

- AMD Threadripper 3960X, -O3 -march=native -fprofile-use -fprofile-correction (after -fprofile-generate):
   `./rinex_scan 2020_200/m*.20o  13.85s user 1.28s system 99% cpu 15.125 total`
   3039 MB/sec user, 2782 MB/sec user+system, 2783 MB/sec wall clock
- Apple M2 Max, -O3 with NEON:
   `./+release/rinex_scan 2020_200/m*.20o  29.39s user 15.88s system 84% cpu 53.489 total`
   1432 MB/sec user, 930 MB/sec user+system, 787 MB/sec wall clock
- AMD Threadripper 3960X, -O3:
   `./rinex_scan 2020_200/m*.20o  48.45s user 0.96s system 99% cpu 49.423 total`
   869 MB/sec user, 852 MB/sec user+system, 852 MB/sec wall clock
- Apple M2 Max, optimized build but no NEON:
   `./+release/rinex_scan 2020_200/m*.20o  54.14s user 3.82s system 99% cpu 58.018 total`
   777 MB/sec user, 726 MB/sec user+system, 725 MB/sec wall clock
- Intel Core i7-6700HQ, -O3 -mavx2:
   `./rinex_scan 2020_200/m*.20o  23.49s user 8.65s system 83% cpu 38.621 total`
   1792 MB/sec user, 1310 MB/sec user+system, 1090 MB/sec wall clock
- Intel Core i7-6700HQ, -O3 but no -mavx2:
   `./rinex_scan 2020_200/m*.20o  73.44s user 7.98s system 99% cpu 1:21.54 total`
   573 MB/sec user, 517 MB/sec user+system, 516 MB/sec wall clock
- Jetson Nano (MaxN mode, ARM Cortex-A57 up to 1479 MHz):
   `./rinex_scan 2020_200/m*.20o  284.48s user 77.68s system 95% cpu 6:18.62 total`
   148 MB/sec user, 116 MB/sec user+system, 111 MB/sec wall clock
- Jetson Nano (5W mode, ARM Cortex-A57 up to 918 MHz):
   `./rinex_scan 2020_200/m*.20o  412.55s user 111.94s system 90% cpu 9:41.93 total`
   102 MB/sec user, 80 MB/sec user+system, 72 MB/sec wall clock
- Raspberry Pi 4 Model B (ARM Cortex-A72), 64-bit mode, 1.5 GHz:
   4m10.758 user, 1m17.239 system, 5m42.485 total
   168 MB/sec user, 128 MB/sec user+system, 123 MB/sec wall clock
- Jetson Nano (MaxN mode, with current NEON optimizations):
   `./rinex_scan 2020_200/m*.20o  217.00s user 71.00s system 97% cpu 4:55.27 total`
   194 MB/sec user, 146 MB/sec user+system, 143 MB/sec wall clock

Note the apparent disk bottlenecks for the laptop AVX2 version.
The workstation had 64 GiB RAM, allowing these files to be processed
from RAM.
The Jetson and Raspberry Pi platforms ran from a USB3-attached SSD, with
more read throughput than the parser could support.
