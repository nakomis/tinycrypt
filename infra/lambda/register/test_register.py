"""CRYPT-30: the register Lambda against an in-memory SNS (no AWS, no boto3 needed)."""
import os
import sys
import types

import pytest

sys.modules.setdefault("boto3", types.SimpleNamespace(client=lambda name: None))
os.environ.update(THING_PREFIX="tinycrypt-sandbox-", PLATFORM="APNS_SANDBOX",
                  WATCH_APP="tinycrypt-watch-sandbox", PHONE_APP="tinycrypt-phone-sandbox")
sys.path.insert(0, os.path.dirname(__file__))
import register  # noqa: E402

PREFIX = "arn:aws:sns:eu-west-2:111122223333:"


class FakeSns:
    def __init__(self):
        self.endpoints = {}  # arn -> token
        self.subs = []       # (sub arn, topic arn, endpoint arn)
        self.n = 0

    def create_platform_endpoint(self, PlatformApplicationArn, Token):
        app = PlatformApplicationArn.split(":")[-1].replace("app/", "")
        for arn, (a, t) in self.endpoints.items():
            if a == app and t == Token:
                return {"EndpointArn": arn}
        self.n += 1
        arn = f"{PREFIX}endpoint/{app}/{self.n}"
        self.endpoints[arn] = (app, Token)
        return {"EndpointArn": arn}

    def set_endpoint_attributes(self, EndpointArn, Attributes):
        assert EndpointArn in self.endpoints

    def get_paginator(self, op):
        fake = self

        class Pages:
            def paginate(self, TopicArn=None):
                subs = [s for s in fake.subs if TopicArn is None or s[1] == TopicArn]
                yield {"Subscriptions": [{"SubscriptionArn": s, "TopicArn": t, "Endpoint": e,
                                          "Protocol": "application"} for s, t, e in subs]}
        return Pages()

    def subscribe(self, TopicArn, Protocol, Endpoint):
        self.n += 1
        self.subs.append((f"{TopicArn}:{self.n}", TopicArn, Endpoint))

    def unsubscribe(self, SubscriptionArn):
        self.subs = [s for s in self.subs if s[0] != SubscriptionArn]

    def delete_endpoint(self, EndpointArn):
        del self.endpoints[EndpointArn]

    def topics_of(self, endpoint):
        return sorted(t.split(":")[-1] for _, t, e in self.subs if e == endpoint)


@pytest.fixture
def sns(monkeypatch):
    fake = FakeSns()
    monkeypatch.setattr(register, "sns", fake)
    return fake


CTX = types.SimpleNamespace(invoked_function_arn=f"{PREFIX}function:tinycrypt-register-sandbox".replace(":sns:", ":lambda:"))
TOKEN_A, TOKEN_B = "a" * 64, "b" * 64


def reg(thing, kind, token, client=None):
    register.handler({"thing": thing, "kind": kind, "token": token, "client": client or thing}, CTX)


def endpoint_for(sns, kind, token):
    return next(a for a, (app, t) in sns.endpoints.items() if t == token and app.endswith(f"tinycrypt-{kind}-sandbox"))


def test_subscribes_a_phone_to_its_key_only(sns):
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_A)
    ep = endpoint_for(sns, "phone", TOKEN_A)
    assert "APNS_SANDBOX/tinycrypt-phone-sandbox" in ep
    assert sns.topics_of(ep) == ["tinycrypt-sandbox-key-1"]


def test_repeat_is_harmless(sns):
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_A)
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_A)
    assert len(sns.subs) == 1 and len(sns.endpoints) == 1


def test_new_phone_replaces_old_phone_but_not_the_watch(sns):
    reg("tinycrypt-sandbox-key-1", "watch", TOKEN_A)
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_A)
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_B)
    watch = endpoint_for(sns, "watch", TOKEN_A)
    assert sns.topics_of(watch) == ["tinycrypt-sandbox-key-1"]
    assert sns.topics_of(endpoint_for(sns, "phone", TOKEN_B)) == ["tinycrypt-sandbox-key-1"]
    assert not any(t == TOKEN_A and "phone" in app for app, t in sns.endpoints.values())  # old phone deleted


def test_one_watch_on_two_keys_survives_replacement_on_one(sns):
    reg("tinycrypt-sandbox-key-1", "watch", TOKEN_A)
    reg("tinycrypt-sandbox-key-2", "watch", TOKEN_A)
    reg("tinycrypt-sandbox-key-1", "watch", TOKEN_B)
    old = endpoint_for(sns, "watch", TOKEN_A)  # still exists: key-2 uses it
    assert sns.topics_of(old) == ["tinycrypt-sandbox-key-2"]
    assert sns.topics_of(endpoint_for(sns, "watch", TOKEN_B)) == ["tinycrypt-sandbox-key-1"]


def test_empty_token_deregisters_that_kind_only(sns):
    reg("tinycrypt-sandbox-key-1", "watch", TOKEN_A)
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_B)
    reg("tinycrypt-sandbox-key-1", "phone", "")
    assert sns.topics_of(endpoint_for(sns, "watch", TOKEN_A)) == ["tinycrypt-sandbox-key-1"]
    assert not any("phone" in app for app, _ in sns.endpoints.values())   # unused, so deleted


def test_deregistering_keeps_an_endpoint_another_key_uses(sns):
    reg("tinycrypt-sandbox-key-1", "watch", TOKEN_A)
    reg("tinycrypt-sandbox-key-2", "watch", TOKEN_A)
    reg("tinycrypt-sandbox-key-1", "watch", "")
    assert sns.topics_of(endpoint_for(sns, "watch", TOKEN_A)) == ["tinycrypt-sandbox-key-2"]


def test_another_keys_client_cant_register_for_this_key(sns):
    reg("tinycrypt-sandbox-key-1", "phone", TOKEN_A, client="tinycrypt-sandbox-key-2")
    assert not sns.subs and not sns.endpoints


@pytest.mark.parametrize("event", [
    {"thing": "tinycrypt-prod-key-1", "kind": "phone", "token": TOKEN_A},     # another environment
    {"thing": "someone-else", "kind": "phone", "token": TOKEN_A},
    {"thing": "tinycrypt-sandbox-key-1", "kind": "tablet", "token": TOKEN_A},
    {"thing": "tinycrypt-sandbox-key-1", "kind": "phone", "token": "not hex"},
    {"thing": "tinycrypt-sandbox-key-1", "kind": "phone", "token": "ab"},       # too short
    {"thing": "tinycrypt-sandbox-key-1", "kind": "phone"},
    {"thing": "tinycrypt-sandbox-key-1", "kind": ["phone"], "token": TOKEN_A},
    {"thing": "tinycrypt-sandbox-key-1", "kind": "phone", "token": 5},
])
def test_refuses_bad_registrations(sns, event):
    register.handler({"client": event.get("thing"), **event}, CTX)
    assert not sns.subs and not sns.endpoints
