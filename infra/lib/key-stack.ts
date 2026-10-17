import * as path from 'path';
import * as cdk from 'aws-cdk-lib';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as iot from 'aws-cdk-lib/aws-iot';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as logs from 'aws-cdk-lib/aws-logs';
import * as sns from 'aws-cdk-lib/aws-sns';
import { Construct } from 'constructs';

export interface KeyStackProps extends cdk.StackProps {
    deployEnv: 'sandbox' | 'prod';
    /** One IoT Thing per physical key, e.g. ['key-1']. */
    keyNames: string[];
}

/**
 * CRYPT-15: each security key is an IoT Thing that may only connect as itself and publish presence
 * requests on its own topic, tinycrypt/{env}/presence/{thing}/request.
 * CRYPT-14: it also takes Fleet's commands on tinycrypt/{thing}/commands and answers on
 * tinycrypt/{thing}/responses (its own only), and its attributes tell Fleet to use the secure tier:
 * signed images only, and an install the key's owner approves. Its certificate is made and
 * attached by `infra/scripts/provision-key.sh`, not here: the private key must never pass through
 * CloudFormation (and moves onto the ATECC608 later).
 *
 * CRYPT-30: each key has its own SNS topic, fed by its own IoT rule from its presence request topic,
 * so its pushes reach only its own watch and phone. (An IoT SNS action's target can't be templated
 * by topic, hence a rule per key.) Still no Lambda on that path. A watch or phone joins a key's topic
 * by enrolling with the key over BLE: the key publishes the device's APNs token to
 * tinycrypt/{env}/register/{thing}, and the register Lambda subscribes it.
 */
