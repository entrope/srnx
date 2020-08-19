all: librinex.a rinex_scan

# CC = clang
CFLAGS = -Wall -Wextra -Werror -O3 -g -march=native # -ftree-vectorize

.PHONY: clean
clean:
	rm -f librinex.a *.o

librinex.a: rinex_mmap.o rinex_parse.o rinex_stdio.o
	ar crs $@ $?

rinex_scan: rinex_scan.o librinex.a
