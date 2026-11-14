#!/bin/sh
# Builds the driver's platform-free parts with this machine's compiler and runs their unit tests.
#
#   sh viogpu3d/umd/tests/run-host-tests.sh
#
# Needs a C11 compiler (CC, default cc) and the DXVK submodule checked out, for the Vulkan headers.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
UMD=$(dirname "$HERE")
VULKAN_INC="$UMD/dxvk/include/vulkan/include"
CC=${CC:-cc}

[ -f "$VULKAN_INC/vulkan/vulkan_core.h" ] ||
    { echo "run-host-tests: no Vulkan headers in $VULKAN_INC; run git submodule update --init --recursive" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

"$CC" -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$UMD/src" -I"$VULKAN_INC" \
    "$HERE/umd_host_tests.c" \
    "$UMD/src/umd_adapter_info.c" "$UMD/src/umd_blob_map.c" "$UMD/src/umd_format.c" "$UMD/src/umd_path.c" \
    "$UMD/src/umd_pipeline.c" \
    "$UMD/src/umd_resource_record.c" "$UMD/src/umd_vk_chain.c" \
    -o "$OUT/umd_host_tests"
"$OUT/umd_host_tests"
