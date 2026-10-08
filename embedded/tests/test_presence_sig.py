"""Secure Enclave signature interop (CRYPT-10).

Every vectors/se_p256*.json file was signed by a real Secure Enclave key
through CryptoKit `signature(for: Data)`: se_p256.json by the Mac's
(watch/tools/se-vector), se_p256_watch.json by an Apple Watch's over BLE
presence challenges (CRYPT-11, watch/tools/key-standin). They pin the
convention: the signed value is SHA-256(message), the public key is X||Y and
the signature is r||s.
"""
import hashlib
import json
import os
import subprocess
from pathlib import Path

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed, encode_dss_signature

SIGCHECK = Path(os.environ.get("TINYCRYPT_SIGCHECK",
                               Path(__file__).parent.parent / "build" / "tinycrypt-sigcheck"))
VECTOR_FILES = sorted((Path(__file__).parent / "vectors").glob("se_p256*.json"))
VECTORS = {f.stem: json.loads(f.read_text()) for f in VECTOR_FILES}
# (file stem, public key, case) for every case in every file.
CASES = [(name, bytes.fromhex(v["pub"]), case) for name, v in VECTORS.items() for case in v["cases"]]
# Order of the P-256 group.
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551


def sigcheck(pub: bytes, message: bytes, sig: bytes) -> bool:
    if not SIGCHECK.exists():
        pytest.fail(f"{SIGCHECK} not built: cmake -S . -B build -DTINYCRYPT_INSECURE_SOFT_KEY=ON && cmake --build build")
    result = subprocess.run([str(SIGCHECK), pub.hex(), message.hex(), sig.hex()],
                            capture_output=True, text=True)
    assert result.returncode in (0, 1), result.stderr
    return result.returncode == 0


def flip(data: bytes, bit: int) -> bytes:
    out = bytearray(data)
    out[bit // 8] ^= 1 << (bit % 8)
    return bytes(out)


def ids(item):
    return f"{item[0]}:{item[2]['name']}" if isinstance(item, tuple) else item


@pytest.fixture(params=list(VECTORS), ids=list(VECTORS))
def vector(request):
    """One vector file: (public key, its cases)."""
    v = VECTORS[request.param]
    return bytes.fromhex(v["pub"]), v["cases"]


def test_mac_vector_present():
    assert "se_p256" in VECTORS


def test_vectors_come_from_a_secure_enclave(vector):
    pub, cases = vector
    assert len(pub) == 64
    assert cases


@pytest.mark.parametrize("item", CASES, ids=ids)
def test_vector_digest_is_sha256_of_message(item):
    _, _, case = item
    message = bytes.fromhex(case["message"])
    assert hashlib.sha256(message).hexdigest() == case["sha256"]


@pytest.mark.parametrize("item", CASES, ids=ids)
def test_key_side_verifier_accepts_secure_enclave_signature(item):
    _, pub, case = item
    assert sigcheck(pub, bytes.fromhex(case["message"]), bytes.fromhex(case["sig"]))


@pytest.mark.parametrize("item", CASES, ids=ids)
def test_independent_verifier_agrees(item):
    """python `cryptography` (OpenSSL) also verifies it as ECDSA over SHA-256(message)."""
    _, pub, case = item
    key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + pub)
    sig = bytes.fromhex(case["sig"])
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    key.verify(der, bytes.fromhex(case["message"]), ec.ECDSA(hashes.SHA256()))
    key.verify(der, bytes.fromhex(case["sha256"]), ec.ECDSA(Prehashed(hashes.SHA256())))


@pytest.mark.parametrize("item", CASES, ids=ids)
def test_nonce_is_not_the_digest(item):
    """The original bug: treating the 32-byte nonce as if it were the digest.
    The nonce is the message's last 32 bytes (all of it for the Mac vectors)."""
    _, pub, case = item
    key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + pub)
    sig = bytes.fromhex(case["sig"])
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    with pytest.raises(InvalidSignature):
        key.verify(der, bytes.fromhex(case["message"])[-32:], ec.ECDSA(Prehashed(hashes.SHA256())))
    # Feeding our verifier the digest as the message hashes it twice: also rejected.
    assert not sigcheck(pub, bytes.fromhex(case["sha256"]), sig)


