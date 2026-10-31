#!/usr/bin/env node
import * as fs from 'fs';
import * as path from 'path';
import * as cdk from 'aws-cdk-lib';
import { GithubCiStack } from '../lib/github-ci-stack';
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

// One IoT Thing per physical key, listed per environment in infra/keys.json (CRYPT-26). Committed,
// so every deploy declares every key: a key left out of a deploy would have its Thing deleted.
const keyNames: string[] = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'keys.json'), 'utf8'))[deployEnv];
if (!Array.isArray(keyNames) || keyNames.some(k => typeof k !== 'string' || !/^[a-z0-9]+(-[a-z0-9]+)*$/.test(k) || k.length > 32) || new Set(keyNames).size !== keyNames.length) {
    throw new Error(`infra/keys.json: "${deployEnv}" must be a list of distinct names like key-1 or nakomis (lower case, digits, single hyphens, at most 32)`);
}

new KeyStack(app, 'TinycryptKeyStack', {
    env,
    deployEnv,
    keyNames,
    description: `tinycrypt security keys as IoT Things, and what they may publish (${deployEnv})`,
});

new GithubCiStack(app, 'TinycryptGithubCiStack', {
    env,
    deployEnv,
    description: `tinycrypt GitHub Actions role: sign and publish firmware (${deployEnv})`,
});
