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
for slot in 500 50 10; do
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -DMUL_FOLLOWER_SLOT_MS=$slot -DMUL_PROP_MAX_AGE_MS=6000 \
 -I. tests/test_hybrid.c mul_hybrid.c mul_legacy_twr.c mul_clock.c mul_relation.c mul_observation.c mul_downlink.c mul_localization.c mul_common.c -lm -o "$work/hybrid"
"$work/hybrid"
done
