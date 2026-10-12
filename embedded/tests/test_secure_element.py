"""CRYPT-22: solo1's secrets live in the secure element (here the sim's INSECURE software one).

These prove the routing in port/crypto_tc.c: credentials come from the element's master secret,
not the authenticator state; a reset rolls it; attestation is the element's key with a
self-signed FIDO certificate; the sign counter is the element's; the IoT CSR is well formed.
"""

import os
import subprocess

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from cryptography.x509.oid import NameOID
from fido2.attestation import PackedAttestation
from fido2.client import ClientError
from fido2.ctap import CtapError
from fido2.ctap2 import Ctap2
from fido2.server import Fido2Server

from conftest import SIM
from test_sim import RP, USER, authenticate, client_for, register

AAGUID = bytes.fromhex("f3751ba08b9747acaba7d1c15c36fecf")
AAGUID_OID = x509.ObjectIdentifier("1.3.6.1.4.1.45724.1.1.4")
# AuthenticatorState_0x01 (solo1 storage.h): key_space starts after is_initialized, is_pin_set,
# PIN_CODE_HASH[32], PIN_SALT[32], int _reserved (aligned to 4), remaining_tries, rk_stored
# (aligned to 2) and key_lens[1].
KEY_SPACE = slice(78, 78 + 128)


def make_credential(sim):
    options, state = (server := Fido2Server(RP, attestation="direct")).register_begin(
        USER, user_verification="discouraged"
    )
    response = client_for(sim.device()).make_credential(options.public_key)
    return server, state, response


def test_attestation_is_the_elements_key_with_a_fido_certificate(sim):
    server, state, response = make_credential(sim)
    att = response.response.attestation_object
    assert att.fmt == "packed"
    # python-fido2's packed verifier checks the FIDO certificate rules (version 3, OU
    # "Authenticator Attestation", CA:FALSE, the AAGUID extension matching) and the signature.
    PackedAttestation().verify(att.att_stmt, att.auth_data, response.response.client_data.hash)
    server.register_complete(state, response)

    cert = x509.load_der_x509_certificate(att.att_stmt["x5c"][0])
    subject = {a.oid: a.value for a in cert.subject}
    assert subject == {
        NameOID.COUNTRY_NAME: "GB",
        NameOID.ORGANIZATION_NAME: "Nakomis",
        NameOID.ORGANIZATIONAL_UNIT_NAME: "Authenticator Attestation",
        NameOID.COMMON_NAME: "tinycrypt",
    }
    assert cert.issuer == cert.subject
    cert.public_key().verify(cert.signature, cert.tbs_certificate_bytes, ec.ECDSA(hashes.SHA256()))
    assert cert.extensions.get_extension_for_class(x509.BasicConstraints).critical
    assert not cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca
    assert cert.extensions.get_extension_for_oid(AAGUID_OID).value.value == b"\x04\x10" + AAGUID
    # Not solo1's published development key.
    assert b"TEST CA" not in att.att_stmt["x5c"][0]


def test_attestation_certificate_is_stable_across_restarts(sim):
    _, _, first = make_credential(sim)
    sim.restart()
    _, _, second = make_credential(sim)
    cert = lambda r: r.response.attestation_object.att_stmt["x5c"][0]  # noqa: E731
    assert cert(first) == cert(second)


def test_credentials_do_not_depend_on_the_authenticator_state(sim):
    server, auth_data = register(sim.device())
    sim.stop()
    state = sim.state_dir / "authenticator_state.bin"
    data = bytearray(state.read_bytes())
    data[KEY_SPACE] = os.urandom(KEY_SPACE.stop - KEY_SPACE.start)
    state.write_bytes(bytes(data))
    sim.start()
    authenticate(sim.device(), server, [auth_data.credential_data])


def test_another_element_cannot_use_the_credential(sim):
    server, auth_data = register(sim.device())
    sim.stop()
    (sim.state_dir / "secure_element.bin").unlink()
    (sim.state_dir / "attestation_cert.der").unlink()
    sim.start()
    with pytest.raises(ClientError):
        authenticate(sim.device(), server, [auth_data.credential_data])


def test_reset_rolls_the_master_secret(sim):
    server, auth_data = register(sim.device())
    before = (sim.state_dir / "secure_element.bin").read_bytes()
    Ctap2(sim.device()).reset()
    assert (sim.state_dir / "secure_element.bin").read_bytes() != before
    with pytest.raises(ClientError):
        authenticate(sim.device(), server, [auth_data.credential_data])
    sim.restart()  # and not just until the next boot
    with pytest.raises(ClientError):
        authenticate(sim.device(), server, [auth_data.credential_data])
    register(sim.device())  # new credentials still work


