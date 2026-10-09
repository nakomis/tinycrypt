import * as cdk from 'aws-cdk-lib';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as iot from 'aws-cdk-lib/aws-iot';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as logs from 'aws-cdk-lib/aws-logs';
import { buildSync } from 'esbuild';
import * as path from 'path';
import { Construct } from 'constructs';

export interface PushStackProps extends cdk.StackProps {
    deployEnv: 'sandbox' | 'prod';
}

/**
 * CRYPT-12 spike: the key publishes a presence request over MQTT; an IoT rule hands it to a Lambda,
 * which sends an actionable APNs alert straight to the watch.
 *
 * Parameters (created by hand, not by this stack, so the key never passes through CloudFormation):
 *   /tinycrypt/{env}/apns/{team-id,key-id,private-key}  team-scoped APNs auth key (.p8 as a SecureString)
 *   /tinycrypt/{env}/watch/device-token                 the watch's APNs token, from its run log
 */
export class PushStack extends cdk.Stack {
    constructor(scope: Construct, id: string, props: PushStackProps) {
        super(scope, id, props);
        const { deployEnv } = props;

        const ownParams = `/tinycrypt/${deployEnv}`;
        const apnsParams = `${ownParams}/apns`;
        const requestTopic = `tinycrypt/${deployEnv}/presence/request`;

        // Bundled with the esbuild API: pnpm 11's `exec esbuild` runs the native binary through node,
        // which breaks NodejsFunction's own bundling.
        const lambdaDir = path.join(__dirname, '../lambda');
        const relay = new lambda.Function(this, 'PushRelay', {
            code: lambda.Code.fromAsset(lambdaDir, {
                bundling: {
                    image: lambda.Runtime.NODEJS_22_X.bundlingImage,
                    local: {
                        tryBundle(outputDir: string) {
                            buildSync({
                                entryPoints: [path.join(lambdaDir, 'push-relay.ts')],
                                bundle: true,
                                platform: 'node',
                                target: 'node22',
                                outfile: path.join(outputDir, 'index.js'),
                            });
                            return true;
                        },
                    },
                },
            }),
            handler: 'index.handler',
            runtime: lambda.Runtime.NODEJS_22_X,
            architecture: lambda.Architecture.ARM_64,
            memorySize: 512,
            timeout: cdk.Duration.seconds(10),
            // Never retry: a sign-in prompt that turns up minutes late could be approved for a request
            // that is no longer the one on screen. The key times out and asks again instead.
            retryAttempts: 0,
            maxEventAge: cdk.Duration.seconds(60),
            logGroup: new logs.LogGroup(this, 'PushRelayLogs', {
                retention: logs.RetentionDays.ONE_MONTH,
                removalPolicy: cdk.RemovalPolicy.DESTROY,
            }),
            environment: {
                APNS_PARAMS: apnsParams,
                TOKEN_PARAM: `${ownParams}/watch/device-token`,
                // A development-signed watch build only gets pushes from the APNs sandbox.
                APNS_HOST: deployEnv === 'prod' ? 'api.push.apple.com' : 'api.sandbox.push.apple.com',
                APNS_TOPIC: 'com.nakomis.tinycrypt.watch.watchkitapp',
            },
        });
        relay.addToRolePolicy(new iam.PolicyStatement({
            actions: ['ssm:GetParameter'],
            resources: [`arn:${this.partition}:ssm:${this.region}:${this.account}:parameter${ownParams}/*`],
        }));
        relay.addPermission('IotInvoke', {
            principal: new iam.ServicePrincipal('iot.amazonaws.com'),
            sourceAccount: this.account,
        });

        new iot.CfnTopicRule(this, 'PresenceRequestRule', {
            ruleName: `tinycrypt_presence_request_${deployEnv}`,
            topicRulePayload: {
                // iotAt: when IoT Core received the message, to split the latency by hop.
                sql: `SELECT *, timestamp() AS iotAt FROM '${requestTopic}'`,
                actions: [{ lambda: { functionArn: relay.functionArn } }],
                ruleDisabled: false,
                awsIotSqlVersion: '2016-03-23',
            },
        });

        new cdk.CfnOutput(this, 'RequestTopic', { value: requestTopic });
        new cdk.CfnOutput(this, 'RelayLogGroup', { value: relay.logGroup.logGroupName });
    }
}
