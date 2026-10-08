"""End-to-end CTAP2 tests against the Mac sim, driven by python-fido2."""

import os

import pytest
from fido2.client import ClientError, DefaultClientDataCollector, Fido2Client
from fido2.ctap import CtapError
from fido2.ctap1 import APDU, ApduError, Ctap1
from fido2.ctap2 import Ctap2
from fido2.server import Fido2Server
from fido2.webauthn import PublicKeyCredentialRpEntity, PublicKeyCredentialUserEntity

RP = PublicKeyCredentialRpEntity(name="tinycrypt test", id="example.com")
ORIGIN = "https://example.com"
USER = PublicKeyCredentialUserEntity(id=b"user-0001", name="acme", display_name="Acme Test")


def client_for(device, origin=ORIGIN):
    return Fido2Client(device, client_data_collector=DefaultClientDataCollector(origin))


def register(device, server=None):
    server = server or Fido2Server(RP)
    options, state = server.register_begin(USER, user_verification="discouraged")
    response = client_for(device).make_credential(options.public_key)
    auth_data = server.register_complete(state, response)
    return server, auth_data


def authenticate(device, server, credentials):
    options, state = server.authenticate_begin(credentials, user_verification="discouraged")
    selection = client_for(device).get_assertion(options.public_key)
    response = selection.get_response(0)
    server.authenticate_complete(state, credentials, response)
    return response


def test_get_info_reports_fido2(sim):
    info = Ctap2(sim.device()).get_info()
    assert "FIDO_2_0" in info.versions
    assert str(info.aaguid) == "f3751ba0-8b97-47ac-aba7-d1c15c36fecf"


def test_register_then_authenticate(sim):
    device = sim.device()
    server, auth_data = register(device)
    cred = auth_data.credential_data
    assert cred is not None
    # authenticate_complete verifies the signature against the registered key.
    authenticate(device, server, [cred])


def test_sign_counter_increases_and_survives_restart(sim):
    server, auth_data = register(sim.device())
    cred = auth_data.credential_data
    first = authenticate(sim.device(), server, [cred]).response.authenticator_data.counter
    second = authenticate(sim.device(), server, [cred]).response.authenticator_data.counter
    assert second > first
    sim.restart()
    third = authenticate(sim.device(), server, [cred]).response.authenticator_data.counter
    assert third > second


def test_credential_survives_restart(sim):
    server, auth_data = register(sim.device())
    sim.restart()
    authenticate(sim.device(), server, [auth_data.credential_data])


def test_presence_denied_blocks_registration(make_sim):
    sim = make_sim(presence="deny")
    ctap = Ctap2(sim.device())
    with pytest.raises(CtapError) as err:
        ctap.make_credential(
            os.urandom(32),
            {"id": RP.id, "name": RP.name},
            {"id": USER.id, "name": USER.name},
            [{"type": "public-key", "alg": -7}],
        )
    assert err.value.code in (CtapError.ERR.OPERATION_DENIED, CtapError.ERR.ACTION_TIMEOUT)


def test_credential_is_bound_to_its_rp(sim):
    _, auth_data = register(sim.device())
    other = Fido2Server(PublicKeyCredentialRpEntity(name="other", id="other.example"))
    options, _ = other.authenticate_begin([auth_data.credential_data], user_verification="discouraged")
    with pytest.raises(ClientError) as err:
        client_for(sim.device(), origin="https://other.example").get_assertion(options.public_key)
    assert err.value.code == ClientError.ERR.DEVICE_INELIGIBLE


def test_resident_key_found_without_allow_list_and_survives_restart(sim):
    server = Fido2Server(RP)
    options, state = server.register_begin(
        USER, resident_key_requirement="required", user_verification="discouraged"
    )
    cred = server.register_complete(
        state, client_for(sim.device()).make_credential(options.public_key)
    ).credential_data

    for _ in range(2):
        options, state = server.authenticate_begin(user_verification="discouraged")
        selection = client_for(sim.device()).get_assertion(options.public_key)
        response = selection.get_response(0)
        assert response.response.user_handle == USER.id
        server.authenticate_complete(state, [cred], response)
        sim.restart()


def test_u2f_register_and_authenticate(sim):
    ctap1 = Ctap1(sim.device())
    app_param = os.urandom(32)
    reg = ctap1.register(os.urandom(32), app_param)
    challenge = os.urandom(32)
    sig = ctap1.authenticate(challenge, app_param, reg.key_handle)
    assert sig.user_presence & 1
    sig.verify(app_param, challenge, reg.public_key)


@pytest.mark.parametrize("op", ["register", "authenticate"])
def test_u2f_respects_denied_presence(make_sim, op):
    # Regression guard for the presence contract: U2F only treats 0 as "absent".
    approving = make_sim(presence="auto")
    app_param = os.urandom(32)
    reg = Ctap1(approving.device()).register(os.urandom(32), app_param)
    approving.stop()

    denying = make_sim(presence="deny")
    ctap1 = Ctap1(denying.device())
    with pytest.raises(ApduError) as err:
        if op == "register":
            ctap1.register(os.urandom(32), app_param)
        else:
            ctap1.authenticate(os.urandom(32), app_param, reg.key_handle)
    assert err.value.code == APDU.USE_NOT_SATISFIED
