"""Presence is the button OR the watch (CRYPT-13), and the wait's CTAPHID channel rules.

Runs against the sim with --presence button+watch: stdin is the button, and the watch is scripted
to approve --watch-after ms into each wait. Raw CTAPHID packets, so channels and keepalives are
visible.
"""

import os
import socket
import struct
import time

import pytest
from fido2 import cbor

REPORT = 64
BROADCAST = b"\xff\xff\xff\xff"
INIT, PING, MSG, CBOR, CANCEL, ERROR, KEEPALIVE = 0x86, 0x81, 0x83, 0x90, 0x91, 0xBF, 0xBB
CHANNEL_BUSY = 0x06
ACTION_TIMEOUT = 0x3A  # solo1's CTAP2_ERR_ACTION_TIMEOUT


class RawHid:
    def __init__(self, port):
        self.addr = ("127.0.0.1", port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))

    def close(self):
        self.sock.close()

    def send(self, cid, cmd, payload=b""):
        first = cid + bytes([cmd]) + struct.pack(">H", len(payload)) + payload[:57]
        self.sock.sendto(first.ljust(REPORT, b"\0"), self.addr)
        rest, seq = payload[57:], 0
        while rest:
            self.sock.sendto((cid + bytes([seq]) + rest[:59]).ljust(REPORT, b"\0"), self.addr)
            rest, seq = rest[59:], seq + 1

    def recv(self, timeout):
        """One whole message: (cid, cmd, payload), or None on timeout."""
        self.sock.settimeout(timeout)
        try:
            pkt, _ = self.sock.recvfrom(REPORT)
        except socket.timeout:
            return None
        cid, cmd, n = pkt[:4], pkt[4], struct.unpack(">H", pkt[5:7])[0]
        data = pkt[7:7 + n]
        while len(data) < n:
            cont, _ = self.sock.recvfrom(REPORT)
            data += cont[5:5 + n - len(data)]
        return cid, cmd, data

    def allocate(self):
        nonce = os.urandom(8)
        self.send(BROADCAST, INIT, nonce)
        while True:
            cid, cmd, data = self.recv(2)
            if cmd == INIT and data[:8] == nonce:
                return data[8:12]

    def until(self, cid, cmds, timeout):
        """Reads until a message on `cid` with a command in `cmds`; returns (msgs_seen, last)."""
        seen, end = [], time.time() + timeout
        while time.time() < end:
            msg = self.recv(max(0.01, end - time.time()))
            if msg is None:
                break
            seen.append(msg)
            if msg[0] == cid and msg[1] in cmds:
                return seen, msg
        return seen, None


def make_credential():
    return b"\x01" + cbor.encode({
        1: os.urandom(32),
        2: {"id": "example.com", "name": "Example"},
        3: {"id": b"presence-test", "name": "acme", "displayName": "Acme"},
        4: [{"type": "public-key", "alg": -7}],
    })


@pytest.fixture
def raw(make_sim):
    hids = []

    def factory(*args):
        sim = make_sim("button+watch", *args)
        hid = RawHid(sim.port)
        hids.append(hid)
        return sim, hid

    yield factory
    for hid in hids:
        hid.close()


def test_button_wins(raw):
    sim, hid = raw("--watch-after", "20000", "--up-timeout", "10000")
    a = hid.allocate()
    hid.send(a, CBOR, make_credential())
    time.sleep(0.3)
    sim.press_button()
    _, resp = hid.until(a, {CBOR}, 3)
    assert resp is not None and resp[2][0] == 0


def test_watch_wins(raw):
    _, hid = raw("--watch-after", "300", "--up-timeout", "10000")
    a = hid.allocate()
    start = time.time()
    hid.send(a, CBOR, make_credential())
    _, resp = hid.until(a, {CBOR}, 3)
    assert resp is not None and resp[2][0] == 0
    assert time.time() - start < 3  # watch approves at 0.3 s; slack for a busy runner


