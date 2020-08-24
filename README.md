= File format notes

== Epoch flags

Both RINEX 2.11 and 3.04 define seven epoch flags: 0, 1 and 6 use the
respective observation format; 2 through 5 use the special event format.

== Satellite names

RINEX 2.11 defines four satellite system identifiers; RINEX 3.04 defines
seven.  Each identifies up to 100 satellites, although 00 is presumably
reserved, and the likely count is much smaller.

For example, SBAS PRNs range from 120 through 158 inclusive, so never
use 00 through 19 or above 58; QZSS uses only 01 through 09.

== Observation codes

RINEX 2.11 defines 26 observation codes, out of about 40 potential names
using the general naming scheme.

Defined: {C,L,D,S}{1,2,5,6,7,8} plus P1 and P2
Pattern: {C,D,L,P,S}{1,2,3,4,5,6,7,8}

RINEX 3.04 defines 258 distinct observation codes, out of about 576
potential names (potentially 936 considering the whole alphabet).

Defined: See the RINEX 3.xx specification.
Pattern: {C,L,D,S}{1,2,3,4,5,6,7,8,9}{A,B,C,D,E,I,L,M,N,P,Q,S,W,X,Y,Z}

== Satellite indexing

RINEX v2.11 defines four satellite system codes: G, R, S, E, and for
GPS-only observation files, blank.
RINEX v3.04 adds J, C, and I.

These are unique in their five LSBs: ' '=0x20, 'C'=0x43, 'E'=0x45,
'G'=0x47, 'I'=0x49, 'J'=0x4A, 'R'=0x52, 'S'=0x53.  We can use this to
speed up per-system information lookups; this extends to other capital
letters, should more be assigned.

= Other notes

== In-memory storage

Rather than hashing sv+obs, it is probably better to use a lookup tree:
Index by system identifier & 31, which has tables for each satellite
observed for that system (1..N).  Each satellite has per-signal tables:
 - Number of observations used & allocated
 - Padding/reserved word (or satellite name?)
 - Number of runs
 - Latest epoch # observed
 - Interleaved gap/run length array (w/ used & allocated)
 - Observations (across all runs)
 - LLI (across all runs)
 - SSI (across all runs)
Or maybe (system id) & 31 provides a base index and SV count, and all
satellites are then in one table.

== Generating flame graphs

perf record -F399 -g ...
perf script | stackcollapse-perf.pl > out.perf-folded && flamegraph.pl out.perf-folded > perf-scan.svg

== Compression thoughts

Candidate encoding schemes:
 - Observation epochs: quasi-RLE over HH:MM:SS.SSSSSSS, assuming
   60-second minutes (i.e. break out of RLE for leap seconds and at the
   end of each day)
 - Epoch indexes: RLE over presence bits
 - LLI, SSI: RLE
 - Observation data: Nth-order delta (1..5? typically 2..3) followed by:
   (1) base128 or (2) BP32 (with leftovers using base128).
   - State reset between runs or not?

Maybe the ideal compression is to use BP32-like encoding from Lemire &
Boytsov [1] with SIMD bit transpositions [2].  We can probably do better
than [2] by exploiting knowledge of our "matrix" width, particularly in
the common case that values can all be represented by s32 integers.
(Load 256-bit vectors w/ VPERMD, VPMOVMSKB w/ shifts, VPSRLD, repeat
last two steps as needed.)

[1]- https://onlinelibrary.wiley.com/doi/full/10.1002/spe.2203
[2]- https://mischasan.wordpress.com/2011/10/03/the-full-sse2-bit-matrix-transpose-routine/

== Empirical data from day 2020/200

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
