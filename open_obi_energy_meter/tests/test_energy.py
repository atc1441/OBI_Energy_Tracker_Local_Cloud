"""Exercise the reader energy plausibility rules (include/energy_filter.h) on the host."""

import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("energy") / "native"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error", "-g",
        "-I", str(ROOT / "include"), str(ROOT / "tests/energy_native.cpp"), "-o", str(binary),
    ], check=True)
    return binary


def test_energy_invariants(native):
    """The assertions live in C++ so they run against the very code the firmware compiles."""
    result = subprocess.run([str(native)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "all invariants hold" in result.stdout
