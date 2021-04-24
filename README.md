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

# Empirical data from day 2020/200

```
zrange = [ -336042201777 361882386254; -670837676888 669131761241; -1211061312768 1215589743403; -2420368136452 2421537307808; -4841905444260 4839570043541; ];
lrsb = [ 20 0 0 0 0 162;
 21 0 0 0 212 351103;
 22 0 0 456 366859 1557736;
 23 0 1512 558309 1930226 4059101;
 24 4333 454455 1916376 4685195 5256099;
 25 1353177 2513312 3722603 5426531 8372009;
 26 2701594 4866723 7368281 9424434 7134406;
 27 3117138 5573723 5471323 4392735 2953751;
 28 3546261 4850099 4147415 3740274 3526953;
 29 119132 200190 225899 274853 408335;
 30 65379 118493 169238 392326 691791;
 31 77281 139306 357862 429548 389273;
 32 247725 473644 552108 662908 1429722;
 33 102376 190998 510194 864591 665359;
 34 343889 661199 816162 989259 1319603;
 35 197965 315444 557391 796691 1117741;
 36 1918900 436506 728821 928329 1409173;
 37 33965251 567508 910555 1254160 1778968;
 38 61286221 623339 1130375 1307001 2013773;
 39 161115252 917346 1440117 1918350 3492622;
 40 170007065 1266706 2095429 3020883 4615526;
 41 1347485073 3024202 3892120 4848562 7752889;
 42 986213175 4726517 5731802 8158539 11725756;
 43 1606598836 9296251 9805604 12263196 14175470;
 44 1531796880 13676220 12912403 14537231 17917668;
 45 640850736 24620485 14371984 16709300 17282188;
 46 311936252 57828945 17687789 18201052 16305973;
 47 154698267 50157516 14822679 14448841 21212298;
 48 78214175 98979794 14164391 22591011 63785918;
 49 39691698 141904502 23214352 71008832 286416695;
 50 25280529 118429167 82136980 311523548 829488546;
 51 36830906 241633757 359236317 883712946 1336499442;
 52 80676462 478676125 924809626 1334695277 1398197194;
 53 283438540 1588266235 1380654767 1429044591 1391939785;
 54 661760572 2261866749 1494516804 1433180373 1444949800;
 55 623507465 1851322054 1404182678 1420313145 1328906723;
 56 566239942 1469164588 1417583729 1281626604 887828813;
 57 191441881 793854869 1279446697 896096361 526137454;
 58 2104808 333601156 793720070 470572922 258471749;
 59 1036666 87549130 244583296 137548936 74075636;
 60 517238 44281328 124322590 69093910 37084716;
 61 256987 20100748 61375558 34491379 18539930;
 62 122838 8989029 31202143 17309868 9258941;
 63 782177112 666231890 644604263 446181570 339772382;
]
./rinex_analyze ~/misc/crx/2020_200/*.20o  392.65s user 39.35s system 96% cpu 7:29.50 total

./rinex_scan ~/misc/crx/p052/p0520??0.20o  14.56s user 1.10s system 99% cpu 15.671 total
( for foo in ~/misc/crx/p052/p0520??0.20o; do; echo $foo; rnx2crx $foo - > ; )  246.94s user 4.43s system 99% cpu 4:11.65 total
./rinex_analyze ~/misc/crx/p052/p0520??0.20o  63.11s user 1.97s system 99% cpu 1:05.11 total
```

## Throughput benchmarks:

For `./rinex_scan 2020_200/m*.20o`, with ~42091 MB of input:
 - AMD Threadripper 3960X, -O3 -mavx2:
   `./rinex_scan_avx2 2020_200/m*.20o  18.75s user 2.52s system 99% cpu 21.280 total`
   2245 MB/sec user, 1979 MB/sec user+system, 1978 MB/sec wall clock
 - AMD Threadripper 3960X, -O3 but no -mavx2:
   `./rinex_scan_o3 2020_200/m*.20o  51.64s user 1.64s system 99% cpu 53.294 total`
   815 MB/sec user, 790 MB/sec user+system, 790 MB/sec wall clock
 - Intel Core i7-6700HQ, -O3 -mavx2:
   `./rinex_scan_avx2 2020_200/m*.20o  28.06s user 9.55s system 84% cpu 44.684 total`
   1500 MB/sec user, 1119 MB/sec user+system, 942 MB/sec wall clock
 - Intel Core i7-6700HQ, -O3 but no -mavx2:
   `./rinex_scan_o3 2020_200/m*.20o  88.98s user 7.59s system 99% cpu 1:36.79 total`
   473 MB/sec user, 436 MB/sec user+system, 435 MB/sec wall clock
 - Jetson Nano (MAXN / 10 W mode):
   `./rinex_scan 2020_200/m*.20o  308.63s user 32.11s system 69% cpu 8:11.67 total`
   136 MB/sec user, 124 MB/sec user+system, 85.6 MB/sec wall clock
 - Jetson Nano (5 W mode):
   `./rinex_scan 2020_200/m*.20o  449.44s user 36.72s system 89% cpu 9:01.08 total`
   93.7 MB/sec user, 86.6 MB/sec user+system, 77.8 MB/sec wall clock

Note the apparent disk bottlenecks for the laptop (AVX2 version) and
Jetson.  The workstation had 64 GiB RAM, allowing these files to be
processed from RAM.
