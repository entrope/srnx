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
Pattern: {C,L,D,S}{1,2,3,4,5,6,7,8,9}{A,B,C,D,E,I,L,M,N,P,Q,S,W,Y,X,Z}

== Short signal names

Satellite and signal names can be combined into four base64 digits, with
a conveniently loose definition for the first and last digits:
 - The first digit is the satellite system identifier.
 - The second digit is the satellite number in base64.  If the number
   is above 63, the first digit could be converted to its lower case
   equivalent.
 - The third digit encodes the observation type and frequency number.
   The frequency number is equal to one plus the base64 digit modulo 9.
   The observation type is indexed into the string "CLDSP??" by the
   base64 digit divided by 9.
 - The fourth digit is the RINEX 3 signal ID, the last character of the
   three-character RINEX 3 signal name.  For a RINEX 2 signal, it is a
   space (' ') instead.

Alternatively, the satellite number and observation code can be
concatenated and padded to generate a 64-bit integer. This approach may
be more palatable: conversions are easier and sizes align with 64-bit
pointers.
