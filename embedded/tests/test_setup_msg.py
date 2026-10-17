"""The key's BLE setup messages (CRYPT-30, port/setup_msg.c), through tinycrypt-setupmsg.

What the iPhone and watch apps write to the wifi and register characteristics, and what the key
answers on wifi and info reads (at most 512 bytes: one ATT attribute).
"""

import json
import os
import subprocess
from pathlib import Path

import pytest

SETUPMSG = Path(os.environ.get("TINYCRYPT_SETUPMSG", Path(__file__).parent.parent / "build" / "tinycrypt-setupmsg"))
TOKEN = "0123456789abcdef" * 4


@pytest.fixture(autouse=True)
def built():
    if not SETUPMSG.exists():
        pytest.fail(f"{SETUPMSG} not built (from embedded/): cmake --build build")


def run(mode, *inputs):
    lines = "".join((i if isinstance(i, str) else json.dumps(i)) + "\n" for i in inputs)
    out = subprocess.run([str(SETUPMSG), mode], input=lines, capture_output=True, text=True, check=True).stdout
    return [json.loads(line) for line in out.splitlines()]


def wifi(msg):
    return run("wifi", msg)[0]


def register(msg):
    return run("register", msg)[0]


# ---- wifi writes -----------------------------------------------------------------

def test_join():
    assert wifi({"ssid": "Nakomis", "password": "correct horse"}) == \
        {"ok": True, "scan": False, "ssid": "Nakomis", "password": "correct horse"}


def test_scan_request():
    assert wifi({"scan": True}) == {"ok": True, "scan": True, "ssid": "", "password": ""}


def test_open_network_has_an_empty_password():
    assert wifi({"ssid": "Cafe", "password": ""})["ok"]


def test_longest_ssid_and_passphrase():
    assert wifi({"ssid": "s" * 32, "password": "p" * 63})["ok"]


def test_hex_psk():
    assert wifi({"ssid": "x", "password": "ab" * 32})["ok"]


def test_unicode_ssid_counts_bytes():
    assert wifi({"ssid": "é" * 16, "password": ""})["ok"]       # 32 bytes
    assert not wifi({"ssid": "é" * 17, "password": ""})["ok"]   # 34 bytes


@pytest.mark.parametrize("msg, err", [
    ({"ssid": "s" * 33, "password": "password"}, "field too long"),
    ({"ssid": "x", "password": "p" * 65}, "field too long"),
    ({"ssid": "x", "password": "z" * 64}, "64 characters must be a hex PSK"),
    ({"ssid": "x", "password": "short"}, "password too short"),
    ({"ssid": "", "password": "password"}, "empty SSID"),
    ({"ssid": "x"}, "missing field"),
    ({"password": "password"}, "missing field"),
    ({"ssid": 5, "password": "password"}, "missing field"),
    ({"ssid": "a\u0001b", "password": "password"}, "control character"),
    ({"ssid": "x", "password": "pass\nword"}, "control character"),
    ({"scan": False}, "missing field"),
    ("[1, 2]", "not a JSON object"),
    ("{not json", "not a JSON object"),
])
def test_bad_wifi(msg, err):
    assert wifi(msg) == {"ok": False, "err": err}


def test_refusal_leaves_no_password():
    # The tool prints nothing of a refused message; the parser also wipes its output struct.
    assert "password" not in wifi({"ssid": "", "password": "secret123"})


# ---- register writes -------------------------------------------------------------

@pytest.mark.parametrize("kind", ["watch", "phone"])
def test_register(kind):
    out = register({"kind": kind, "token": TOKEN})
    assert out["ok"] and out["kind"] == kind and out["token"] == TOKEN
    assert json.loads(out["payload"]) == {"kind": kind, "token": TOKEN}


def test_token_is_lower_cased():
    assert register({"kind": "phone", "token": TOKEN.upper()})["token"] == TOKEN


@pytest.mark.parametrize("msg, err", [
    ({"kind": "tablet", "token": TOKEN}, "unknown kind"),
    ({"kind": "phone", "token": "g" * 64}, "token must be hex"),
    ({"kind": "phone", "token": "ab" * 15}, "token must be hex"),         # 30 digits: too short
    ({"kind": "phone", "token": "a" * 201}, "field too long"),
    ({"kind": "phone"}, "missing field"),
    ({"token": TOKEN}, "missing field"),
    ({"kind": "phones", "token": TOKEN}, "unknown kind"),
    ({"kind": "smartphone", "token": TOKEN}, "field too long"),
])
def test_bad_register(msg, err):
    assert register(msg) == {"ok": False, "err": err}


# ---- reads ------------------------------------------------------------------------

def test_info():
    out = run("info", {"thing": "tinycrypt-sandbox-key-1", "version": "0.3.0", "ssid": "Nakomis",
                       "wifi": 2, "iot": True, "watch": True, "phone": False})[0]
    assert out == {"thing": "tinycrypt-sandbox-key-1", "version": "0.3.0",
                   "wifi": {"ssid": "Nakomis", "state": "connected"}, "iot": True, "watch": True, "phone": False}


@pytest.mark.parametrize("state, name", list(enumerate(
    ["unset", "connecting", "connected", "wrong-password", "not-found", "failed"])))
def test_wifi_state_names(state, name):
    assert run("info", {"wifi": state})[0]["wifi"]["state"] == name


def test_scan_sorted_deduplicated_hidden_dropped():
    out = run("scan", [["Home", -70, 1], ["Cafe", -40, 0], ["", -30, 1], ["Home", -50, 1]])[0]
    assert out == [["Cafe", -40, 0], ["Home", -50, 1]]


def test_scan_keeps_multibyte_utf8_ssids():
    # The sim reads its input as JSON, so it can't be fed an SSID that isn't UTF-8 (only the radio
    # can); this checks the UTF-8 filter keeps valid multi-byte names.
    out = run("scan", [["Café", -40, 1], ["日本", -50, 1]])[0]
    assert out == [["Café", -40, 1], ["日本", -50, 1]]


def test_scan_keeps_twelve():
    out = run("scan", [[f"net{i}", -30 - i, 1] for i in range(20)])[0]
    assert [n[0] for n in out] == [f"net{i}" for i in range(12)]


def test_scan_fits_one_attribute():
    nets = [[chr(0x41 + i) * 32, -30 - i, 1] for i in range(12)]
    raw = subprocess.run([str(SETUPMSG), "scan"], input=json.dumps(nets) + "\n",
                         capture_output=True, text=True, check=True).stdout.strip()
    out = json.loads(raw)
    assert len(raw.encode()) <= 512 and 0 < len(out) < 12
    assert out == [n for n in nets[:len(out)]]   # the weakest are the ones dropped