@pytest.mark.parametrize("bit", [0, 255, 256, 511])
def test_rejects_tampered_signature(vector, bit):
    pub, cases = vector
    case = cases[0]
    assert not sigcheck(pub, bytes.fromhex(case["message"]), flip(bytes.fromhex(case["sig"]), bit))


@pytest.mark.parametrize("bit", [0, 255])
def test_rejects_tampered_message(vector, bit):
    pub, cases = vector
    case = cases[0]
    assert not sigcheck(pub, flip(bytes.fromhex(case["message"]), bit), bytes.fromhex(case["sig"]))


def test_rejects_other_cases_signature(vector):
    """A signature is bound to its own message."""
    pub, cases = vector
    assert len(cases) >= 2
    assert not sigcheck(pub, bytes.fromhex(cases[0]["message"]), bytes.fromhex(cases[1]["sig"]))


def test_rejects_other_public_key(vector):
    _, cases = vector
    other = ec.generate_private_key(ec.SECP256R1()).public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)[1:]
    case = cases[0]
    assert not sigcheck(other, bytes.fromhex(case["message"]), bytes.fromhex(case["sig"]))


def test_rejects_signature_from_other_secure_enclave_key():
    """Each enrolled key verifies only its own signatures (needs both vector files)."""
    keys = {name: (bytes.fromhex(v["pub"]), v["cases"][0]) for name, v in VECTORS.items()}
    if len(keys) < 2:
        pytest.skip("only one vector file")
    (pub_a, _), (_, case_b) = list(keys.values())[:2]
    assert not sigcheck(pub_a, bytes.fromhex(case_b["message"]), bytes.fromhex(case_b["sig"]))


@pytest.mark.parametrize("bad", ["zero", "x-zero", "flipped"])
def test_rejects_public_key_not_on_curve(vector, bad):
    """Rejected either way. Note uECC_verify alone also rejects these (checked by
    removing the uECC_valid_public_key guard), so this pins behaviour rather than
    proving the guard; the guard is defence in depth, since the key is the watch's
    registered key, not attacker-chosen."""
    good, cases = vector
    pub = {"zero": bytes(64),                    # the all-zero "point at infinity"
           "x-zero": bytes(32) + good[32:],      # x = 0
           "flipped": flip(good, 0)}[bad]        # flipped bit: off the curve
    case = cases[0]
    assert not sigcheck(pub, bytes.fromhex(case["message"]), bytes.fromhex(case["sig"]))


def test_high_s_is_accepted(vector):
    """ECDSA malleability: (r, n - s) is also valid. Neither micro-ecc nor
    CryptoKit enforces low-S. Harmless while every challenge has a fresh nonce,
    but pinned here so nobody assumes signatures are unique."""
    pub, cases = vector
    case = cases[0]
    sig = bytes.fromhex(case["sig"])
    s = int.from_bytes(sig[32:], "big")
    twin = sig[:32] + (N - s).to_bytes(32, "big")
    assert twin != sig
    assert sigcheck(pub, bytes.fromhex(case["message"]), twin)


@pytest.mark.parametrize("args", [
    [],
    ["00" * 64, "00", "00" * 63],    # short signature
    ["00" * 63, "00", "00" * 64],    # short public key
    ["zz" * 64, "00", "00" * 64],    # not hex
    ["00" * 64, "0", "00" * 64],     # odd-length message
    ["00" * 64, " f", "00" * 64],    # whitespace, which sscanf would have accepted
    ["00" * 64, "+f", "00" * 64],    # sign, likewise
])
def test_sigcheck_rejects_bad_arguments(args):
    if not SIGCHECK.exists():
        pytest.fail(f"{SIGCHECK} not built")
    assert subprocess.run([str(SIGCHECK), *args], capture_output=True).returncode == 2
