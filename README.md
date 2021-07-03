# File format notes

## Epoch flags

Both RINEX 2.11 and 3.04 define seven epoch flags: 0, 1 and 6 use the
respective observation format; 2 through 5 use the special event format.

## Satellite names

RINEX 2.11 defines four satellite system identifiers; RINEX 3.04 defines
seven.  Each identifies up to 100 satellites, although 00 is presumably
reserved, and the typical count is much smaller.

For example, SBAS PRNs range from 120 through 158 inclusive, so never
use 00 through 19 or above 58; QZSS uses only 01 through 09.

## Observation codes

RINEX 2.11 defines 26 observation codes, out of about 40 potential names
using the general naming scheme.

Defined: {C,L,D,S}{1,2,5,6,7,8} plus P1 and P2
Pattern: {C,D,L,P,S}{1,2,3,4,5,6,7,8}

RINEX 3.04 defines 258 distinct observation codes, out of about 576
potential names (potentially 936 considering the whole alphabet).

Defined: See the RINEX 3.xx specification.
Pattern: {C,L,D,S}{1,2,3,4,5,6,7,8,9}{A,B,C,D,E,I,L,M,N,P,Q,S,W,X,Y,Z}

## Satellite indexing

RINEX v2.11 defines four satellite system codes: G, R, S, E, and for
GPS-only observation files, blank.
RINEX v3.04 adds J, C, and I.

Letters plus space will be distinct in the five LSBs, allowing for easy
lookup into a reasonably-sized table.

# In-memory storage

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

# Generating flame graphs

perf record -F399 -g ...
perf script | stackcollapse-perf.pl > out.perf-folded && flamegraph.pl out.perf-folded > perf-scan.svg

## Throughput benchmarks:

For `./rinex_scan 2020_200/m*.20o`, with ~42091 MB of input:
 - AMD Threadripper 3960X, -O3 -mavx2:
   `./rinex_scan 2020_200/m*.20o  14.96s user 1.37s system 99% cpu 16.334 total`
   2814 MB/sec user, 2578 MB/sec user+system, 2577 MB/sec wall clock
 - AMD Threadripper 3960X, -O3 but no -mavx2:
   `./rinex_scan 2020_200/m*.20o  49.35s user 1.46s system 99% cpu 50.822 total`
   853 MB/sec user, 828 MB/sec user+system, 828 MB/sec user+system
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
