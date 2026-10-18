#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: tests/run.sh [SANITIZE (default address,undefined)] [host|linux]
set -euo pipefail
cd "$(dirname "$0")/.."
san=${1:-address,undefined}
where=${2:-host}
build=build/test/$where-${san//,/-}
work=$build/run
rm -rf "$work" && mkdir -p "$work"
trap 'docker compose --profile test down -t 1 > /dev/null 2>&1; rm -rf "$work"' EXIT

ssh-keygen -q -t ed25519 -N '' -C tab5-ssh-test -f "$work/key"
ssh-keygen -q -t ed25519 -N '' -C tab5-ssh-test-unknown -f "$work/unknown"
AUTHORIZED_KEY=$(cat "$work/key.pub") docker compose --progress quiet --profile test up -d --build sshd
port=$(docker compose port sshd 22 | cut -d: -f2)
for _ in $(seq 50); do
    ssh-keyscan -p "$port" -t ed25519 127.0.0.1 2> /dev/null | grep -v '^#' > "$work/host" && break
    sleep 0.2
done
fp=$(ssh-keygen -lf "$work/host" | cut -d' ' -f2)
if [ "$where" = linux ]; then name=sshd target="sshd 22"; else name="[127.0.0.1]:$port" target="127.0.0.1 $port"; fi
echo "$name $(cut -d' ' -f2- "$work/host")" > "$work/known_hosts"
echo "$name $(cut -d' ' -f1-2 "$work/unknown.pub")" > "$work/changed_hosts"
args="$target tester $work/key $work/unknown tab5-test-password $work/known_hosts $work/changed_hosts $fp"

run="cmake -S tests -B $build -DSANITIZE=$san > /dev/null && cmake --build $build -j8 > /dev/null && $build/c6_lease_test && $build/c6_pick_test && $build/c6_nvs_test && $build/config_test && $build/dns_message_test && $build/gesture_test && $build/hangul_test && $build/layout_test && $build/modem_test && $build/network_rules_test && $build/settings_test && $build/ssh_test $args && $build/sshd_test $work/key $work/unknown $work"
if [ "$where" = linux ]; then
    docker compose --progress quiet --profile test run --rm --no-deps --build sanitize sh -c "$run"
else
    sh -c "$run"
fi