export class KeyStack extends cdk.Stack {
    constructor(scope: Construct, id: string, props: KeyStackProps) {
        super(scope, id, props);
        const { deployEnv, keyNames } = props;

        const arn = (resource: string) => `arn:${this.partition}:iot:${this.region}:${this.account}:${resource}`;

        // Policy variables pin each certificate to its own Thing: the client id must be the Thing's
        // name, and the certificate must be attached to that Thing.
        const policy = new iot.CfnPolicy(this, 'KeyPolicy', {
            policyName: `tinycrypt-key-${deployEnv}`,
            policyDocument: {
                Version: '2012-10-17',
                Statement: [
                    {
                        Effect: 'Allow',
                        Action: 'iot:Connect',
                        Resource: arn('client/${iot:Connection.Thing.ThingName}'),
                        Condition: { Bool: { 'iot:Connection.Thing.IsAttached': 'true' } },
                    },
                    {
                        Effect: 'Allow',
                        Action: 'iot:Publish',
                        // Its own topic only, so a key can't pose as another.
                        Resource: arn(`topic/tinycrypt/${deployEnv}/presence/\${iot:Connection.Thing.ThingName}/request`),
                    },
                    {
                        Effect: 'Allow',
                        Action: 'iot:Publish',
                        // CRYPT-30: registers its own enrolled devices only.
                        Resource: arn(`topic/tinycrypt/${deployEnv}/register/\${iot:Connection.Thing.ThingName}`),
                    },
                    {
                        Effect: 'Allow',
                        Action: 'iot:Publish',
                        Resource: arn('topic/tinycrypt/${iot:Connection.Thing.ThingName}/responses'),
                    },
                    {
                        Effect: 'Allow',
                        Action: 'iot:Subscribe',
                        Resource: arn('topicfilter/tinycrypt/${iot:Connection.Thing.ThingName}/commands'),
                    },
                    {
                        Effect: 'Allow',
                        Action: 'iot:Receive',
                        Resource: arn('topic/tinycrypt/${iot:Connection.Thing.ThingName}/commands'),
                    },
                ],
            },
        });

        const ruleRole = new iam.Role(this, 'PresenceRuleRole', {
            assumedBy: new iam.ServicePrincipal('iot.amazonaws.com'),
        });

        for (const name of keyNames) {
            const thingName = `tinycrypt-${deployEnv}-${name}`;
            // Named after the Thing, so the register Lambda finds it from the topic alone.
            const topic = new sns.Topic(this, `PresenceTopic-${name}`, {
                topicName: thingName,
                displayName: `tinycrypt ${name} presence requests`,
            });
            topic.grantPublish(ruleRole);
            new iot.CfnTopicRule(this, `PresenceRule-${name}`, {
                ruleName: `tinycrypt_presence_${deployEnv}_${name.replace('-', '_')}`,
                topicRulePayload: {
                    sql: `SELECT * FROM 'tinycrypt/${deployEnv}/presence/${thingName}/request'`,
                    actions: [{ sns: { targetArn: topic.topicArn, roleArn: ruleRole.roleArn, messageFormat: 'JSON' } }],
                    ruleDisabled: false,
                    awsIotSqlVersion: '2016-03-23',
                },
            });
            new cdk.CfnOutput(this, `PresenceTopicArn-${name}`, { value: topic.topicArn });
            new iot.CfnThing(this, `Thing-${name}`, {
                thingName,
                // Read by Fleet (nakomis-fleet): ota-signed makes it refuse unsigned builds for this key,
                // ota-approval makes it expect awaiting_approval / approved from the key.
                attributePayload: {
                    attributes: { project: 'tinycrypt', deviceType: 'fido-key', capabilities: 'ota,ota-signed,ota-approval' },
                },
            });
            new cdk.CfnOutput(this, `ThingName-${name}`, { value: thingName });
        }

        // Registration (enrolment only, never on the sign-in path): token in, subscription out.
        const prefix = `tinycrypt-${deployEnv}-`;
        const register = new lambda.Function(this, 'RegisterFunction', {
            functionName: `tinycrypt-register-${deployEnv}`,
            runtime: lambda.Runtime.PYTHON_3_13,
            handler: 'register.handler',
            code: lambda.Code.fromAsset(path.join(__dirname, '..', 'lambda', 'register'), { exclude: ['test_*', '__pycache__'] }),
            timeout: cdk.Duration.seconds(30),
            environment: {
                THING_PREFIX: prefix,
                // The platform applications are made by sns-platform.sh (CloudFormation can't).
                PLATFORM: deployEnv === 'prod' ? 'APNS' : 'APNS_SANDBOX',
                WATCH_APP: `tinycrypt-watch-${deployEnv}`,
                PHONE_APP: `tinycrypt-phone-${deployEnv}`,
            },
            logGroup: new logs.LogGroup(this, 'RegisterLogs', {
                logGroupName: `/aws/lambda/tinycrypt-register-${deployEnv}`,
                retention: logs.RetentionDays.THREE_MONTHS,
                removalPolicy: cdk.RemovalPolicy.DESTROY,
            }),
        });
        const snsArn = (resource: string) => `arn:${this.partition}:sns:${this.region}:${this.account}:${resource}`;
        register.addToRolePolicy(new iam.PolicyStatement({
            actions: ['sns:CreatePlatformEndpoint', 'sns:ListEndpointsByPlatformApplication'],
            resources: [snsArn(`app/*/tinycrypt-watch-${deployEnv}`), snsArn(`app/*/tinycrypt-phone-${deployEnv}`)],
        }));
        register.addToRolePolicy(new iam.PolicyStatement({
            actions: ['sns:GetEndpointAttributes', 'sns:SetEndpointAttributes', 'sns:DeleteEndpoint'],
            resources: [snsArn(`endpoint/*/tinycrypt-watch-${deployEnv}/*`), snsArn(`endpoint/*/tinycrypt-phone-${deployEnv}/*`)],
        }));
        register.addToRolePolicy(new iam.PolicyStatement({
            actions: ['sns:Subscribe', 'sns:ListSubscriptionsByTopic'],
            resources: [snsArn(`${prefix}*`)],
        }));
        register.addToRolePolicy(new iam.PolicyStatement({
            // Unsubscribe takes a subscription ARN; ListSubscriptions (for an endpoint's other keys)
            // can't be scoped.
            actions: ['sns:Unsubscribe', 'sns:ListSubscriptions'],
            resources: ['*'],
        }));
        const registerRule = new iot.CfnTopicRule(this, 'RegisterRule', {
            ruleName: `tinycrypt_register_${deployEnv}`,
            topicRulePayload: {
                // The Thing comes from the topic, which the key's policy limits to its own name.
                sql: `SELECT *, topic(4) AS thing FROM 'tinycrypt/${deployEnv}/register/+'`,
                actions: [{ lambda: { functionArn: register.functionArn } }],
                ruleDisabled: false,
                awsIotSqlVersion: '2016-03-23',
            },
        });
        register.addPermission('IotInvoke', {
            principal: new iam.ServicePrincipal('iot.amazonaws.com'),
            sourceArn: registerRule.attrArn,
        });
        new cdk.CfnOutput(this, 'KeyPolicyName', { value: policy.policyName! });
    }
}
