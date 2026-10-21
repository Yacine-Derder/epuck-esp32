#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
cd "$repo_dir"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -Imain main/bridge_protocol.c tests/test_protocol.c -o "$test_dir/protocol"
"$test_dir/protocol"
cc -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined \
 -DBRIDGE_HOST_TEST -Itests -Imain -IThirdParty/mul_core main/bridge_protocol.c ThirdParty/mul_core/mul_telemetry.c tests/test_transport.c -o "$test_dir/transport"
"$test_dir/transport"
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
