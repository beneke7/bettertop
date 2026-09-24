#!/usr/bin/env python3
"""PTY and fake-worker checks for BetterTop's Linux UI and GPU bridge."""

import fcntl
import os
import pty
import re
import resource
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


CSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
CURSOR_ROW = re.compile(rb"\x1b\[(\d+);1f")
RESTORE_SEQUENCES = (
    b"\x1b[?2026l", b"\x1b[?1002l", b"\x1b[?1015l", b"\x1b[?1006l",
    b"\x1b[?1003l", b"\x1b[0m", b"\x1b[?1049l", b"\x1b[?25h",
)

FAKE_HELPER = r'''#!/usr/bin/env python3
import os, signal, struct, time
from pathlib import Path

mode = os.environ["BETTERTOP_HELPER_MODE"]
log_path = Path(os.environ["BETTERTOP_HELPER_LOG"])
try:
    invocation = len(log_path.read_text().splitlines()) + 1
except FileNotFoundError:
    invocation = 1
with log_path.open("a") as log:
    log.write(str(os.getpid()) + "\n")
    log.flush()
signal.signal(signal.SIGTERM, signal.SIG_IGN)
fd = os.open("/dev/null", os.O_RDONLY)
os.dup2(fd, 0)
os.close(fd)
fd = os.open("/dev/null", os.O_WRONLY)
os.dup2(fd, 2)
os.close(fd)

HEADER = struct.Struct("<4sHHIIQQIIII")
DEVICE = struct.Struct("<IIQQQQQQQQQIIII32s80s128s")
PROCESS = struct.Struct("<IIIIQQII")
GIB = 1024 ** 3

def header(sequence, frame_size=48, version=1, status=0, detail=0, devices=0, processes=0):
    return HEADER.pack(b"BTGP", version, 48, frame_size, status, sequence,
                       time.monotonic_ns(), devices, processes, 0, detail)

def fixed(value, size):
    return value.encode()[:size - 1].ljust(size, b"\0")

def device(index, valid, util):
    return DEVICE.pack(index, 0, valid, 128 * GIB, 30 * GIB, 0, 0, 0, 0, 0, 0,
                       util, 0, 0, 0, fixed("00000000:01:%02x.0" % index, 32),
                       fixed("fixture-uuid-%d" % index, 80),
                       fixed("NVIDIA Blackwell %d" % index, 128))

def process(index, pid, valid, vram, util, kind):
    return PROCESS.pack(index, pid, kind, 0, valid, vram, util, 0)

def frame(sequence, records=(), processes=(), status=0, detail=0):
    body = b"".join(records) + b"".join(processes)
    return header(sequence, 48 + len(body), status=status, detail=detail,
                  devices=len(records), processes=len(processes)) + body

def fixture(sequence):
    memory = (1 << 2) | (1 << 3)
    records = [device(i, memory | (1 if i != 1 else 0), (i + 1) * 20) for i in range(4)]
    processes = [
        process(0, os.getppid(), 1 | 2, 0, 97, 1),
        process(0, 424242, 1 | 2, 6 * GIB, 55, 1),
        process(1, 424242, 1 | 2, 7 * GIB, 40, 1),
        process(2, 424243, 2, 0, 83, 2),
        process(3, 424244, 1, GIB, 0, 1),
    ]
    return frame(sequence, records, processes)

def write_partial(data):
    for offset in range(0, len(data), 37):
        os.write(1, data[offset:offset + 37])
        time.sleep(0.001)

def hold():
    while True:
        time.sleep(1)

if mode == "startup-hang":
    hold()
elif mode == "broken-pipe":
    os.close(1)
    hold()
elif mode == "malformed-version":
    write_partial(header(1, version=2))
    hold()
elif mode == "oversized-length":
    write_partial(header(1, frame_size=1024 * 1024 + 1))
    hold()
elif mode == "oversized-count":
    write_partial(header(1, devices=65))
    hold()
elif mode == "unavailable":
    write_partial(frame(1, status=1, detail=999))
elif mode == "fast":
    payload = b"".join(frame(i) for i in range(1, 65))
    while payload:
        payload = payload[os.write(1, payload):]
    hold()
elif mode == "fixture":
    sequence = 1
    while True:
        write_partial(fixture(sequence))
        sequence += 1
        time.sleep(0.35)
elif mode == "recover" and invocation > 1:
    sequence = 1
    while True:
        write_partial(fixture(sequence))
        sequence += 1
        time.sleep(0.35)
else:
    write_partial(frame(1))
    hold()
'''


