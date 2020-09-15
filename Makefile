all: librinex.a rinex_analyze rinex_scan rinex_test

CFLAGS = -Wall -Wextra -Werror -O3 -g -march=native

.PHONY: clean
clean:
	rm -f librinex.a *.o rinex_analyze rinex_scan

librinex.a: driver.o rinex_mmap.o rinex_p.o rinex_parse.o rinex_stdio.o \
	srnx.o
	ar crs $@ $?

rinex_analyze: rinex_analyze.c librinex.a

rinex_n_obs: rinex_n_obs.c librinex.a

rinex_scan: rinex_scan.c librinex.a
