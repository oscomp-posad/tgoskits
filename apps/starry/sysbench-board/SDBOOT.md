# Fast board runs via SD-card local-load (`sdboot-run.py`)

Replaces ostool's serial **ymodem** upload (~90 s at 1.5 Mbaud, corruption-prone, and the loop
strands the board between runs) for StarryOS runs on the OrangePi-5-Plus.

## Why not TFTP (the obvious fix)

**This board's U-Boot has no working Ethernet.** Verified at the U-Boot prompt: `net list` is empty
and `ping` returns *"No ethernet found."* — the 2.5 GbE ports are RTL8125 (PCIe) with no U-Boot
driver, and the native GMAC isn't wired up. So `tftp`/`dhcp` boot is impossible on the current
U-Boot, and ostool's `[net]` mode just falls back to `loady` (ymodem) anyway.

## The mechanism that does work

Three facts make a serial-free transfer possible even without U-Boot Ethernet:

- **Linux on the board has fast Ethernet** (scp over link-local `en5`).
- **U-Boot can read the ext4 rootfs** — `ls mmc 1:2 /` works (it can also read the FAT `/boot` =
  `mmc 1:1`).
- **The board ships `mkimage`** (u-boot-tools).

So `sdboot-run.py`:
1. builds the kernel on the host (`cargo xtask starry build --config <build.toml>`),
2. `scp`s `starryos.bin` + `orangepi-5-plus.dtb` + `starryos.its` (+ the bench scripts) to
   `/home/orangepi` — **writable, sudo-free**; the orangepi user cannot write the FAT `/boot`, has
   no `mtools`, and there is no passwordless sudo, so the ext4 home is the deploy target,
3. builds the FIT **on the board** with `mkimage`,
4. warm-reboots, catches U-Boot on serial, `load mmc 1:2 0x0a000000 /home/orangepi/starryos.fit` +
   `bootm` (local load ≈ 1 s at 11 MiB/s vs ~90 s ymodem),
5. types the benchmark init cmd at the StarryOS shell and captures the console to the success
   sentinel (same `success_regex`/`fail_regex` contract as the ostool uboot config),
6. `reboot -f` back to Linux — plain `reboot` is a **no-op** in StarryOS on this board and strands
   it at the shell (this was the biggest board-loop failure mode); `reboot -f` resets the SoC so
   U-Boot autoboots SD Linux.

## Usage

```bash
scripts/board/sdboot-run.py \
  --config apps/starry/sysbench-board/build-aarch64-placement-orangepi-5-plus.toml \
  --uboot-config apps/starry/sysbench-board/uboot-schedbench-short.toml \
  --out /tmp/sdboot.log
# --no-build reuses the current target/.../starryos.bin
```

Host/board wiring (serial device, `169.254.99.39` host bind, `169.254.50.2` board) is defaulted for
this rig at the top of the script.

## Manual recovery (if a run leaves the board stranded)

Board wedged at a U-Boot `loady`/`=>` prompt or the StarryOS shell → over serial
(`/dev/cu.usbserial-AQ03MLX2 @ 1500000`): send `\x18`×10 (abort any ymodem), then `reboot -f` (from
the StarryOS shell) or `boot` (from the U-Boot `=>` prompt) to return to SD Linux. Host `en5` drops
its `169.254.99.39` link-local IP whenever the board's Ethernet link goes down and restores it once
Linux is back up.
