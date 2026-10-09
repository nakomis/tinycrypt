#!/usr/bin/env bash
# CRYPT-12: the SNS pieces CloudFormation can't make. Safe to re-run.
#   1. The SNS platform application, holding the team-scoped APNs key (from SSM, never written
#      to disk here), with delivery status logging.
#   2. A platform endpoint for the watch's device token, subscribed to the presence topic.
#
#   infra/scripts/sns-platform.sh <watch-device-token>
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox). A
# development-signed watch build only gets pushes from the APNs sandbox, hence APNS_SANDBOX.
set -euo pipefail

token=${1:?usage: sns-platform.sh <watch-device-token>}
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
platform=$([ "$env" = prod ] && echo APNS || echo APNS_SANDBOX)
name="tinycrypt-watch-${env}"
params="/tinycrypt/${env}/apns"

output() {
    aws cloudformation describe-stacks --stack-name TinycryptPushStack \
        --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}
topic_arn=$(output PresenceTopicArn)
feedback_role=$(output SnsDeliveryFeedbackRoleArn)

param() { aws ssm get-parameter --name "$1" ${2:+--with-decryption} --query Parameter.Value --output text; }
team_id=$(param "${params}/team-id")
key_id=$(param "${params}/key-id")
bundle_id=com.nakomis.tinycrypt.watch.watchkitapp

# The key goes in as a JSON-encoded attribute file so it never appears on a command line.
attrs=$(mktemp)
trap 'rm -f "$attrs"' EXIT
param "${params}/private-key" decrypt | python3 -c '
import json, sys
print(json.dumps({
    "PlatformCredential": sys.stdin.read(),
    "PlatformPrincipal": sys.argv[1],
    "ApplePlatformTeamID": sys.argv[2],
    "ApplePlatformBundleID": sys.argv[3],
    "SuccessFeedbackRoleArn": sys.argv[4],
    "FailureFeedbackRoleArn": sys.argv[4],
    "SuccessFeedbackSampleRate": "100",
}))' "$key_id" "$team_id" "$bundle_id" "$feedback_role" > "$attrs"

app_arn=$(aws sns list-platform-applications \
    --query "PlatformApplications[?ends_with(PlatformApplicationArn, ':app/${platform}/${name}')].PlatformApplicationArn" \
    --output text)
if [ -z "$app_arn" ]; then
    app_arn=$(aws sns create-platform-application --name "$name" --platform "$platform" \
        --attributes "file://${attrs}" --query PlatformApplicationArn --output text)
    echo "created platform application ${app_arn}"
else
    aws sns set-platform-application-attributes --platform-application-arn "$app_arn" --attributes "file://${attrs}"
    echo "updated platform application ${app_arn}"
fi

# Creating an endpoint for an existing token returns the existing endpoint.
endpoint_arn=$(aws sns create-platform-endpoint --platform-application-arn "$app_arn" --token "$token" \
    --query EndpointArn --output text)
aws sns set-endpoint-attributes --endpoint-arn "$endpoint_arn" --attributes Enabled=true
echo "endpoint ${endpoint_arn}"

existing=$(aws sns list-subscriptions-by-topic --topic-arn "$topic_arn" \
    --query "Subscriptions[?Endpoint=='${endpoint_arn}'].SubscriptionArn" --output text)
if [ -z "$existing" ]; then
    aws sns subscribe --topic-arn "$topic_arn" --protocol application --notification-endpoint "$endpoint_arn" \
        --query SubscriptionArn --output text
else
    echo "already subscribed: ${existing}"
fi
