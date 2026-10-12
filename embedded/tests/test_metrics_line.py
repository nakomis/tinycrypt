"""CRYPT-28: the key's metrics line (port/metrics_line.c), InfluxDB line protocol for VictoriaMetrics."""

import subprocess

import pytest

from conftest import SIM

TOOL = SIM.parent / "tinycrypt-metricsline"


def line(*args, cap=512):
    out = subprocess.run([str(TOOL), str(cap), *args], capture_output=True, text=True, check=True).stdout
    return None if out == "ERROR\n" else out


def test_integer_fields_tags_and_newline():
    assert line("tinycrypt", "tinycrypt-sandbox-key-1", "0.2.16", "heap_free=123456", "rssi=-61", "mqtt=1") == (
        "tinycrypt,host_name=tinycrypt-sandbox-key-1,version=0.2.16 heap_free=123456i,rssi=-61i,mqtt=1i\n"
    )


def test_version_is_optional():
    assert line("tinycrypt", "key", "-", "uptime_s=5") == "tinycrypt,host_name=key uptime_s=5i\n"


def test_tag_values_are_escaped():
    # Comma, space, equals and backslash are line-protocol syntax in a tag value.
    assert line("tinycrypt", r"a,b c=d\e", "-", "x=1") == "tinycrypt,host_name=a\\,b\\ c\\=d\\\\e x=1i\n"


@pytest.mark.parametrize("host", ["", "new\nline"])
def test_bad_tag_values_are_refused(host):
    assert line("tinycrypt", host, "-", "x=1") is None


@pytest.mark.parametrize("field", ["Heap=1", "heap free=1", "heap,free=1", "heap-free=1"])
def test_field_names_must_be_plain(field):
    assert line("tinycrypt", "key", "-", field) is None


def test_no_fields_is_refused():
    assert line("tinycrypt", "key", "-") is None


def test_a_line_that_doesnt_fit_is_refused_not_truncated():
    full = line("tinycrypt", "tinycrypt-sandbox-key-1", "0.2.16", "heap_free=123456")
    assert line("tinycrypt", "tinycrypt-sandbox-key-1", "0.2.16", "heap_free=123456", cap=len(full) + 1) == full
    assert line("tinycrypt", "tinycrypt-sandbox-key-1", "0.2.16", "heap_free=123456", cap=len(full)) is None


def test_extreme_values():
    assert line("t", "k", "-", "a=-9223372036854775808", "b=9223372036854775807") == (
        "t,host_name=k a=-9223372036854775808i,b=9223372036854775807i\n"
    )
