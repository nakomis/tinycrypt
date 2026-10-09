import * as cdk from 'aws-cdk-lib';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as iot from 'aws-cdk-lib/aws-iot';
import * as sns from 'aws-cdk-lib/aws-sns';
import { Construct } from 'constructs';

export interface PushStackProps extends cdk.StackProps {
    deployEnv: 'sandbox' | 'prod';
}

/**
 * CRYPT-12: the key publishes a presence request over MQTT; an IoT rule hands it straight to SNS,
 * which delivers it to the watch through APNs. No Lambda, so no cold start on a path that is
 * almost always cold (sign-ins are rare).
 *
 * The key publishes the SNS message itself, in SNS's JSON message structure:
 *   {"default": "...", "APNS_SANDBOX" | "APNS": "<the APNs payload as a JSON string>"}
 *
 * Made by hand with `infra/scripts/sns-platform.sh`, because CloudFormation has no resource for them:
 *   - the SNS platform application, holding the team-scoped APNs key (a .p8 must not pass
 *     through CloudFormation anyway);
 *   - the watch's platform endpoint (its device token), subscribed to this stack's topic.
 */
export class PushStack extends cdk.Stack {
    constructor(scope: Construct, id: string, props: PushStackProps) {
        super(scope, id, props);
        const { deployEnv } = props;

        const requestTopic = `tinycrypt/${deployEnv}/presence/request`;

        const topic = new sns.Topic(this, 'PresenceTopic', {
            topicName: `tinycrypt-presence-${deployEnv}`,
            displayName: 'tinycrypt presence requests',
        });

        const ruleRole = new iam.Role(this, 'PresenceRuleRole', {
            assumedBy: new iam.ServicePrincipal('iot.amazonaws.com'),
        });
        topic.grantPublish(ruleRole);

        new iot.CfnTopicRule(this, 'PresenceRequestRule', {
            ruleName: `tinycrypt_presence_request_${deployEnv}`,
            topicRulePayload: {
                sql: `SELECT * FROM '${requestTopic}'`,
                actions: [{ sns: { targetArn: topic.topicArn, roleArn: ruleRole.roleArn, messageFormat: 'JSON' } }],
                ruleDisabled: false,
                awsIotSqlVersion: '2016-03-23',
            },
        });

        // SNS writes per-delivery logs for the platform application (status, APNs response and
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

        new cdk.CfnOutput(this, 'RequestTopic', { value: requestTopic });
        new cdk.CfnOutput(this, 'PresenceTopicArn', { value: topic.topicArn });
        new cdk.CfnOutput(this, 'SnsDeliveryFeedbackRoleArn', { value: feedbackRole.roleArn });
    }
}
