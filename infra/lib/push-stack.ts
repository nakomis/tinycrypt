import * as cdk from 'aws-cdk-lib';
import * as iam from 'aws-cdk-lib/aws-iam';
import { Construct } from 'constructs';

export interface PushStackProps extends cdk.StackProps {
    deployEnv: 'sandbox' | 'prod';
}

/**
 * CRYPT-12: the key publishes a presence request over MQTT; an IoT rule hands it straight to SNS,
 * which delivers it to the watch (or phone) through APNs. No Lambda, so no cold start on a path that
 * is almost always cold (sign-ins are rare).
 *
 * The key publishes the SNS message itself, in SNS's JSON message structure:
 *   {"default": "...", "APNS_SANDBOX" | "APNS": "<the APNs payload as a JSON string>"}
 *
 * CRYPT-30: the topics and rules are per key, in KeyStack. What's left here is shared by all keys.
 *
 * Made by hand with `infra/scripts/sns-platform.sh`, because CloudFormation has no resource for them:
 * the SNS platform applications (tinycrypt-watch-{env} and tinycrypt-phone-{env}: one per bundle
 * ID), holding the team-scoped APNs key (a .p8 must not pass through CloudFormation anyway).
 */
export class PushStack extends cdk.Stack {
    constructor(scope: Construct, id: string, props: PushStackProps) {
        super(scope, id, props);

        // SNS writes per-delivery logs for the platform applications (status, APNs response and
        // dwellTimeMs: how long SNS held the message), which splits out SNS's share of the latency.
        const feedbackRole = new iam.Role(this, 'SnsDeliveryFeedbackRole', {
            assumedBy: new iam.ServicePrincipal('sns.amazonaws.com'),
            inlinePolicies: {
                logs: new iam.PolicyDocument({
                    statements: [new iam.PolicyStatement({
                        actions: ['logs:CreateLogGroup', 'logs:CreateLogStream', 'logs:PutLogEvents', 'logs:PutRetentionPolicy'],
                        resources: [`arn:${this.partition}:logs:${this.region}:${this.account}:log-group:sns/${this.region}/${this.account}/app/*`],
                    })],
                }),
            },
        });

        new cdk.CfnOutput(this, 'SnsDeliveryFeedbackRoleArn', { value: feedbackRole.roleArn });
    }
}
