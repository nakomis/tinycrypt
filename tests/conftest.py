import os
import socket
import subprocess
import threading
from pathlib import Path

import pytest

from tinycrypt_udp import open_device

SIM = Path(os.environ.get("TINYCRYPT_SIM", Path(__file__).parent.parent / "build" / "tinycrypt-sim"))


def _free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Sim:
    """A running tinycrypt-sim process with its own state directory."""

    def __init__(self, state_dir: Path, presence: str = "auto"):
        self.state_dir = state_dir
        self.presence = presence
        self.port = _free_udp_port()
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [str(SIM), "--port", str(self.port), "--state-dir", str(self.state_dir),
             "--presence", self.presence],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        # solo1 logs to stdout too, so skip ahead to the readiness line.
        seen = []
        for line in self.proc.stdout:
            if line.startswith("READY"):
                # Keep draining both pipes so a chatty sim never blocks on a full pipe.
                for stream in (self.proc.stdout, self.proc.stderr):
                    threading.Thread(target=stream.read, daemon=True).start()
                return self
            seen.append(line)
        self.stop()
        raise RuntimeError(f"sim failed to start: {''.join(seen)} {self.proc.stderr.read()}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=5)

    def restart(self):
        self.stop()
        return self.start()

    def device(self):
        return open_device(self.port)


@pytest.fixture
def make_sim(tmp_path):
    if not SIM.exists():
        pytest.fail(f"{SIM} not built: cmake -S . -B build -DTINYCRYPT_INSECURE_SOFT_KEY=ON && cmake --build build")
    sims = []

    def factory(presence: str = "auto") -> Sim:
        sim = Sim(tmp_path, presence).start()
        sims.append(sim)
        return sim

    yield factory
    for sim in sims:
        sim.stop()


@pytest.fixture
def sim(make_sim):
    return make_sim()
