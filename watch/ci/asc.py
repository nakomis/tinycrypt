#!/usr/bin/env python3
"""App Store Connect helpers for CI's TestFlight upload (CRYPT-27).

    asc.py cert KEY.pem OUT.cer        the Apple Distribution certificate for this private key (DER)
    asc.py profile NAME OUT.mobileprovision
                                       an active provisioning profile, by name

One-off setup, by hand (CRYPT-30, when the iOS stub became the iPhone app):
    asc.py push BUNDLE_ID              turn on Push Notifications for an App ID (idempotent)
    asc.py make-profile NAME BUNDLE_ID KEY.pem
                                       an IOS_APP_STORE profile for that App ID and the Apple
                                       Distribution certificate of KEY.pem, unless one is active

The API key comes from the environment: ASC_KEY_ID, ASC_ISSUER_ID and ASC_KEY_PATH (the .p8). The
certificate is found by its public key rather than an id, so a renewed certificate (same private
key or a new one stored in SSM) needs no change here. Needs the cryptography package.
"""

import base64
import json
import os
import sys
import time
import urllib.parse
import urllib.request

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

API = "https://api.appstoreconnect.apple.com"


def token():
    key = serialization.load_pem_private_key(open(os.environ["ASC_KEY_PATH"], "rb").read(), None)
    b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=")  # noqa: E731
    head = b64(json.dumps({"alg": "ES256", "kid": os.environ["ASC_KEY_ID"], "typ": "JWT"}).encode())
    now = int(time.time())
    body = b64(json.dumps({"iss": os.environ["ASC_ISSUER_ID"], "iat": now, "exp": now + 600,
                           "aud": "appstoreconnect-v1"}).encode())
    r, s = decode_dss_signature(key.sign(head + b"." + body, ec.ECDSA(hashes.SHA256())))
    return (head + b"." + body + b"." + b64(r.to_bytes(32, "big") + s.to_bytes(32, "big"))).decode()


def get(path, **params):
    url = API + path + ("?" + urllib.parse.urlencode(params) if params else "")
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + token()})
    return json.load(urllib.request.urlopen(req))


def post(path, body):
    req = urllib.request.Request(API + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Authorization": "Bearer " + token(), "Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req))


def bundle_id(identifier):
    found = [b for b in get("/v1/bundleIds", **{"filter[identifier]": identifier})["data"]
             if b["attributes"]["identifier"] == identifier]  # the filter matches prefixes too
    if len(found) != 1:
        sys.exit(f"want exactly one App ID {identifier!r}, found {len(found)}")
    return found[0]["id"]


def spki(public_key):
    return public_key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)


def find_cert(key_path):
    want = spki(serialization.load_pem_private_key(open(key_path, "rb").read(), None).public_key())
    certs = get("/v1/certificates", **{"filter[certificateType]": "DISTRIBUTION", "limit": "200"})["data"]
    for c in certs:
        der = base64.b64decode(c["attributes"]["certificateContent"])
        if spki(x509.load_der_x509_certificate(der).public_key()) == want:
            return c, der
    sys.exit("no Apple Distribution certificate for this private key")


def cert(key_path, out):
    c, der = find_cert(key_path)
    open(out, "wb").write(der)
    print(f"certificate {c['id']}, expires {c['attributes']['expirationDate'][:10]}")


def push(identifier):
    bid = bundle_id(identifier)
    have = get(f"/v1/bundleIds/{bid}/bundleIdCapabilities")["data"]
    if any(c["attributes"]["capabilityType"] == "PUSH_NOTIFICATIONS" for c in have):
        print(f"{identifier}: push already on")
        return
    post("/v1/bundleIdCapabilities", {"data": {
        "type": "bundleIdCapabilities",
        "attributes": {"capabilityType": "PUSH_NOTIFICATIONS"},
        "relationships": {"bundleId": {"data": {"type": "bundleIds", "id": bid}}}}})
    print(f"{identifier}: push turned on")


def make_profile(name, identifier, key_path):
    if get("/v1/profiles", **{"filter[name]": name, "filter[profileState]": "ACTIVE"})["data"]:
        print(f"profile {name!r} already exists")
        return
    c, _ = find_cert(key_path)
    made = post("/v1/profiles", {"data": {
        "type": "profiles",
        "attributes": {"name": name, "profileType": "IOS_APP_STORE"},
        "relationships": {
            "bundleId": {"data": {"type": "bundleIds", "id": bundle_id(identifier)}},
            "certificates": {"data": [{"type": "certificates", "id": c["id"]}]}}}})["data"]["attributes"]
    print(f"profile {name!r} {made['uuid']} made, expires {made['expirationDate'][:10]}")


def profile(name, out):
    found = get("/v1/profiles", **{"filter[name]": name, "filter[profileState]": "ACTIVE"})["data"]
    if len(found) != 1:
        sys.exit(f"want exactly one active profile named {name!r}, found {len(found)}")
    a = found[0]["attributes"]
    open(out, "wb").write(base64.b64decode(a["profileContent"]))
    print(f"profile {name!r} {a['uuid']}, expires {a['expirationDate'][:10]}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "cert":
        cert(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "profile":
        profile(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == "push":
        push(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "make-profile":
        make_profile(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
