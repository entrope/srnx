#! /usr/bin/env python3
# Usage: test_stdin.py <exe> <stdin>

import subprocess
import sys

exename = sys.argv[1]
inname = sys.argv[2]
infile = open(inname)
proc = subprocess.run([exename, '-'] + sys.argv[3:], stdin=infile, capture_output=True)
