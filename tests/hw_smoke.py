"""Register and authenticate against a real tinycrypt key over USB HID.

Needs a human: press the key's presence button (BOOT) when prompted.
    tests/.venv/bin/python tests/hw_smoke.py
"""

import sys
import time

from fido2.client import ClientError, DefaultClientDataCollector, Fido2Client, UserInteraction
from fido2.hid import CtapHidDevice
from fido2.server import Fido2Server
from fido2.webauthn import PublicKeyCredentialRpEntity, PublicKeyCredentialUserEntity

RP = PublicKeyCredentialRpEntity(name="tinycrypt hw smoke", id="example.com")
USER = PublicKeyCredentialUserEntity(id=b"hw-smoke", name="acme", display_name="Acme Test")


class Prompt(UserInteraction):
    def prompt_up(self):
        print(">>> Press BOOT on the key now", flush=True)


def find_key(wait_s=120):
    deadline = time.time() + wait_s
    while time.time() < deadline:
        for dev in CtapHidDevice.list_devices():
            if dev.descriptor.product_name and "tinycrypt" in dev.descriptor.product_name:
                return dev
        time.sleep(1)
    sys.exit("no tinycrypt key found")


def with_retries(step, attempts=6):
    """Each presence wait times out after ~30 s; give the human a few goes."""
    for attempt in range(1, attempts + 1):
        try:
            return step()
        except ClientError as err:
            if err.code != ClientError.ERR.TIMEOUT or attempt == attempts:
                raise
            print(f"... no press within the timeout, retrying ({attempt}/{attempts})", flush=True)


def main():
    dev = find_key()
    client = Fido2Client(dev, DefaultClientDataCollector("https://example.com"), Prompt())
    server = Fido2Server(RP)

    options, state = server.register_begin(USER, user_verification="discouraged")
    t = time.time()
    auth_data = server.register_complete(
        state, with_retries(lambda: client.make_credential(options.public_key))
    )
    cred = auth_data.credential_data
    print(f"registered in {time.time() - t:.1f}s, credential {cred.credential_id.hex()[:16]}...", flush=True)

    options, state = server.authenticate_begin([cred], user_verification="discouraged")
    t = time.time()
    response = with_retries(lambda: client.get_assertion(options.public_key)).get_response(0)
    server.authenticate_complete(state, [cred], response)
    print(f"authenticated in {time.time() - t:.1f}s, sign count {response.response.authenticator_data.counter}")
    print("PASS")


if __name__ == "__main__":
    main()
