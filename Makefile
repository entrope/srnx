all: librinex.a rinex_analyze rinex_scan transpose_test

CC = aarch64-linux-gnu-gcc
CFLAGS = -Wall -Wextra -Werror -O3 -g

.PHONY: clean
clean:
	rm -f librinex.a *.o *.s rinex_analyze rinex_scan transpose_test

librinex.a: driver.o rinex_mmap.o rinex_p.o rinex_parse.o rinex_stdio.o \
	srnx.o transpose.o
	ar crs $@ $?

rinex_analyze: rinex_analyze.c librinex.a

rinex_n_obs: rinex_n_obs.c librinex.a

rinex_scan: rinex_scan.c librinex.a

transpose_test: transpose_test.c librinex.a

%.s: %.c
	$(CC) $(CFLAGS) -S -o $@ $<

# For performance analysis, do something like this:
# make transpose.s
# (edit transpose.s to insert # LLVM-MCA-BEGIN / # LLVM-MCA-END pairs)
# llvm-mca --bottleneck-analysis --mcpu=cascadelake transpose.s
#
# x86 CPUs of interest: skylake, cascadelake, znver2
# ARM CPUs of interest: apple-latest, carmel (LLVM 11+)
