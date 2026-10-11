#!/usr/bin/env node
import * as cdk from 'aws-cdk-lib';
import { KeyStack } from '../lib/key-stack';
import { PushStack } from '../lib/push-stack';

const npmEnvironment = process.env.NPM_ENVIRONMENT;
if (npmEnvironment !== 'sandbox' && npmEnvironment !== 'prod') {
    throw new Error('NPM_ENVIRONMENT must be "sandbox" or "prod". Use `pnpm run deploy-sandbox`.');
}
const deployEnv = npmEnvironment;

// The account comes from the profile the deploy runs under, never a literal.
const env = { account: process.env.CDK_DEFAULT_ACCOUNT, region: process.env.CDK_DEFAULT_REGION ?? 'eu-west-2' };

const app = new cdk.App();

new PushStack(app, 'TinycryptPushStack', {
    env,
    deployEnv,
    description: `tinycrypt presence push: IoT rule -> SNS -> APNs -> watch (${deployEnv})`,
});

// One IoT Thing per physical key: `-c keys=key-1,key-2` (default key-1).
const keyNames = String(app.node.tryGetContext('keys') ?? 'key-1').split(',').map(k => k.trim()).filter(Boolean);

new KeyStack(app, 'TinycryptKeyStack', {
    env,
    deployEnv,
    keyNames,
    description: `tinycrypt security keys as IoT Things, and what they may publish (${deployEnv})`,
});
