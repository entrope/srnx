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
| 246-255 | QZSS reserved

Simplicity and expandability seems preferable, so this library keeps a
character to identify the system and a second byte (0 to 99) to identify
the satellite.

### Observation codes

RINEX 2.11 defines 26 observation codes, out of about 40 potential names
using the general naming scheme.

Defined: {C,L,D,S}{1,2,5,6,7,8} plus P1 and P2
Pattern: {C,D,L,P,S}{1,2,3,4,5,6,7,8}

RINEX 3.05 defines 266 distinct observation codes, out of about 576
potential names (potentially 936 considering the whole alphabet as
candidates for the third character).  The number of observation codes
defined for a given GNSS system is smaller: at most 120 (for BeiDou,
considering the 1I/Q/X synonyms for 2I/Q/X).

Defined: See the RINEX 3.0x and 4.00 specification.
Pattern: {C,L,D,S}{1,2,3,4,5,6,7,8,9}{A,B,C,D,E,I,L,M,N,P,Q,S,W,X,Y,Z}

RINEX 3.x and 4.x also allow receiver channel numbers to be recorded as
pseudo-observables "X1 " through "X9 ", with blank attributes.

### Satellite indexing

RINEX v2.11 defines four satellite system codes: G, R, S, E, and for
GPS-only observation files, blank.
RINEX v3.04 added J, C, and I.

Letters plus space will be distinct in the five LSBs, allowing for easy
lookup into a reasonably-sized table.

## In-memory storage

Rather than hashing sv+obs, it is better to use a lookup tree: Index by
system identifier & 31 to get an offset into a table of all satellites.

Each satellite has per-signal tables:

- Number of observations used & allocated
- Number of runs
- Latest epoch # observed
- Interleaved gap/run length array
- Observations (across all runs)
- LLI (across all runs)
- SSI (across all runs)

## Build system

This uses [Meson](https://mesonbuild.com/) to build.  Typical usage:

```bash
meson setup --buildtype debug -D b_coverage=true +build
meson test -C +build
meson test -C +build --benchmark
ninja -C +build coverage-html
sensible-browser +build/meson-logs/coveragereport/index.html
```

For an optimized build, use something like:

```bash
meson setup +release
meson compile -C +release
meson test -C +release --benchmark
```

## Generating flame graphs

```bash
perf record -F399 -g ...
perf script | stackcollapse-perf.pl > out.perf-folded && flamegraph.pl out.perf-folded > perf-scan.svg
```

```bash
# This doesn't require sudo, but it is not clear how to extract the traces.
# xctrace record --template 'Time Profiler' --launch -- ...

sudo dtrace -x ustackframes=100 -n 'profile-997 /execname == "rinex_scan" && arg1/ {  @[ustack()] = count(); } tick-60s { exit(0); }' -o scan.dtr
stackcollapse.pl scan.dtr | flamegraph.pl > flames-scan.svg
```

### Throughput benchmarks

For `./rinex_scan 2020_200/m*.20o`, with 42,091,216,806 bytes of input:

- AMD Threadripper 3960X, -O3 -march=native -fprofile-use -fprofile-correction (after -fprofile-generate):
   `./rinex_scan 2020_200/m*.20o  13.85s user 1.28s system 99% cpu 15.125 total`
   3039 MB/sec user, 2782 MB/sec user+system, 2783 MB/sec wall clock
- Apple M2 Max, -O3 with NEON:
   `./+release/rinex_scan 2020_200/m*.20o  29.48s user 1.94s system 97% cpu 32.291 total`
   1428 MB/sec user, 1340 MB/sec user+system, 1303 MB/sec wall clock
- AMD Threadripper 3960X, -O3:
   `./rinex_scan 2020_200/m*.20o  48.45s user 0.96s system 99% cpu 49.423 total`
   869 MB/sec user, 852 MB/sec user+system, 852 MB/sec user+system
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
from RAM.  The ARM platforms ran from a USB3-attached SSD, with more
read throughput than the parser could support.
