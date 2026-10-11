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
    assert time.time() - start < 2


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