def test_sign_counter_is_the_elements(sim):
    server, auth_data = register(sim.device())
    authenticate(sim.device(), server, [auth_data.credential_data])
    assert not (sim.state_dir / "sign_counter.bin").exists()
    assert (sim.state_dir / "secure_element.bin").exists()


def test_ed25519_credential(sim):
    ctap = Ctap2(sim.device())
    rp = {"id": RP.id, "name": RP.name}
    made = ctap.make_credential(os.urandom(32), rp, {"id": USER.id, "name": USER.name},
                                [{"type": "public-key", "alg": -8}])
    cred = made.auth_data.credential_data
    assert cred.public_key[3] == -8
    pub = Ed25519PublicKey.from_public_bytes(cred.public_key[-2])
    cdh = os.urandom(32)
    got = ctap.get_assertion(RP.id, cdh, [{"type": "public-key", "id": cred.credential_id}])
    pub.verify(got.signature, bytes(got.auth_data) + cdh)


def test_unknown_credential_is_still_refused(sim):
    ctap = Ctap2(sim.device())
    with pytest.raises(CtapError) as err:
        ctap.get_assertion(RP.id, os.urandom(32), [{"type": "public-key", "id": os.urandom(80)}])
    assert err.value.code == CtapError.ERR.NO_CREDENTIALS


def test_iot_csr(tmp_path):
    run = lambda: subprocess.run(  # noqa: E731
        [str(SIM), "--state-dir", str(tmp_path), "--print-csr", "key-7"],
        capture_output=True, text=True, check=True,
    )
    csr = x509.load_der_x509_csr(bytes.fromhex(run().stdout.strip().splitlines()[-1]))
    assert csr.is_signature_valid
    assert csr.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[0].value == "key-7"
    assert isinstance(csr.public_key(), ec.EllipticCurvePublicKey)
    assert csr.public_key().curve.name == "secp256r1"
    # The IoT key is its own key, not the attestation key, and it is the same on every run.
    cert = x509.load_der_x509_certificate((tmp_path / "attestation_cert.der").read_bytes())
    raw = lambda k: k.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)  # noqa: E731
    assert raw(csr.public_key()) != raw(cert.public_key())
    again = x509.load_der_x509_csr(bytes.fromhex(run().stdout.strip().splitlines()[-1]))
    assert raw(again.public_key()) == raw(csr.public_key())


def test_long_custom_credential_id_is_not_a_crash(sim):
    # solo1 takes "custom" credential IDs of up to 255 bytes (wallet tag first) and can derive a
    # key from the whole ID. A relying party chooses the ID, so the derivation hashes it rather
    # than send it to the element whole (which takes 228 bytes at most, and aborts beyond). This
    # sim build doesn't reach that derivation by this route (the earlier code didn't crash here
    # either); it guards the route all the same.
    ctap = Ctap2(sim.device())
    for n in (24, 200, 211, 212, 255):
        for up in (False, True):
            cred = b"\x8c\x27\x90\xf6" + os.urandom(n - 4)
            try:
                ctap.get_assertion(RP.id, os.urandom(32), [{"type": "public-key", "id": cred}],
                                   options={"up": up})
            except CtapError:
                pass
    assert "FIDO_2_0" in Ctap2(sim.device()).get_info().versions  # still running
    assert sim.proc.poll() is None


def test_lost_state_takes_the_credentials_with_it(sim):
    # solo1 starts a fresh state (no PIN) when its stored one is gone; old credentials must not
    # carry over, or wiping the state would bypass the PIN.
    server, auth_data = register(sim.device())
    sim.stop()
    (sim.state_dir / "authenticator_state.bin").unlink()
    sim.start()
    with pytest.raises(ClientError):
        authenticate(sim.device(), server, [auth_data.credential_data])
    register(sim.device())


def test_attestation_certificate_follows_the_element(sim):
    _, _, before = make_credential(sim)
    sim.stop()
    (sim.state_dir / "secure_element.bin").unlink()  # a new "chip"; the old certificate stays
    sim.start()
    _, _, after = make_credential(sim)
    att = after.response.attestation_object
    PackedAttestation().verify(att.att_stmt, att.auth_data, after.response.client_data.hash)
    assert att.att_stmt["x5c"][0] != before.response.attestation_object.att_stmt["x5c"][0]
