#!/usr/bin/env bash
set -euo pipefail

lib=${1:?usage: check-layer-exports.sh <liblsfg-vk-layer.so> [nm]}
nm=${2:-nm}
expected=vkNegotiateLoaderLayerInterfaceVersion

exports=$("$nm" -D --defined-only "$lib" | awk '{ print $NF }' | LC_ALL=C sort -u)

if [[ "$exports" != "$expected" ]]; then
    echo "layer must export only $expected, it exports:" >&2
    echo "$exports" >&2
    exit 1
fi

echo "layer exports $exports"
