#!/usr/bin/env bash
# CRYPT-12: publish presence requests to IoT Core as the key would, from the Mac.
# The IoT rule passes the message straight to SNS, so it is already in SNS's JSON message
# structure, with the APNs payload as a string under APNS_SANDBOX (or APNS in prod). Each carries
# tc.sentAt (ms since the epoch), which the watch compares with its own clock on arrival.
#
#   infra/scripts/push-test.sh [count] [gap-seconds]
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox).
set -euo pipefail

count=${1:-1}
gap=${2:-20}
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
platform=$([ "$env" = prod ] && echo APNS || echo APNS_SANDBOX)

endpoint=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)
topic="tinycrypt/${env}/presence/request"

for i in $(seq 1 "$count"); do
    id="run-$(date +%H%M%S)-$i"
    # sentAt is stamped here, before the AWS CLI starts (~0.3 s), so the totals are pessimistic:
    # a real key holds its MQTT connection open and publishes in tens of ms.
    payload=$(python3 -c '
import json, sys, time
apns = {
    "aps": {"alert": {"title": "Sign in?", "body": "tinycrypt: approve this sign-in"},
            "category": "PRESENCE", "sound": "default"},
    "tc": {"id": sys.argv[1], "sentAt": int(time.time() * 1000)},
}
print(json.dumps({"default": "tinycrypt presence request", sys.argv[2]: json.dumps(apns)}))' "$id" "$platform")
    aws iot-data publish --endpoint-url "https://${endpoint}" --topic "$topic" \
        --cli-binary-format raw-in-base64-out --payload "$payload"
    echo "$(date +%H:%M:%S) published ${id}"
    if [ "$i" -lt "$count" ]; then sleep "$gap"; fi
done
