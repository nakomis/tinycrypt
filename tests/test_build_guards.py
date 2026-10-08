"""The INSECURE software key must never build in a release configuration."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).parent.parent
PORT = ROOT / "port"


def configure(tmp_path, *flags):
    return subprocess.run(
        ["cmake", "-S", str(ROOT), "-B", str(tmp_path / "build"), *flags],
        capture_output=True,
        text=True,
    )


def compile_keystore_header(*defines):
    return subprocess.run(
        ["cc", "-fsyntax-only", "-x", "c", f"-I{PORT}", *[f"-D{d}" for d in defines], "-"],
        input='#include "tinycrypt_keystore.h"\n',
        capture_output=True,
        text=True,
    )


def test_cmake_refuses_soft_key_in_release(tmp_path):
    result = configure(tmp_path, "-DTINYCRYPT_RELEASE=ON", "-DTINYCRYPT_INSECURE_SOFT_KEY=ON")
    assert result.returncode != 0
    assert "must never be built in a release configuration" in result.stderr


def test_header_refuses_soft_key_in_release():
    result = compile_keystore_header("TINYCRYPT_INSECURE_SOFT_KEY", "TINYCRYPT_RELEASE")
    assert result.returncode != 0
    assert "must never be built in a release configuration" in result.stderr


def test_header_refuses_build_without_a_keystore():
    result = compile_keystore_header()
    assert result.returncode != 0
    assert "No secure keystore backend exists yet" in result.stderr


def test_header_accepts_soft_key_test_build():
    assert compile_keystore_header("TINYCRYPT_INSECURE_SOFT_KEY").returncode == 0
