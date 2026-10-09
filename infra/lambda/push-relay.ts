// CRYPT-12 spike: IoT rule -> this Lambda -> APNs -> watch.
// Sends an actionable alert (category PRESENCE, the same one the watch's Approve/Deny handler uses)
// and logs a timestamp per hop so the latency can be split: sentAt (publisher clock), iotAt (IoT Core),
// lambdaAt, apnsAt (APNs accepted). The watch adds its own delivery time.
import { createPrivateKey, sign, type KeyObject } from 'node:crypto';
import { connect, constants, type ClientHttp2Session } from 'node:http2';
import { GetParameterCommand, SSMClient } from '@aws-sdk/client-ssm';

interface PresenceRequest {
    id?: string;
    sentAt?: number;
    iotAt?: number;
    title?: string;
    body?: string;
}

const ssm = new SSMClient({});
const { APNS_PARAMS, TOKEN_PARAM, APNS_HOST, APNS_TOPIC } = process.env as Record<string, string>;

async function param(name: string, decrypt = false): Promise<string> {
    const out = await ssm.send(new GetParameterCommand({ Name: name, WithDecryption: decrypt }));
    return out.Parameter!.Value!;
}

// Kept across warm invocations: APNs wants a provider token reused for 20-60 minutes, and a
// long-lived HTTP/2 connection saves the TLS handshake on every push.
let signer: { teamId: string; keyId: string; key: KeyObject } | undefined;
let jwt: { token: string; issuedAt: number } | undefined;
let session: ClientHttp2Session | undefined;

async function providerToken(): Promise<string> {
    signer ??= await (async () => {
        const [teamId, keyId, pem] = await Promise.all([
            param(`${APNS_PARAMS}/team-id`),
            param(`${APNS_PARAMS}/key-id`),
            param(`${APNS_PARAMS}/private-key`, true),
        ]);
        return { teamId, keyId, key: createPrivateKey(pem) };
    })();
    const now = Math.floor(Date.now() / 1000);
    if (!jwt || now - jwt.issuedAt > 40 * 60) {
        const b64 = (o: object) => Buffer.from(JSON.stringify(o)).toString('base64url');
        const input = `${b64({ alg: 'ES256', kid: signer.keyId })}.${b64({ iss: signer.teamId, iat: now })}`;
        const sig = sign('sha256', Buffer.from(input), { key: signer.key, dsaEncoding: 'ieee-p1363' });
        jwt = { token: `${input}.${sig.toString('base64url')}`, issuedAt: now };
    }
    return jwt.token;
}

function apns(): ClientHttp2Session {
    if (!session || session.closed || session.destroyed) {
        session = connect(`https://${APNS_HOST}`);
        session.on('error', () => { session = undefined; });
        session.on('goaway', () => { session = undefined; });
    }
    return session;
}

function post(deviceToken: string, token: string, body: string): Promise<{ status: number; reason?: string; apnsId?: string }> {
    return new Promise((resolve, reject) => {
        const req = apns().request({
            [constants.HTTP2_HEADER_METHOD]: 'POST',
            [constants.HTTP2_HEADER_PATH]: `/3/device/${deviceToken}`,
            authorization: `bearer ${token}`,
            'apns-topic': APNS_TOPIC,
            'apns-push-type': 'alert',
            'apns-priority': '10',
            'apns-expiration': '0',
            'content-type': 'application/json',
        });
        let status = 0;
        let apnsId: string | undefined;
        const chunks: Buffer[] = [];
        req.on('response', h => { status = Number(h[':status']); apnsId = h['apns-id'] as string | undefined; });
        req.on('data', (c: Buffer) => chunks.push(c));
        req.on('end', () => {
            let reason: string | undefined;
            try { reason = JSON.parse(Buffer.concat(chunks).toString()).reason; } catch { /* empty body on 200 */ }
            resolve({ status, reason, apnsId });
        });
        req.on('error', reject);
        req.end(body);
    });
}

export async function handler(event: PresenceRequest): Promise<void> {
    const lambdaAt = Date.now();
    const [token, deviceToken] = await Promise.all([providerToken(), param(TOKEN_PARAM)]);
    const body = JSON.stringify({
        aps: {
            alert: { title: event.title ?? 'Sign in?', body: event.body ?? 'tinycrypt: approve this sign-in' },
            category: 'PRESENCE',
            sound: 'default',
        },
        tc: { id: event.id, sentAt: event.sentAt, iotAt: event.iotAt, lambdaAt },
    });
    const sentToApns = Date.now();
    const res = await post(deviceToken, token, body);
    const apnsAt = Date.now();
    console.log(JSON.stringify({
        msg: 'pushed', id: event.id, status: res.status, reason: res.reason, apnsId: res.apnsId,
        publishToIotMs: event.sentAt && event.iotAt ? event.iotAt - event.sentAt : undefined,
        iotToLambdaMs: event.iotAt ? lambdaAt - event.iotAt : undefined,
        lambdaPrepMs: sentToApns - lambdaAt,
        apnsMs: apnsAt - sentToApns,
        publishToApnsAcceptedMs: event.sentAt ? apnsAt - event.sentAt : undefined,
    }));
    if (res.status !== 200) throw new Error(`APNs ${res.status} ${res.reason ?? ''}`);
}
