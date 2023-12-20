#! /usr/bin -e

if test -f +build ; then
    rm -r +build/meson-logs/coveragereport || true
else
    meson setup --buildtype debug -D b_coverage=true +build
fi
ninja -C +build -t clean
# rm +build/*/*.gcno +build/*/*.gcda
meson test -C +build
meson test -C +build --benchmark
ninja -C +build coverage
open +build/meson-logs/coveragereport/index.html
