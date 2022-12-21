EXE = rinex_analyze rinex_maxima rinex_scan transpose_test
TESTS = test_rnx_v2
all: librinex.a $(EXE)

LIBTAP_HOME=../libtap

# CC = aarch64-linux-gnu-gcc
# CC = clang -finline-aggressive -finstrument-functions-after-inlining -fitodcallsbyclone -floop-splitting -flto -fnt-store=auto -funroll-loops -freroll-loops -fvectorize -fwrapv
# CC = gcc -fprofile-generate
# CC = gcc -fprofile-use -fprofile-correction
CFLAGS = -Wall -Wextra -Werror -g -flto -O3 -march=native

.PHONY: clean check
clean:
	rm -f librinex.a librinex_cov.a *.o *.s *.gcda *.gcno \
	coverage.css coverage.html coverage.*.html \
	$(EXE) $(TESTS)

check: $(TESTS)
	./test_rnx_v2 | ./tapview

LIBRINEX_OBJS = \
	driver.o \
	rinex_mmap.o \
	rinex_p.o \
	rinex_parse.o \
	rinex_stdio.o \
	rinex_buffer.o \
	transpose.o
# srnx.o

librinex.a: $(LIBRINEX_OBJS)
	ar crs $@ $?

rinex_analyze: rinex_analyze.c librinex.a
rinex_maxima: rinex_maxima.c librinex.a
rinex_n_obs: rinex_n_obs.c librinex.a
rinex_scan: rinex_scan.c librinex.a
transpose_test: transpose_test.c librinex.a

test_rnx_v2: test_rnx_v2.c librinex_cov.a
test_rnx_v2: CFLAGS += -I$(LIBTAP_HOME)
test_rnx_v2: LDFLAGS += -L$(LIBTAP_HOME) --coverage
test_rnx_v2: LDLIBS += -Wl,-rpath,$(LIBTAP_HOME) -ltap

librinex_cov.a: $(LIBRINEX_OBJS:.o=.cov.o)
	ar crs $@ $?

%.cov.o: %.c
	$(CC) $(subst -O3,-O0 --coverage,$(CFLAGS)) -c -o $@ $<

%.s: %.c
	$(CC) $(CFLAGS) -S -o $@ $<

# For performance analysis, do something like this:
# make transpose.s
# (edit transpose.s to insert # LLVM-MCA-BEGIN / # LLVM-MCA-END pairs)
# llvm-mca --bottleneck-analysis --mcpu=cascadelake transpose.s
#
# x86 CPUs of interest: skylake, cascadelake, znver2
# ARM CPUs of interest: apple-latest, carmel (LLVM 11+)
