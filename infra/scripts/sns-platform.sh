#!/usr/bin/env bash
# CRYPT-12: the SNS pieces CloudFormation can't make. Safe to re-run.
#   1. The SNS platform application, holding the team-scoped APNs key (from SSM, never written
#      to disk here), with delivery status logging.
#   2. A platform endpoint for a device token, subscribed to a key's presence topic.
#
#   infra/scripts/sns-platform.sh [--phone] --app-only
#       just the platform application: tinycrypt-watch-{env}, or with --phone tinycrypt-phone-{env}
#       (CRYPT-30: one per bundle ID, the iPhone app's and the watch app's)
#   infra/scripts/sns-platform.sh [--phone] <device-token> --thing <key name, e.g. key-1>
#       subscribe a device to key-N's topic by hand. Normally a device registers itself by enrolling
#       with the key (CRYPT-30); this is for migration and repairs.
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox). A
# development-signed watch build only gets pushes from the APNs sandbox, hence APNS_SANDBOX.
set -euo pipefail

usage='usage: sns-platform.sh [--phone] --app-only | [--phone] <device-token> --thing key-N'
kind=watch
if [ "${1:-}" = --phone ]; then
    kind=phone
    shift
fi
token=${1:?$usage}
key=
if [ "$token" != --app-only ]; then
    [ "${2:-}" = --thing ] && [[ "${3:-}" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]] || { echo "$usage" >&2; exit 2; }
    key=$3
fi
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
platform=$([ "$env" = prod ] && echo APNS || echo APNS_SANDBOX)
name="tinycrypt-${kind}-${env}"
params="/tinycrypt/${env}/apns"

output() {
    aws cloudformation describe-stacks --stack-name TinycryptPushStack \
        --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}
feedback_role=$(output SnsDeliveryFeedbackRoleArn)

param() { aws ssm get-parameter --name "$1" ${2:+--with-decryption} --query Parameter.Value --output text; }
team_id=$(param "${params}/team-id")
key_id=$(param "${params}/key-id")
bundle_id=$([ "$kind" = phone ] && echo com.nakomis.tinycrypt.watch || echo com.nakomis.tinycrypt.watch.watchkitapp)

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

[ "$token" = --app-only ] && exit 0

topic_arn=$(aws cloudformation describe-stacks --stack-name TinycryptKeyStack \
    --query "Stacks[0].Outputs[?OutputKey=='PresenceTopicArn${key//-/}'].OutputValue" --output text)
[ -n "$topic_arn" ] && [ "$topic_arn" != None ] || { echo "no presence topic for $key in $env (infra/keys.json?)" >&2; exit 1; }

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
