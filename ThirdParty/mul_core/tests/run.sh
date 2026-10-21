#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ -f UPSTREAM.json ]; then python3 verify.py; fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_core.c mul_core.c mul_common.c -o "$work/core"
"$work/core"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_ads.c mul_ads.c mul_core.c mul_common.c -lm -o "$work/ads"
"$work/ads"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_infrastructure.c mul_telemetry.c mul_beacon.c mul_time.c mul_common.c -lm -o "$work/infrastructure"
"$work/infrastructure"