def test_both_time_out(raw):
    _, hid = raw("--watch-after", "0", "--up-timeout", "1000")
    a = hid.allocate()
    hid.send(a, CBOR, make_credential())
    _, resp = hid.until(a, {CBOR}, 5)
    assert resp is not None and resp[2][0] == ACTION_TIMEOUT


def test_keepalives_go_to_the_requesting_channel(raw):
    _, hid = raw("--watch-after", "800", "--up-timeout", "10000")
    a = hid.allocate()
    hid.send(a, CBOR, make_credential())
    seen, resp = hid.until(a, {CBOR}, 3)
    keepalives = [m for m in seen if m[1] == KEEPALIVE]
    assert len(keepalives) >= 3
    assert all(m[0] == a for m in keepalives)
    assert resp is not None and resp[2][0] == 0


def test_other_channels_get_busy_and_cannot_cancel(raw):
    _, hid = raw("--watch-after", "1500", "--up-timeout", "10000")
    a = hid.allocate()
    b = hid.allocate()
    hid.send(a, CBOR, make_credential())
    time.sleep(0.2)

    hid.send(b, CANCEL)  # nothing to cancel on b: ignored
    hid.send(b, PING, b"hello")
    _, err = hid.until(b, {ERROR}, 1)
    assert err is not None and err[2][0] == CHANNEL_BUSY

    hid.send(BROADCAST, INIT, os.urandom(8))  # a new client while busy
    _, err = hid.until(BROADCAST, {ERROR}, 1)
    assert err is not None and err[2][0] == CHANNEL_BUSY

    # The watch still approves request a, and its keepalives were never diverted.
    seen, resp = hid.until(a, {CBOR}, 3)
    assert resp is not None and resp[2][0] == 0
    assert all(m[0] == a for m in seen if m[1] == KEEPALIVE)

    # Afterwards the device serves everyone again.
    assert hid.allocate()


def test_cancel_on_the_requesting_channel_ends_the_wait(raw):
    _, hid = raw("--watch-after", "0", "--up-timeout", "10000")
    a = hid.allocate()
    hid.send(a, CBOR, make_credential())
    time.sleep(0.3)
    start = time.time()
    hid.send(a, CANCEL)
    _, resp = hid.until(a, {CBOR}, 2)
    assert resp is not None
    # Not KEEPALIVE_CANCEL: the presence contract allows only 1/0 (see port/presence.h).
    assert resp[2][0] == ACTION_TIMEOUT
    assert time.time() - start < 1


def test_cancelled_u2f_register_is_not_signed(raw):
    _, hid = raw("--watch-after", "0", "--up-timeout", "10000")
    a = hid.allocate()
    apdu = bytes([0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x40]) + os.urandom(64) + b"\x00\x00"
    hid.send(a, MSG, apdu)
    time.sleep(0.2)
    hid.send(a, CANCEL)
    _, resp = hid.until(a, {MSG}, 3)
    assert resp is not None
    assert resp[2][-2:] != b"\x90\x00"  # never a successful registration


def test_init_on_the_requesting_channel_aborts_and_is_answered(raw):
    _, hid = raw("--watch-after", "0", "--up-timeout", "10000")
    a = hid.allocate()
    hid.send(a, CBOR, make_credential())
    time.sleep(0.3)
    nonce = os.urandom(8)
    hid.send(a, INIT, nonce)  # resynchronise: aborts the request
    seen, resp = hid.until(a, {INIT}, 2)
    assert resp is not None and resp[2][:8] == nonce and resp[2][8:12] == a
    # The aborted request gets no answer at all, before or after the INIT reply.
    assert not [m for m in seen if m[0] == a and m[1] == CBOR]
    _, late = hid.until(a, {CBOR}, 0.5)
    assert late is None


