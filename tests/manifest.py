#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path


def configure(cmake, generator, compiler, source, build, *options):
    return subprocess.run(
        [cmake, "-S", source, "-B", str(build), "-G", generator, f"-DCMAKE_CXX_COMPILER={compiler}",
         "-DLSFGVK_BUILD_CLI=OFF", *options],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, check=False)


def layer(build):
    path = build / "lsfg-vk-layer" / "VkLayer_LSFGVK_frame_generation.json"
    return json.loads(path.read_text(encoding="utf-8"))["layer"]


def main():
    if len(sys.argv) != 6:
        sys.exit("usage: manifest.py <cmake> <generator> <c++ compiler> <source dir> <scratch dir>")
    cmake, generator, compiler, source = sys.argv[1:5]
    scratch = Path(sys.argv[5])
    shutil.rmtree(scratch, ignore_errors=True)

    result = configure(cmake, generator, compiler, source, scratch / "default")
    assert result.returncode == 0, result.stderr
    default = layer(scratch / "default")
    assert "enable_environment" not in default, default
    assert default["disable_environment"] == {"DISABLE_LSFGVK": "1"}, default

    result = configure(cmake, generator, compiler, source, scratch / "enabled",
                       "-DLSFGVK_LAYER_ENABLE_ENVIRONMENT=LSFGVK_ENV")
    assert result.returncode == 0, result.stderr
    enabled = layer(scratch / "enabled")
    assert enabled["enable_environment"] == {"LSFGVK_ENV": "1"}, enabled
    assert enabled["disable_environment"] == {"DISABLE_LSFGVK": "1"}, enabled
    assert {k: v for k, v in enabled.items() if k != "enable_environment"} == default, enabled

    for name in ('X": "1', "LSFGVK-ENV", "1ABC"):
        result = configure(cmake, generator, compiler, source, scratch / "invalid",
                           f"-DLSFGVK_LAYER_ENABLE_ENVIRONMENT={name}")
        assert result.returncode != 0, name
        assert "not a valid variable name" in result.stderr, result.stderr
        shutil.rmtree(scratch / "invalid")

    print("manifest: all checks passed")


if __name__ == "__main__":
    main()
