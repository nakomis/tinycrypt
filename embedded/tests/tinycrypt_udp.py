"""python-fido2 transport for the tinycrypt Mac sim.

Each UDP datagram is one 64-byte CTAPHID report, so python-fido2's own
CTAPHID framing (CtapHidDevice) runs unchanged on top of it.
"""

import socket

from fido2.hid import CtapHidDevice
from fido2.hid.base import CtapHidConnection, HidDescriptor

REPORT_SIZE = 64


class UdpConnection(CtapHidConnection):
    def __init__(self, host: str, port: int, timeout: float = 30.0):
        self._addr = (host, port)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.settimeout(timeout)

    def write_packet(self, data: bytes) -> None:
        self._sock.sendto(data.ljust(REPORT_SIZE, b"\0"), self._addr)

    def read_packet(self) -> bytes:
        data, _ = self._sock.recvfrom(REPORT_SIZE)
        return data

    def close(self) -> None:
        self._sock.close()


def open_device(port: int, host: str = "127.0.0.1") -> CtapHidDevice:
    descriptor = HidDescriptor(
        path=f"udp://{host}:{port}",
        vid=0,
        pid=0,
        report_size_in=REPORT_SIZE,
        report_size_out=REPORT_SIZE,
        product_name="tinycrypt sim",
        serial_number=None,
    )
    return CtapHidDevice(descriptor, UdpConnection(host, port))
