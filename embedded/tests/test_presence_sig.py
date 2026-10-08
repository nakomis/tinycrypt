"""Secure Enclave signature interop (CRYPT-10).

The vectors in vectors/se_p256.json were signed by a real Secure Enclave key
through CryptoKit `signature(for: Data)`, exactly as the watch will sign
(watch/tools/se-vector). They pin the convention: the signed value is
SHA-256(message), the public key is X||Y and the signature is r||s.
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
VECTORS = json.loads((Path(__file__).parent / "vectors" / "se_p256.json").read_text())
PUB = bytes.fromhex(VECTORS["pub"])
CASES = VECTORS["cases"]
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


def ids(case):
    return case["name"]


def test_vectors_come_from_a_secure_enclave():
    assert VECTORS["source"] == "secure-enclave"
    assert len(PUB) == 64
    assert CASES


@pytest.mark.parametrize("case", CASES, ids=ids)
def test_vector_digest_is_sha256_of_message(case):
    message = bytes.fromhex(case["message"])
    assert hashlib.sha256(message).hexdigest() == case["sha256"]


@pytest.mark.parametrize("case", CASES, ids=ids)
def test_key_side_verifier_accepts_secure_enclave_signature(case):
    assert sigcheck(PUB, bytes.fromhex(case["message"]), bytes.fromhex(case["sig"]))


@pytest.mark.parametrize("case", CASES, ids=ids)
def test_independent_verifier_agrees(case):
    """python `cryptography` (OpenSSL) also verifies it as ECDSA over SHA-256(message)."""
    key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + PUB)
    sig = bytes.fromhex(case["sig"])
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    key.verify(der, bytes.fromhex(case["message"]), ec.ECDSA(hashes.SHA256()))
    key.verify(der, bytes.fromhex(case["sha256"]), ec.ECDSA(Prehashed(hashes.SHA256())))


@pytest.mark.parametrize("case", CASES, ids=ids)
def test_nonce_is_not_the_digest(case):
    """The original bug: treating the 32-byte nonce as if it were the digest."""
    key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + PUB)
    sig = bytes.fromhex(case["sig"])
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    with pytest.raises(InvalidSignature):
        key.verify(der, bytes.fromhex(case["message"]), ec.ECDSA(Prehashed(hashes.SHA256())))
    # Feeding our verifier the digest as the message hashes it twice: also rejected.
    assert not sigcheck(PUB, bytes.fromhex(case["sha256"]), sig)


@pytest.mark.parametrize("bit", [0, 255, 256, 511])
def test_rejects_tampered_signature(bit):
    case = CASES[0]
    assert not sigcheck(PUB, bytes.fromhex(case["message"]), flip(bytes.fromhex(case["sig"]), bit))


@pytest.mark.parametrize("bit", [0, 255])
def test_rejects_tampered_message(bit):
    case = CASES[0]
    assert not sigcheck(PUB, flip(bytes.fromhex(case["message"]), bit), bytes.fromhex(case["sig"]))


def test_rejects_other_cases_signature():
    """A signature is bound to its own message."""
    assert len(CASES) >= 2
    assert not sigcheck(PUB, bytes.fromhex(CASES[0]["message"]), bytes.fromhex(CASES[1]["sig"]))


def test_rejects_other_public_key():
    other = ec.generate_private_key(ec.SECP256R1()).public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)[1:]
    case = CASES[0]
    assert not sigcheck(other, bytes.fromhex(case["message"]), bytes.fromhex(case["sig"]))


def test_rejects_public_key_not_on_curve():
    case = CASES[0]
    assert not sigcheck(flip(PUB, 0), bytes.fromhex(case["message"]), bytes.fromhex(case["sig"]))


def test_high_s_is_accepted():
    """ECDSA malleability: (r, n - s) is also valid. Neither micro-ecc nor
    CryptoKit enforces low-S. Harmless while every challenge has a fresh nonce,
    but pinned here so nobody assumes signatures are unique."""
    case = CASES[0]
    sig = bytes.fromhex(case["sig"])
    s = int.from_bytes(sig[32:], "big")
    twin = sig[:32] + (N - s).to_bytes(32, "big")
    assert twin != sig
    assert sigcheck(PUB, bytes.fromhex(case["message"]), twin)


@pytest.mark.parametrize("args", [
    [],
    ["00" * 64, "00", "00" * 63],    # short signature
    ["00" * 63, "00", "00" * 64],    # short public key
    ["zz" * 64, "00", "00" * 64],    # not hex
    ["00" * 64, "0", "00" * 64],     # odd-length message
])
def test_sigcheck_rejects_bad_arguments(args):
    if not SIGCHECK.exists():
        pytest.fail(f"{SIGCHECK} not built")
    assert subprocess.run([str(SIGCHECK), *args], capture_output=True).returncode == 2
