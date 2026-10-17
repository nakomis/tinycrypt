"""CRYPT-30: subscribe a key's enrolled watch or phone to that key's presence topic.

Invoked by the IoT rule on tinycrypt/{env}/register/{thing}. The key relays what the device gave it
over BLE at enrolment: {"kind": "watch" | "phone", "token": "<APNs device token, hex>"}; the rule adds
"thing" from the topic, which the key's IoT policy limits to its own name. One device of each kind
per key: a new phone replaces the key's previous phone. An endpoint nothing subscribes to any more is
deleted (one watch may serve several keys, so only then).
"""
import json
import os
import re

import boto3

sns = boto3.client("sns")

TOKEN = re.compile(r"^[0-9a-f]{32,200}$")
APPS = {"watch": os.environ.get("WATCH_APP", ""), "phone": os.environ.get("PHONE_APP", "")}


def log(**fields):
    print(json.dumps(fields))


def app_of(endpoint_arn):
    """arn:aws:sns:REGION:ACCOUNT:endpoint/APNS/<app>/<id> -> <app>"""
    parts = endpoint_arn.split(":", 5)[-1].split("/")
    return parts[2] if len(parts) == 4 and parts[0] == "endpoint" else None


def subscriptions(topic_arn):
    pages = sns.get_paginator("list_subscriptions_by_topic").paginate(TopicArn=topic_arn)
    return [s for page in pages for s in page["Subscriptions"]]


def still_used(endpoint_arn):
    for page in sns.get_paginator("list_subscriptions").paginate():
        if any(s["Endpoint"] == endpoint_arn for s in page["Subscriptions"]):
            return True
    return False


def handler(event, context):
    thing, kind, token = event.get("thing"), event.get("kind"), str(event.get("token", "")).lower()
    prefix = os.environ["THING_PREFIX"]
    if not isinstance(thing, str) or not thing.startswith(prefix) or kind not in APPS or not TOKEN.match(token):
        log(result="refused", thing=thing, kind=kind)
        return

    _, partition, _, region, account = context.invoked_function_arn.split(":")[:5]
    topic_arn = f"arn:{partition}:sns:{region}:{account}:{thing}"
    app_arn = f"arn:{partition}:sns:{region}:{account}:app/{os.environ['PLATFORM']}/{APPS[kind]}"

    # Idempotent for a token it already has (with no attributes passed), so a repeat is harmless.
    endpoint = sns.create_platform_endpoint(PlatformApplicationArn=app_arn, Token=token)["EndpointArn"]
    sns.set_endpoint_attributes(EndpointArn=endpoint, Attributes={"Enabled": "true"})

    subscribed = False
    for sub in subscriptions(topic_arn):
        if sub["Endpoint"] == endpoint:
            subscribed = True
        elif sub["Protocol"] == "application" and app_of(sub["Endpoint"]) == APPS[kind]:
            # This key's previous device of the same kind.
            sns.unsubscribe(SubscriptionArn=sub["SubscriptionArn"])
            old = sub["Endpoint"]
            if not still_used(old):
                sns.delete_endpoint(EndpointArn=old)
            log(result="replaced", thing=thing, kind=kind, old=old)
    if not subscribed:
        sns.subscribe(TopicArn=topic_arn, Protocol="application", Endpoint=endpoint)
    log(result="subscribed" if not subscribed else "already subscribed", thing=thing, kind=kind, endpoint=endpoint)