def test_an_intruder_mid_request_cannot_take_over_the_wait(raw):
    _, hid = raw("--watch-after", "0", "--up-timeout", "10000")
    a = hid.allocate()
    b = hid.allocate()
    request = make_credential() + os.urandom(80)  # pad to force continuation packets
    # a's first packet, then b barges in, then a's continuations complete the request.
    first = a + bytes([CBOR]) + struct.pack(">H", len(request)) + request[:57]
    hid.sock.sendto(first.ljust(REPORT, b"\0"), hid.addr)
    hid.send(b, CBOR, make_credential())
    _, busy = hid.until(b, {ERROR}, 1)
    assert busy is not None and busy[2][0] == CHANNEL_BUSY
    rest, seq = request[57:], 0
    while rest:
        hid.sock.sendto((a + bytes([seq]) + rest[:59]).ljust(REPORT, b"\0"), hid.addr)
        rest, seq = rest[59:], seq + 1
    # The wait belongs to a: b can't cancel it, a can.
    seen, _ = hid.until(a, {KEEPALIVE}, 1)
    hid.send(b, CANCEL)
    seen, resp = hid.until(a, {CBOR}, 0.5)
    assert resp is None
    hid.send(a, CANCEL)
    _, resp = hid.until(a, {CBOR}, 2)
    assert resp is not None


def actions(sim):
    return [line for line in sim.log if line.endswith(" action")]


def u2f_register():
    return bytes([0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x40]) + os.urandom(64) + b"\x00\x00"


def test_u2f_re_asks_are_one_action_but_a_new_channel_is_not(raw):
    sim, hid = raw("--watch-after", "0", "--up-timeout", "300")
    a = hid.allocate()
    request = u2f_register()
    for _ in range(3):  # the browser's U2F retry loop: the same request, short waits back to back
        hid.send(a, MSG, request)
        hid.until(a, {MSG}, 2)
    b = hid.allocate()
    hid.send(b, MSG, request)
    hid.until(b, {MSG}, 2)
    time.sleep(0.2)
    assert actions(sim) == [">>> new action", ">>> same action", ">>> same action", ">>> new action"]


def test_a_retry_after_a_full_timeout_or_a_cancel_is_a_new_action(raw):
    sim, hid = raw("--watch-after", "0", "--up-timeout", "2500")
    a = hid.allocate()
    hid.send(a, CBOR, make_credential())
    hid.until(a, {CBOR}, 5)              # times out after a long wait
    hid.send(a, CBOR, make_credential())  # the client retries at once
    time.sleep(0.3)
    hid.send(a, CANCEL)
    hid.until(a, {CBOR}, 2)
    hid.send(a, CBOR, make_credential())  # straight after a cancel
    time.sleep(0.3)
    hid.send(a, CANCEL)
    hid.until(a, {CBOR}, 2)
    time.sleep(0.2)
    assert actions(sim) == [">>> new action"] * 3


def requests(sim):
    return [line for line in sim.log if line.startswith(">>> request ")]


def test_the_wait_knows_the_site_and_the_request_hash(raw):
    import hashlib
    sim, hid = raw("--watch-after", "100", "--up-timeout", "5000")
    a = hid.allocate()
    payload = make_credential()
    hid.send(a, CBOR, payload)
    hid.until(a, {CBOR}, 3)
    time.sleep(0.2)
    assert requests(sim)[-1] == f">>> request rp=example.com hash={hashlib.sha256(payload).hexdigest()}"


def test_a_different_request_is_a_new_action_even_on_the_same_channel(raw):
    sim, hid = raw("--watch-after", "0", "--up-timeout", "300")
    a = hid.allocate()
    hid.send(a, MSG, u2f_register())
    hid.until(a, {MSG}, 2)
    hid.send(a, MSG, u2f_register())  # a fresh challenge: another request
    hid.until(a, {MSG}, 2)
    time.sleep(0.2)
    assert actions(sim) == [">>> new action", ">>> new action"]
    assert all(r.startswith(">>> request rp= hash=") for r in requests(sim))  # U2F: no site name
