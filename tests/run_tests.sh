#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# JUNGLE's tests: the engine alone, the app in fm1-base's host simulator, the loader page against brk.c.
#   sh tests/run_tests.sh            (the loader's test needs Node and Playwright's Chromium; SKIP_LOADER=1 skips it)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CC="${CC:-cc}"
echo "== engine"
$CC -std=gnu99 -O2 -Wall -Werror -Ibase/include -Iapp tests/engine_test.c app/engine.c -lm -o build/engine_test
build/engine_test build/engine_test.wav
echo "== host simulator"
./build.sh --host > build/host_build.log 2>&1 || { cat build/host_build.log; exit 1; }
mkdir -p app/build/host
app/build/host/jungle_host app/tests/basic.txt app/build/host
echo "== loader page"
$CC -std=gnu99 -Wall -Werror -Iapp -Ibase/include tests/proto_dev.c -o build/proto_dev
if [ -z "$SKIP_LOADER" ]; then node tests/loader_test.mjs build/loader.png; fi
echo "all tests passed"
