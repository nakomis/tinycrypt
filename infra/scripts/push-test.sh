#!/usr/bin/env bash
# CRYPT-12: publish presence requests to IoT Core as the key would, from the Mac.
# Each message carries sentAt (ms since the epoch); the Lambda and the watch log their own
# timestamps against it.
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

endpoint=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)
topic="tinycrypt/${env}/presence/request"

for i in $(seq 1 "$count"); do
    id="run-$(date +%H%M%S)-$i"
    now=$(python3 -c 'import time; print(int(time.time() * 1000))')
    aws iot-data publish --endpoint-url "https://${endpoint}" --topic "$topic" \
        --cli-binary-format raw-in-base64-out \
        --payload "{\"id\":\"${id}\",\"sentAt\":${now}}"
    echo "$(date +%H:%M:%S) published ${id} sentAt=${now}"
    [ "$i" -lt "$count" ] && sleep "$gap"
done
