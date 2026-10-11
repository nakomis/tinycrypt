import * as cdk from 'aws-cdk-lib';
import * as iot from 'aws-cdk-lib/aws-iot';
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

        for (const name of keyNames) {
            const thingName = `tinycrypt-${deployEnv}-${name}`;
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
        new cdk.CfnOutput(this, 'KeyPolicyName', { value: policy.policyName! });
    }
}
