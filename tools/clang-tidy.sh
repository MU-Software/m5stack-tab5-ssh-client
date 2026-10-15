#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Host test and simulator sources use their host builds' compile databases; firmware sources run in the IDF container
# against a clang-toolchain configure in build/clang (the GCC build's flags are not clang's).
set -euo pipefail
cd "$(dirname "$0")/.."
tests=() sim=() fw=()
for f in "$@"; do
    case $f in tests/*) tests+=("$f") ;; sim/*) sim+=("$f") ;; *) fw+=("$f") ;; esac
done
host() { # SOURCE_DIR BUILD_DIR FILES...
    local src=$1 db=$2
    shift 2
    [ $# -gt 0 ] || return 0
    [ -f "$db/compile_commands.json" ] || cmake -S "$src" -B "$db" > /dev/null
    "$(brew --prefix llvm)/bin/clang-tidy" --quiet -p "$db" --extra-arg=-isysroot"$(xcrun --show-sdk-path)" \
        --header-filter='/(main|tests|sim)/' "$@" 2>&1 | grep -v "warnings generated\.$" || true
    return "${PIPESTATUS[0]}"
}
host tests build/test/host-address-undefined "${tests[@]+"${tests[@]}"}"
host sim build/sim "${sim[@]+"${sim[@]}"}"
[ ${#fw[@]} -gt 0 ] || exit 0
docker compose --progress quiet run --rm -T --entrypoint bash idf -c '
    . "$IDF_PATH/export.sh" > /dev/null 2>&1
    if [ ! -f build/clang/compile_commands.json ] || [ sdkconfig.defaults -nt build/clang/sdkconfig ] ||
        [ main/CMakeLists.txt -nt build/clang/compile_commands.json ]; then
        rm -f build/clang/sdkconfig
        idf.py -B build/clang -D SDKCONFIG="$PWD/build/clang/sdkconfig" -D IDF_TOOLCHAIN=clang reconfigure > /dev/null 2>&1
    fi
    clang-tidy --quiet -p build/clang "$@" 2>&1 | grep -v "warnings generated\.$"
    exit "${PIPESTATUS[0]}"' _ "${fw[@]}"
