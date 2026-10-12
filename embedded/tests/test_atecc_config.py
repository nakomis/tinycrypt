"""CRYPT-22: the ATECC608B configuration in esp32s3/main/atecc_config.h, decoded bit by bit.

Locking it is permanent, so each permission the plan relies on is asserted here, from the
datasheet's definitions (ATECC608A datasheet tables 2-5 to 2-11), not from the hex.
"""

import re
from pathlib import Path

HEADER = (Path(__file__).parent.parent / "esp32s3" / "main" / "atecc_config.h").read_text()


def array(name):
    body = re.search(rf"{name}\[16\] = \{{(.*?)\}};", HEADER, re.S).group(1)
    values = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", body)]
    assert len(values) == 16
    return values


def define(name):
    return int(re.search(rf"#define {name} (\d+)", HEADER).group(1))


SLOT = array("ATECC_SLOT_CONFIG")
KEY = array("ATECC_KEY_CONFIG")
IOT, ATTEST, MASTER = define("ATECC_SLOT_IOT"), define("ATECC_SLOT_ATTEST"), define("ATECC_SLOT_MASTER")


def slot(i):
    v = SLOT[i]
    return {
        "read_key": v & 0xF, "no_mac": v >> 4 & 1, "limited_use": v >> 5 & 1,
        "encrypt_read": v >> 6 & 1, "is_secret": v >> 7 & 1, "write_key": v >> 8 & 0xF,
        "write_config": v >> 12 & 0xF,
    }


def key(i):
    v = KEY[i]
    return {
        "private": v & 1, "pub_info": v >> 1 & 1, "key_type": v >> 2 & 7, "lockable": v >> 5 & 1,
        "req_random": v >> 6 & 1, "req_auth": v >> 7 & 1, "auth_key": v >> 8 & 0xF,
        "persistent_disable": v >> 12 & 1, "rfu": v >> 13 & 1, "x509id": v >> 14 & 3,
    }


def genkey_allowed(wc):
    return bool(wc & 0b0010)  # table 2-9


def privwrite_allowed(wc):
    return bool(wc & 0b0100)  # table 2-10


def write_allowed(wc):
    # table 2-7: Always (0000), PubInvalid (0001) or Encrypt (X1XX)
    return wc in (0b0000, 0b0001) or bool(wc & 0b0100)


def derivekey(wc):
    # table 2-8: None, or (roll|create, needs_mac)
    if not wc & 0b0010:
        return None
    return ("create" if wc & 0b0001 else "roll", bool(wc & 0b1000))


def test_private_key_slots_sign_external_digests_only():
    for i in range(8):
        s, k = slot(i), key(i)
        assert k["private"] and k["key_type"] == 4 and k["pub_info"], i
        assert s["is_secret"], i  # GenKey and Sign fail without it
        assert s["read_key"] == 0b0001, i  # external sign only: no internal sign, no ECDH
        assert genkey_allowed(s["write_config"]), i
        assert not privwrite_allowed(s["write_config"]), i  # a key can only be born on the chip
        assert not s["limited_use"] and not k["req_auth"] and not k["persistent_disable"], i


def test_iot_and_attestation_are_private_key_slots():
    assert IOT == 0  # esp-tls's secure-element convention
    assert ATTEST == 1
    assert key(IOT)["private"] and key(ATTEST)["private"]


def test_master_secret_is_never_readable_or_writable_only_rollable():
    s, k = slot(MASTER), key(MASTER)
    assert not k["private"] and k["key_type"] == 7  # a SHA key
    assert s["is_secret"] and not s["encrypt_read"]  # reads never permitted (table 2-6)
    assert not s["no_mac"]  # SHA(HMAC_Start) needs a key legal for MAC operations
    assert not write_allowed(s["write_config"])
    assert derivekey(s["write_config"]) == ("roll", False)  # CTAP reset, no authorising MAC
    assert k["req_random"]  # and the roll mixes in the chip's random nonce
    assert s["read_key"] != 0  # the datasheet: never 0 unless CheckMac copy is wanted
    assert not s["limited_use"] and not k["req_auth"] and not k["lockable"]


def test_unused_slots_are_inert():
    for i in [8] + list(range(10, 16)):
        s, k = slot(i), key(i)
        assert not k["private"] and k["key_type"] == 7, i
        assert s["is_secret"] and not s["encrypt_read"], i
        assert not write_allowed(s["write_config"]), i
        assert derivekey(s["write_config"]) is None, i
        assert not genkey_allowed(s["write_config"]), i


def test_reserved_bits_clear():
    for i in range(16):
        k = key(i)
        assert k["rfu"] == 0 and k["x509id"] == 0 and k["auth_key"] == 0, i
        assert slot(i)["write_key"] == 0, i


def test_sign_counter_is_the_unattached_one():
    assert define("ATECC_SIGN_COUNTER") == 1  # Counter[0] can be tied to LimitedUse keys
    assert not any(slot(i)["limited_use"] for i in range(16))