def drain(master, output, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            try:
                output.extend(os.read(master, 65536))
            except OSError:
                return


def screen_rows(output):
    matches = list(CURSOR_ROW.finditer(output))
    rows = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(output)
        text = CSI.sub(b"", output[match.end():end])
        rows[int(match.group(1))] = text.decode("utf-8", "replace").rstrip()
    return rows


def helper_pids(log):
    return [int(value) for value in log.read_text().splitlines()] if log.exists() else []


def assert_reaped(log):
    pids = helper_pids(log)
    survivors = []
    for pid in pids:
        proc = Path(f"/proc/{pid}")
        if proc.exists():
            try:
                state = next(line for line in (proc / "status").read_text().splitlines()
                             if line.startswith("State:"))
                command = (proc / "cmdline").read_bytes()
            except (FileNotFoundError, StopIteration):
                continue
            survivors.append((pid, state, command))
    assert not survivors, f"helper child survived: {survivors}"


def run(binary, args, env, duration, predicate=None, terminate_signal=None,
        size=(30, 100), resizes=(), keys_after_ready=()):
    master, slave = pty.openpty()
    rows, columns = size
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    before = termios.tcgetattr(slave)
    process = subprocess.Popen([str(binary), *args], stdin=slave, stdout=slave, stderr=slave, env=env)
    output = bytearray()
    start = time.monotonic()
    deadline = start + duration
    resize_index = 0
    try:
        while time.monotonic() < deadline and process.poll() is None:
            elapsed = time.monotonic() - start
            while resize_index < len(resizes) and elapsed >= resizes[resize_index][0]:
                _, rows, columns = resizes[resize_index]
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
                process.send_signal(signal.SIGWINCH)
                resize_index += 1
            drain(master, output, 0.1)
            if predicate and predicate(output):
                break

        assert process.poll() is None, f"{binary.name} exited before quit: {process.returncode}"
        for keys, delay in keys_after_ready:
            os.write(master, keys)
            drain(master, output, delay)
        if terminate_signal is None:
            os.write(master, b"q")
            expected_code = 0
        else:
            process.send_signal(terminate_signal)
            expected_code = 128 + terminate_signal
        deadline = time.monotonic() + 5
        while process.poll() is None and time.monotonic() < deadline:
            drain(master, output, 0.1)
        if process.poll() is None:
            raise subprocess.TimeoutExpired(process.args, 5)
        code = process.returncode
        after = termios.tcgetattr(slave)
        pendin = getattr(termios, "PENDIN", 0)
        assert code == expected_code, f"{binary.name} exited with {code}, expected {expected_code}"
        assert before[:3] == after[:3] and before[4:] == after[4:], "terminal attributes changed"
        assert before[3] & ~pendin == after[3] & ~pendin, "terminal local flags changed"
        assert b"\x00" not in output, "terminal output contains NUL bytes"
        for sequence in RESTORE_SEQUENCES:
            assert sequence in output, f"terminal restore sequence missing: {sequence!r}"
        return output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def suspend_resume(binary, env, log):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    before = termios.tcgetattr(slave)
    process = subprocess.Popen([str(binary), "--update", "100"], stdin=slave,
                               stdout=slave, stderr=slave, env=env)
    output = bytearray()
    try:
        deadline = time.monotonic() + 5
        while b"\x1b[?1049h" not in output and time.monotonic() < deadline:
            drain(master, output, 0.1)
        assert b"\x1b[?1049h" in output, "terminal did not initialize"

        process.send_signal(signal.SIGTSTP)
        deadline = time.monotonic() + 5
        stopped = False
        while time.monotonic() < deadline and process.poll() is None:
            drain(master, output, 0.1)
            try:
                state = next(line.split()[1] for line in Path(f"/proc/{process.pid}/status").read_text().splitlines()
                             if line.startswith("State:"))
            except (FileNotFoundError, StopIteration):
                state = ""
            if state in ("T", "t"):
                stopped = True
                break
        assert stopped, "SIGTSTP did not stop the process after restoring the terminal"
        drain(master, output, 0.2)
        for sequence in RESTORE_SEQUENCES:
            assert sequence in output, f"suspend restore sequence missing: {sequence!r}"

        process.send_signal(signal.SIGCONT)
        deadline = time.monotonic() + 5
        while output.count(b"\x1b[?1049h") < 2 and time.monotonic() < deadline:
            drain(master, output, 0.1)
        assert output.count(b"\x1b[?1049h") >= 2, "terminal was not reinitialized after SIGCONT"
        os.write(master, b"q")
        assert process.wait(timeout=5) == 0
        drain(master, output, 0.1)
        after = termios.tcgetattr(slave)
        pendin = getattr(termios, "PENDIN", 0)
        assert before[:3] == after[:3] and before[4:] == after[4:], "suspend/resume changed terminal attributes"
        assert before[3] & ~pendin == after[3] & ~pendin, "suspend/resume changed terminal local flags"
        for sequence in RESTORE_SEQUENCES:
            assert sequence in output, f"exit restore sequence missing: {sequence!r}"
        assert_reaped(log)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def controlled_abort(binary, env, log):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    before = termios.tcgetattr(slave)

    def disable_core_dump():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    process = subprocess.Popen([str(binary), "--update", "100"], stdin=slave,
                               stdout=slave, stderr=slave, env=env, preexec_fn=disable_core_dump)
    output = bytearray()
    try:
        deadline = time.monotonic() + 5
        while b"\x1b[?1049h" not in output and time.monotonic() < deadline:
            drain(master, output, 0.1)
        assert b"\x1b[?1049h" in output, "terminal did not initialize before abort"
        process.send_signal(signal.SIGABRT)
        assert process.wait(timeout=5) == -signal.SIGABRT, "fatal signal disposition was not preserved"
        drain(master, output, 0.1)
        after = termios.tcgetattr(slave)
        pendin = getattr(termios, "PENDIN", 0)
        assert before[:3] == after[:3] and before[4:] == after[4:], "controlled abort changed terminal attributes"
        assert before[3] & ~pendin == after[3] & ~pendin, "controlled abort changed terminal local flags"
        for sequence in RESTORE_SEQUENCES:
            assert sequence in output, f"abort restore sequence missing: {sequence!r}"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        for pid in helper_pids(log):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        os.close(master)
        os.close(slave)


def env_for(base, root, name, mode=None):
    env = base.copy()
    env.update(
        XDG_CONFIG_HOME=str(root / (name + "-config")),
        XDG_STATE_HOME=str(root / (name + "-state")),
        BETTERTOP_HELPER_LOG=str(root / (name + "-helper-pids")),
    )
    if mode:
        env["BETTERTOP_HELPER_MODE"] = mode
    return env


def fixture_visible(output):
    rows = screen_rows(output)
    return all("Blackwell " + str(index) in rows.get(6 + index, "") for index in range(4)) \
        and "424242" in rows.get(13, "") and "120G" in rows.get(29, "")

def classic_graphs_visible(output):
    text = CSI.sub(b"", output).decode("utf-8", "replace")
    pixels = any(0x2801 <= ord(char) <= 0x28ff for char in text)
    return pixels and "CPU" in text \
        and all(f"GPU{index}" in text for index in range(4))

def classic_gpu_process_visible(output):
    text = CSI.sub(b"", output).decode("utf-8", "replace")
    return "GPU%" in text and "gpu" in text and "97%" in text


def main():
    if not sys.platform.startswith("linux"):
        raise SystemExit("Linux only")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="bettertop-pty-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.update(HOME=str(root / "home"), TERM="xterm-256color")

        fake_dir = root / "fake-bin"
        fake_dir.mkdir()
        fake_ui = fake_dir / "bettertop"
        shutil.copy2(binary, fake_ui)
        fake_helper = fake_dir / "bettertop-gpu"
        fake_helper.write_text(FAKE_HELPER)
        fake_helper.chmod(0o755)

        plain_env = env_for(env, root, "plain")
        for args in (["--help"], ["--version"]):
            result = subprocess.run([str(binary), *args], env=plain_env, capture_output=True, check=True)
            assert b"\x1b" not in result.stdout + result.stderr, f"{args[0]} emitted terminal escapes"
        startup_error = subprocess.run([str(binary), "--force-utf"], env=plain_env, capture_output=True)
        startup_text = startup_error.stdout + startup_error.stderr
        assert startup_error.returncode == 1 and b"interactive shell" in startup_text
        assert b"\x1b" not in startup_text
        print("Noninteractive checks passed: help, version, startup error have no escapes")

        fixture_env = env_for(env, root, "fixture", "fixture")
        output = run(fake_ui, ["--update", "100", "--fun=cat"], fixture_env, 6,
                     lambda captured: fixture_visible(captured))
        rows = screen_rows(output)
        process_rows = [row for row in rows.values() if "424242" in row]
        assert len(process_rows) == 1, f"multi-GPU PID was duplicated: {process_rows}"
        assert all(value in process_rows[0] for value in ("13G", "0,1", "55%")), process_rows[0]
        pid_without_memory = next(row for row in rows.values() if "424243" in row)
        assert "83%" in pid_without_memory and "--" in pid_without_memory
        pid_without_util = next(row for row in rows.values() if "424244" in row)
        assert re.search(r"(?:^|\s)1(?:\.0)?G(?:\s|$)", pid_without_util) and "--" in pid_without_util, pid_without_util
        gpu_without_util = rows[7]
        assert "--" in gpu_without_util, gpu_without_util
        assert b"CPU " in output, "host metrics disappeared with the fixture"
        print("Four-GPU fixture passed: partial frames, 120 GiB fleet use, PID aggregation, unknown fields")

        classic_env = env_for(env, root, "classic-graphs", "fixture")
        classic_output = run(fake_ui, ["--classic", "--update", "100"], classic_env, 6,
                             classic_graphs_visible, size=(30, 180),
                             keys_after_ready=((b"\x1b[C", 1),))
        assert classic_graphs_visible(classic_output), "classic CPU/GPU pixel graphs were not rendered"
        assert classic_gpu_process_visible(classic_output), "GPU-ranked process data was not rendered in classic view"
        assert_reaped(root / "classic-graphs-helper-pids")
        print("Classic view passed: CPU/GPU pixel panels and GPU-ranked process utilization rendered")

        diagnostic_env = env_for(env, root, "diagnostic", "fixture")
        diagnostic = subprocess.run([str(fake_ui), "--diagnose-gpu"], env=diagnostic_env,
                                    capture_output=True, timeout=10)
        assert diagnostic.returncode == 0, diagnostic.stdout + diagnostic.stderr
        assert b"\x1b" not in diagnostic.stdout + diagnostic.stderr
        assert b"Inventory: 4 NVIDIA GPU(s)" in diagnostic.stdout
        assert_reaped(root / "diagnostic-helper-pids")
        unavailable_env = env_for(env, root, "missing-driver", "unavailable")
        unavailable = subprocess.run([str(fake_ui), "--diagnose-gpu"], env=unavailable_env,
                                     capture_output=True, timeout=10)
        assert unavailable.returncode == 1 and b"No NVIDIA sample" in unavailable.stdout
        assert b"\x1b" not in unavailable.stdout + unavailable.stderr
        assert_reaped(root / "missing-driver-helper-pids")
        print("Diagnostic passed: supervised inventory and missing-driver report without a TTY")

        resume_env = env_for(env, root, "suspend-resume", "fixture")
        suspend_resume(fake_ui, resume_env, root / "suspend-resume-helper-pids")
        print("PTY suspend/resume passed: terminal restored while stopped and reinitialized after SIGCONT")

        abort_env = env_for(env, root, "controlled-abort", "fixture")
        controlled_abort(fake_ui, abort_env, root / "controlled-abort-helper-pids")
        print("PTY abort passed: fatal signal restored terminal and preserved signal disposition")

        for sig, name in ((signal.SIGINT, "SIGINT"), (signal.SIGTERM, "SIGTERM"),
                          (signal.SIGHUP, "SIGHUP")):
            output = run(binary, ["--classic"], env_for(env, root, name), 1,
                         terminate_signal=sig)
            print(f"PTY signal passed: {name}, terminal restored")

        startup_log = root / "startup-hang-helper-pids"
        startup_env = env_for(env, root, "startup-hang", "startup-hang")

        def startup_restarted(captured):
            pids = helper_pids(startup_log)
            if len(pids) >= 2:
                assert not Path(f"/proc/{pids[0]}").exists(), "replacement spawned before old helper was reaped"
            return len(pids) >= 2

        output = run(fake_ui, ["--update", "100"], startup_env, 8,
                     startup_restarted, keys_after_ready=((b"g", 0.4),))
        assert b"CPU " in output and output.count(b"CPU ") >= 3, "host display stopped during startup hang"
        assert b"Processes [all]" in output, "input did not work during startup hang"
        startup_pids = helper_pids(startup_log)
        assert len(startup_pids) >= 2, "startup hang helper was not replaced"
        assert len(startup_pids) <= 3, "startup hang caused a helper process storm"
        assert_reaped(startup_log)
        print("Startup hang passed: host and input stayed live; blocked helper was reaped before retry")

        runtime_log = root / "runtime-hang-helper-pids"
        runtime_env = env_for(env, root, "runtime-hang", "sample-stall")

        def runtime_restarted(captured):
            pids = helper_pids(runtime_log)
            if len(pids) >= 2:
                assert not Path(f"/proc/{pids[0]}").exists(), "replacement spawned before old helper was reaped"
            return b"stale" in captured and len(pids) >= 2

        output = run(fake_ui, ["--update", "100"], runtime_env, 8,
                     runtime_restarted,
                     keys_after_ready=((b"g", 0.3), (b"\x1b[B", 0.3),
                                       (b"f", 0.1), (b"p", 0.1), (b"y", 0.1),
                                       (b"t", 0.1), (b"h", 0.1), (b"o", 0.1),
                                       (b"n", 0.1), (b"\n", 0.5)))
        assert b"CPU " in output and output.count(b"CPU ") >= 3, "host metrics stopped during runtime hang"
        assert b"stale" in output and b"Processes [all]" in output and b"f python" in output
        runtime_pids = helper_pids(runtime_log)
        assert len(runtime_pids) >= 2, "runtime hang helper was not replaced"
        assert len(runtime_pids) <= 3, "runtime hang caused a helper process storm"
        assert_reaped(runtime_log)
        print("Runtime hang passed: stale shown, host/input live, child reaped before recovery")

        recovery_log = root / "recovery-helper-pids"
        recovery_env = env_for(env, root, "recovery", "recover")

        def recovered(captured):
            rows = screen_rows(captured)
            return b"stale" in captured and len(helper_pids(recovery_log)) >= 2 \
                and fixture_visible(captured) and "healthy" in rows.get(30, "")

        output = run(fake_ui, ["--update", "100", "--fun=cat"], recovery_env, 10, recovered)
        rows = screen_rows(output)
        pids = helper_pids(recovery_log)
        assert b"stale" in output and "healthy" in rows.get(30, ""), \
            f"fresh GPU sample did not clear stale state: footer={rows.get(30)!r}, helpers={pids}, " \
            f"fixture_visible={fixture_visible(output)}"
        assert len(pids) >= 2, "stale helper was not replaced"
        assert len(pids) <= 3, "recovery caused a helper process storm"
        assert_reaped(recovery_log)
        print("Recovery passed: a fresh four-GPU sample cleared stale state")

        for mode in ("malformed-version", "oversized-length", "oversized-count", "broken-pipe"):
            scenario_env = env_for(env, root, mode, mode)
            output = run(fake_ui, ["--update", "100"], scenario_env, 1.5)
            assert b"CPU " in output, f"host monitor failed for {mode}"
            assert_reaped(root / (mode + "-helper-pids"))
            print(f"Transport check passed: {mode}")

        fast_env = env_for(env, root, "fast-output", "fast")
        output = run(fake_ui, ["--update", "100"], fast_env, 2)
        assert output.count(b"CPU ") >= 3, "fast helper output starved host rendering"
        assert_reaped(root / "fast-output-helper-pids")
        print("Transport check passed: fast frame producer did not starve host rendering")

        resize_env = env_for(env, root, "resize", "fixture")
        output = run(fake_ui, ["--update", "100"], resize_env, 4,
                     resizes=((0.8, 10, 40), (1.6, 24, 80), (2.4, 30, 100)))
        assert b"Compact terminal" in output, "40x10 compact warning was not rendered"
        assert all(f"Blackwell {i}" in row for i, row in enumerate(
            screen_rows(output).get(row_number, "") for row_number in range(6, 10)))
        assert_reaped(root / "resize-helper-pids")
        print("Resize check passed: 40x10, 80x24 and 100x30 with four GPUs")


if __name__ == "__main__":
    main()
