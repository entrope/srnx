#! /usr/bin -e

ninja -C +build -t clean
# rm +build/*/*.gcno +build/*/*.gcda
meson test -C +build
meson test -C +build --benchmark
ninja -C +build coverage
open +build/meson-logs/coveragereport/index.html
