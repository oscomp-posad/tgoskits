#!/usr/bin/env python3
"""sdboot-run.py — fast, reliable StarryOS board run via SD-card local-load.

Replaces ostool's serial ymodem upload (~90 s at 1.5 Mbaud + corruption-prone)
on the OrangePi-5-Plus. That board's U-Boot has NO working Ethernet
(`net list` is empty → TFTP is impossible), but three things DO work and let us
sidestep the serial transfer entirely:

  * Linux on the board has fast Ethernet (scp over the link-local en5),
  * U-Boot can read the ext4 rootfs (`ls mmc 1:2 /` works),
  * the board ships `mkimage` (u-boot-tools).

So the deploy is:
  1. build the kernel on the host,
  2. scp starryos.bin + dtb + starryos.its to /home/orangepi (writable, sudo-free;
     orangepi is NOT allowed to write the FAT /boot, and there is no mtools),
  3. build the FIT on the board with mkimage,
  4. warm-reboot, catch U-Boot over serial, `load mmc 1:2 ...; bootm`,
  5. type the benchmark init cmd at the StarryOS shell, capture the console to the
     success sentinel,
  6. `reboot -f` back to Linux (plain `reboot` is a no-op in StarryOS on this
     board and strands it at the shell — this is what made the old ymodem loop
     hang between runs).

Load addresses match the ostool-made FIT: kernel 0x02000000, fdt 0x12000000,
FIT staged at 0x0a000000.

Usage:
  scripts/board/sdboot-run.py \
      --config apps/starry/sysbench-board/build-aarch64-placement-orangepi-5-plus.toml \
      --uboot-config apps/starry/sysbench-board/uboot-schedbench-short.toml \
      --out /tmp/sdboot.log
"""
import argparse
import os
import re
import subprocess
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial required: pip3 install pyserial")

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DTB = os.path.join(REPO, "os/StarryOS/configs/board/orangepi-5-plus.dtb")
ITS = os.path.join(REPO, "apps/starry/sysbench-board/starryos.its")
BIN = os.path.join(REPO, "target/aarch64-unknown-linux-musl/release/starryos.bin")

# Board/host wiring (this bench rig). Override via flags if the topology changes.
SERIAL = "/dev/cu.usbserial-AQ03MLX2"
BAUD = 1500000
HOST_BIND = "169.254.99.39"   # host en5 (link-local, board-facing)
BOARD = "orangepi@169.254.50.2"
STAGE = "/home/orangepi"      # writable ext4 dir (sudo-free), = U-Boot mmc 1:2
FIT_ADDR = "0x0a000000"

SSH = ["ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no", "-b", HOST_BIND]
SCP = ["scp", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no", "-o", f"BindAddress={HOST_BIND}"]


def run(cmd, **kw):
    print("+", " ".join(cmd) if isinstance(cmd, list) else cmd, flush=True)
    return subprocess.run(cmd, check=True, **kw)


def parse_uboot_cfg(path):
    """Pull the few fields we need out of the ostool uboot toml (regex, no toml dep)."""
    txt = open(path).read()

    def scalar(key, default=None):
        m = re.search(rf'^{key}\s*=\s*"((?:[^"\\]|\\.)*)"', txt, re.M)
        return m.group(1).encode().decode("unicode_escape") if m else default

    def strlist(key):
        m = re.search(rf"^{key}\s*=\s*\[(.*?)\]", txt, re.M | re.S)
        if not m:
            return []
        return [s.encode().decode("unicode_escape")
                for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))]

    return {
        "shell_prefix": scalar("shell_prefix", "root@starry:/root #"),
        "shell_init_cmd": scalar("shell_init_cmd", ""),
        "success": strlist("success_regex"),
        "fail": strlist("fail_regex"),
        "timeout": int(re.search(r"^timeout\s*=\s*(\d+)", txt, re.M).group(1))
        if re.search(r"^timeout\s*=\s*(\d+)", txt, re.M) else 1800,
    }


def build(config):
    env = dict(os.environ)
    env.pop("RUSTUP_TOOLCHAIN", None)
    run(["cargo", "xtask", "starry", "build", "--config", config], cwd=REPO, env=env)
    if not os.path.exists(BIN):
        sys.exit(f"build produced no {BIN}")


