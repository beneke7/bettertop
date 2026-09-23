#!/usr/bin/env python3
"""PTY smoke for normal quit and a stalled NVIDIA helper."""

import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path


def drain(master, output, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            try:
                output.extend(os.read(master, 65536))
            except OSError:
                return


def run(binary, args, env, duration, predicate=None, terminate_signal=None):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    before = termios.tcgetattr(slave)
    process = subprocess.Popen([str(binary), *args], stdin=slave, stdout=slave, stderr=slave, env=env)
    output = bytearray()
    try:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline and not (predicate and predicate(output)):
            drain(master, output, 0.1)
        if terminate_signal is None:
            os.write(master, b"q")
            expected_code = 0
        else:
            process.send_signal(terminate_signal)
            expected_code = 128 + terminate_signal
        code = process.wait(timeout=5)
        drain(master, output, 0.1)
        after = termios.tcgetattr(slave)
        pendin = getattr(termios, "PENDIN", 0)
        assert code == expected_code, f"{binary.name} exited with {code}, expected {expected_code}"
        assert before[:3] == after[:3] and before[4:] == after[4:], "terminal attributes changed"
        assert before[3] & ~pendin == after[3] & ~pendin, "terminal local flags changed"
        assert b"\x00" not in output, "terminal output contains NUL bytes"
        assert b"\x1b[?1049l" in output, "alternate screen was not restored"
        return output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def main():
    if not sys.platform.startswith("linux"):
        raise SystemExit("Linux only")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="bettertop-pty-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.update(
            HOME=str(root / "home"),
            XDG_CONFIG_HOME=str(root / "config"),
            XDG_STATE_HOME=str(root / "state"),
            TERM="xterm-256color",
        )
        output = run(binary, ["--classic"], env, 2)
        assert len(output) > 1000, "classic view did not render"
        print("PTY quit passed: classic view, terminal restored")
        run(binary, ["--classic"], env, 1, terminate_signal=signal.SIGTERM)
        print("PTY signal passed: SIGTERM, terminal restored")

        fake_dir = root / "fake-bin"
        fake_dir.mkdir()
        fake_ui = fake_dir / "bettertop"
        shutil.copy2(binary, fake_ui)
        helper_log = root / "helper-pids"
        ml_env = env.copy()
        ml_env.update(
            XDG_CONFIG_HOME=str(root / "ml-config"),
            XDG_STATE_HOME=str(root / "ml-state"),
            BETTERTOP_HELPER_LOG=str(helper_log),
        )
        fake_helper = fake_dir / "bettertop-gpu"
        fake_helper.write_text(
            "#!/usr/bin/env python3\n"
            "import os, signal, struct, time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "fd = os.open('/dev/null', os.O_RDONLY); os.dup2(fd, 0); os.close(fd)\n"
            "fd = os.open('/dev/null', os.O_WRONLY); os.dup2(fd, 2); os.close(fd)\n"
            "with open(os.environ['BETTERTOP_HELPER_LOG'], 'a') as log:\n"
            "    log.write(str(os.getpid()) + '\\n'); log.flush()\n"
            "frame = struct.pack('<4sHHIIQQIIII', b'BTGP', 1, 48, 48, 0, 1, time.monotonic_ns(), 0, 0, 0, 0)\n"
            "os.write(1, frame)\n"
            "while True: time.sleep(1)\n"
        )
        fake_helper.chmod(0o755)

        try:
            output = run(fake_ui, ["--update", "100"], ml_env, 8,
                         lambda captured: b"stale" in captured and helper_log.exists()
                         and len(helper_log.read_text().splitlines()) >= 2)
            assert b"CPU " in output, "host metrics stopped while the helper stalled"
            assert b"stale" in output, f"stalled helper was not marked stale; terminal tail: {output[-3000:]!r}"
            pids = [int(value) for value in helper_log.read_text().splitlines()]
            assert len(pids) >= 2, "helper did not restart after the first stalled child was reaped"
            assert len(pids) <= 3, f"helper restarted too often: {len(pids)} children"
            assert all(not Path(f"/proc/{pid}").exists() for pid in pids), "helper child survived UI quit"
            print("PTY stall passed: host stayed responsive, stale status shown, helper reaped")
        finally:
            if helper_log.exists():
                for value in helper_log.read_text().splitlines():
                    try:
                        os.kill(int(value), signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    main()
