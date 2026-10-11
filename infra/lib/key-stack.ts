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
 * requests. Its certificate is made and attached by `infra/scripts/provision-key.sh`, not here: the
 * private key must never pass through CloudFormation (and moves onto the ATECC608 later).
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
                        Resource: arn(`topic/tinycrypt/${deployEnv}/presence/request`),
                    },
                ],
            },
        });

        for (const name of keyNames) {
            const thingName = `tinycrypt-${deployEnv}-${name}`;
            new iot.CfnThing(this, `Thing-${name}`, { thingName });
            new cdk.CfnOutput(this, `ThingName-${name}`, { value: thingName });
        }
        new cdk.CfnOutput(this, 'KeyPolicyName', { value: policy.policyName! });
    }
}
