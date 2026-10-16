#!/usr/bin/env python3
"""Drive the provisioner image (CRYPT-22, main/provisioner.c) over its console, the
native USB port (USB-Serial-JTAG).

    atecc_provision.py PORT info
    atecc_provision.py PORT lock            configure and lock the chip: PERMANENT, asks first
    atecc_provision.py PORT csr THING FILE  write the IoT key's CSR (PEM) for provision-key.sh --csr
    atecc_provision.py PORT check [full]    after locking: does the chip enforce its configuration?
                                            `full` also tries GenKey/PrivWrite on the key slots and
                                            rolls the master secret: the sacrificial chip only

Flash the provisioner first: KEYSTORE=atecc esp32s3/build.sh provisioner, then
esp32s3/flash-manual.sh --provisioner PORT (no BOOT+RST needed over the native port). Run with ESP-IDF's Python (pyserial).
"""

import base64
import json
import sys
import time

import serial

PREFIX = "TCPROV "


def command(port, line, timeout=20.0):
    port.reset_input_buffer()
    port.write((line + "\n").encode())
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = port.readline().decode(errors="replace").strip()
        if raw.startswith(PREFIX):
            reply = json.loads(raw[len(PREFIX):])
            if "error" in reply:
                sys.exit(f"provisioner: {reply['error']}")
            if "ready" not in reply:
                return reply
        elif raw:
            print(f"  | {raw}", file=sys.stderr)
    sys.exit(f"no answer to {line!r}: is the provisioner running on {port.port}?")


def wait_for_boot(port, quiet=2.0):
    # macOS raises DTR on open anyway, which resets the chip over USB-Serial-JTAG: let it boot
    # (until "ready", or the line stays quiet) before sending a command it would miss.
    deadline = time.monotonic() + quiet
    while time.monotonic() < deadline:
        raw = port.readline().decode(errors="replace").strip()
        if raw.startswith(PREFIX) and "ready" in raw:
            return
        if raw.startswith(PREFIX) and "error" in raw:
            sys.exit(f"provisioner: {json.loads(raw[len(PREFIX):])['error']}")
        if raw:
            deadline = time.monotonic() + quiet


def show(info):
    print(f"backend        {info['backend']}")
    print(f"serial         {info['serial']}")
    if "revision" in info:
        print(f"revision       {info['revision']}")
    print(f"config locked  {info['config_locked']}  (tinycrypt's: {info['config_ours']})")
    print(f"data locked    {info['data_locked']}")


def lock(port):
    info = command(port, "info")
    show(info)
    if info["config_locked"] and not info["config_ours"]:
        sys.exit("this chip is locked with someone else's configuration: it can't be used")
    if info["config_locked"] and info["data_locked"]:
        print("already locked; making any missing keys")
    else:
        print("\nLocking is PERMANENT: this chip will only ever hold tinycrypt's configuration.")
        typed = input(f"Type the serial ({info['serial']}) to lock it: ").strip()
        if typed.lower() != info["serial"].lower():
            sys.exit("not locked")
    command(port, f"lock {info['serial']}", timeout=60)
    show(command(port, "info"))


def csr(port, thing, path):
    reply = command(port, f"csr {thing}")
    der = bytes.fromhex(reply["csr"])
    b64 = base64.encodebytes(der).decode().replace("\n", "")
    pem = "-----BEGIN CERTIFICATE REQUEST-----\n"
    pem += "\n".join(b64[i:i + 64] for i in range(0, len(b64), 64))
    pem += "\n-----END CERTIFICATE REQUEST-----\n"
    with open(path, "w", encoding="ascii") as f:
        f.write(pem)
    print(f"wrote {path} (IoT key {reply['iot_pub'][:16]}…)")


def check(port, full):
    port.reset_input_buffer()
    port.write(("check full\n" if full else "check\n").encode())
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        raw = port.readline().decode(errors="replace").strip()
        if not raw.startswith(PREFIX):
            if raw:
                print(f"  | {raw}", file=sys.stderr)
            continue
        reply = json.loads(raw[len(PREFIX):])
        if "error" in reply:
            sys.exit(f"provisioner: {reply['error']}")
        if "check" in reply:
            print(f"{'ok  ' if reply['ok'] else 'FAIL'}  {reply['check']}")
        elif "counter_before" in reply:
            print(f"      sign counter was {reply['counter_before']}")
        elif "checked" in reply:
            if reply["failures"]:
                sys.exit(f"{reply['failures']} check(s) failed")
            print("all checks passed")
            return
    sys.exit("no answer to check")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    # Opening the port must not reset the chip (DTR/RTS drive EN and BOOT over USB-Serial-JTAG).
    port = serial.Serial(None, 115200, timeout=1)
    port.port = sys.argv[1]
    port.dtr = port.rts = False
    port.open()
    wait_for_boot(port)
    cmd = sys.argv[2]
    if cmd == "info":
        show(command(port, "info"))
    elif cmd == "lock":
        lock(port)
    elif cmd == "check" and len(sys.argv) in (3, 4):
        check(port, len(sys.argv) == 4 and sys.argv[3] == "full")
    elif cmd == "csr" and len(sys.argv) == 5:
        csr(port, sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
