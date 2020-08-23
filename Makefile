all: librinex.a rinex_analyze rinex_scan rinex_test hash_test

CFLAGS = -Wall -Wextra -Werror -O3 -g -march=native
CXXFLAGS = -Wall -Wextra -Werror -O3 -g -march=native

.PHONY: clean
clean:
	rm -f librinex.a *.o rinex_analyze rinex_scan hash_test

librinex.a: driver.o rinex_mmap.o rinex_parse.o rinex_stdio.o
	ar crs $@ $?

rinex_analyze: rinex_analyze.cc librinex.a

rinex_n_obs: rinex_n_obs.c librinex.a

rinex_scan: rinex_scan.c librinex.a

rinex_test: rinex_test.c librinex.a
