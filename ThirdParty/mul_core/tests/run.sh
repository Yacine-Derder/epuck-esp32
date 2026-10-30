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
 -I. tests/test_hybrid.c mul_clock_fit.c mul_hybrid.c mul_legacy_twr.c mul_clock.c mul_relation.c mul_observation.c mul_downlink.c mul_localization.c mul_common.c -lm -o "$work/hybrid"
"$work/hybrid"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -DMUL_FOLLOWER_SLOT_MS=$slot -DMUL_PROP_MAX_AGE_MS=6000 \
 -I. tests/test_network.c mul_clock_fit.c mul_hybrid.c mul_legacy_twr.c mul_clock.c mul_relation.c mul_observation.c mul_downlink.c mul_localization.c mul_common.c -lm -o "$work/network"
"$work/network"
done
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_schedule.c mul_schedule.c mul_common.c -o "$work/schedule"
"$work/schedule"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -DMUL_PROP_MAX_AGE_MS=6000 \
 -I. tests/test_multi_protocol.c mul_multi_protocol.c mul_schedule.c mul_relation.c mul_legacy_twr.c mul_clock.c mul_common.c -lm -o "$work/multi_protocol"
"$work/multi_protocol"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -DMUL_PROP_MAX_AGE_MS=6000 \
 -I. tests/test_multi_receiver.c mul_clock_fit.c mul_multi_receiver.c mul_plan_protocol.c mul_ranging_plan.c mul_multi_protocol.c mul_schedule.c mul_relation.c mul_legacy_twr.c mul_clock.c mul_common.c mul_observation.c mul_downlink.c mul_localization.c -lm -o "$work/multi_receiver"
"$work/multi_receiver"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_multi_telemetry.c mul_telemetry.c -o "$work/multi_telemetry"
"$work/multi_telemetry"
for age in 1000 6000; do
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -DMUL_PROP_MAX_AGE_MS=$age \
 -I. tests/test_clock_fit.c mul_clock_fit.c mul_clock.c mul_relation.c mul_common.c -lm -o "$work/clock_fit"
"$work/clock_fit"
done
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -DMUL_PROP_MAX_AGE_MS=6000 \
 -I. tests/test_timing_profile.c mul_telemetry.c mul_clock_fit.c mul_multi_receiver.c mul_plan_protocol.c mul_ranging_plan.c mul_multi_protocol.c mul_schedule.c mul_relation.c mul_legacy_twr.c mul_clock.c mul_common.c mul_observation.c mul_downlink.c mul_localization.c -lm -o "$work/timing_profile"
"$work/timing_profile"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_ranging_plan.c mul_ranging_plan.c mul_schedule.c mul_common.c -o "$work/ranging_plan"
"$work/ranging_plan"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_plan_store.c mul_plan_store.c mul_ranging_plan.c mul_schedule.c mul_common.c -o "$work/plan_store"
"$work/plan_store"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_plan_protocol.c mul_plan_protocol.c mul_multi_protocol.c mul_ranging_plan.c mul_schedule.c mul_relation.c mul_legacy_twr.c mul_clock.c mul_common.c -lm -o "$work/plan_protocol"
"$work/plan_protocol"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_plan_telemetry.c mul_telemetry.c -o "$work/plan_telemetry"
"$work/plan_telemetry"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_ar_telemetry.c mul_telemetry.c -o "$work/ar_telemetry"
"$work/ar_telemetry"

cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_pose.c mul_pose.c mul_clock.c mul_relation.c mul_schedule.c mul_common.c -lm -o "$work/pose"
"$work/pose"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_pose_telemetry.c mul_telemetry.c -o "$work/pose_telemetry"
"$work/pose_telemetry"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_ekf.c mul_ekf.c mul_ekf_stream.c mul_pose.c mul_schedule.c mul_common.c -lm -o "$work/ekf"
"$work/ekf"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie \
 -I. tests/test_antenna_offset.c -o "$work/antenna_offset"
"$work/antenna_offset"
