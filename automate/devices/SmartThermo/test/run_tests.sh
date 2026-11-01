#!/bin/bash
## SmartThermo host test: builds thermo_test with the system C++ compiler and runs it.
## Usage: test/run_tests.sh [thermo_test options], e.g. test/run_tests.sh --scenario seasons --days 7 --season winter
## Needs g++ (or set CXX=clang++) on Linux or macOS; no Arduino tools.
set -e

testDir="$(cd "$(dirname "$0")" && pwd)"
sketchDir="$(dirname "$testDir")"
buildDir="$testDir/build"
compiler="${CXX:-g++}"
mkdir -p "$buildDir"

## The sketch is compiled from a copy in build/ with its #include "creds.h" pointed at the test
## credentials (mocks/sim_creds.h), so the real (git-ignored) creds.h never reaches the test.
{ echo '#line 1 "SmartThermo.ino"'; sed 's|#include "creds.h"|#include "sim_creds.h"|' "$sketchDir/SmartThermo.ino"; } > "$buildDir/SmartThermo.cpp"
if ! grep -q '#include "sim_creds.h"' "$buildDir/SmartThermo.cpp"; then
  echo "run_tests.sh: SmartThermo.ino no longer has #include \"creds.h\"; update the sed line above" >&2
  exit 1
fi

"$compiler" -std=c++17 -O2 -g -Wall -Wextra \
  -I "$testDir/mocks" -I "$sketchDir" \
  -o "$buildDir/thermo_test" \
  "$buildDir/SmartThermo.cpp" "$sketchDir/wdog.cpp" \
  "$testDir/sim_hal.cpp" "$testDir/sim_house.cpp" "$testDir/sim_checks.cpp" \
  "$testDir/sim_scenarios.cpp" "$testDir/thermo_units.cpp" "$testDir/thermo_test.cpp"

cd "$testDir"
exec "$buildDir/thermo_test" "$@"