BENCH_DIR = os.path.join(REPO, "apps/starry/sysbench-board")


def stage():
    """scp bin+dtb+its (+ the bench scripts) to the board's ext4 home, then build
    the FIT there. The board runs the repo copies of run-schedbench.sh /
    sched-bench.sh, so the benchmark stays the source of truth."""
    files = [BIN, DTB, ITS]
    for name in ("run-schedbench.sh", "sched-bench.sh"):
        p = os.path.join(BENCH_DIR, name)
        if os.path.exists(p):
            files.append(p)
    run(SCP + files + [f"{BOARD}:{STAGE}/"])
    # regenerate FIT on the board (mkimage lives there); board reads /home over ext4
    run(SSH + [BOARD, f"cd {STAGE} && mkimage -f starryos.its starryos.fit >/dev/null && "
                      f"ls -l starryos.fit"])


def reboot_linux_to_uboot():
    print("+ warm-reboot board (ssh) to hand off to U-Boot", flush=True)
    subprocess.run(SSH + [BOARD, "reboot"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_regex(s, patterns, timeout, echo=True, sink=None):
    """Read serial until any regex in `patterns` matches the accumulated tail; returns (idx, buf)."""
    compiled = [re.compile(p) for p in patterns]
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        chunk = s.read(4096)
        if chunk:
            txt = chunk.decode("utf-8", "replace")
            if echo:
                sys.stdout.write(txt)
                sys.stdout.flush()
            if sink:
                sink.write(txt)
            buf += txt
            for i, c in enumerate(compiled):
                if c.search(buf[-4000:]):
                    return i, buf
    return -1, buf


def catch_uboot(s, timeout=45):
    """Spam a key to stop autoboot; return when at the `=>` prompt."""
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        s.write(b" ")
        time.sleep(0.08)
        d = s.read(400)
        if d:
            buf += d
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
        if b"=>" in buf[-30:]:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True, help="starry build config toml")
    ap.add_argument("--uboot-config", required=True, help="ostool uboot toml (for init cmd + sentinels)")
    ap.add_argument("--out", default="/tmp/sdboot-run.log")
    ap.add_argument("--no-build", action="store_true", help="reuse existing starryos.bin")
    args = ap.parse_args()

    cfg = parse_uboot_cfg(args.uboot_config)
    for f in (DTB, ITS):
        if not os.path.exists(f):
            sys.exit(f"missing {f}")

    if not args.no_build:
        build(args.config)
    stage()
    reboot_linux_to_uboot()

    s = serial.Serial(SERIAL, BAUD, timeout=0.3)
    sink = open(args.out, "w")
    try:
        if not catch_uboot(s):
            sys.exit("FAILED to catch U-Boot prompt")
        # local-load the freshly-staged FIT from the ext4 rootfs and boot it
        s.write(f"load mmc 1:2 {FIT_ADDR} {STAGE}/starryos.fit\r\n".encode())
        time.sleep(2)
        sys.stdout.write(s.read(4000).decode("utf-8", "replace"))
        s.write(f"bootm {FIT_ADDR}\r\n".encode())

        # wait for the StarryOS shell, then drive the benchmark
        prompt = re.escape(cfg["shell_prefix"])
        idx, _ = wait_regex(s, [prompt], timeout=120, sink=sink)
        if idx < 0:
            sys.exit("FAILED: StarryOS shell prompt not seen after bootm")
        time.sleep(1)
        s.write((cfg["shell_init_cmd"] + "\r\n").encode())

        pats = cfg["success"] + cfg["fail"]
        idx, _ = wait_regex(s, pats, timeout=cfg["timeout"], sink=sink)
        ok = 0 <= idx < len(cfg["success"])
        print(f"\n=== {'SUCCESS' if ok else 'FAIL/timeout'} (match idx={idx}) ===", flush=True)

        # reliably return the board to Linux (plain `reboot` is a no-op here)
        time.sleep(1)
        s.write(b"reboot -f\r\n")
        time.sleep(2)
        sink.close()
        sys.exit(0 if ok else 1)
    finally:
        s.close()


if __name__ == "__main__":
    main()
