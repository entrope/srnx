#! /usr/bin -e

if test -f +coverage ; then
    rm -r +coverage/coverage || true
else
    cmake -B +coverage -DCMAKE_BUILD_TYPE:STRING=Coverage -G Ninja -Wdev
fi
ninja -C +coverage -t clean
ninja -C +coverage
ninja -C +coverage test
ninja -C +coverage coverage
open +coverage/coverage/index.html
