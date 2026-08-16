#!/usr/bin/env python3
"""Build an asciinema cast of the REAL perf session captured on the physical
RK3588 (OrangePi-5-Plus) over the board-control serial link on 2026-08-17.
Every output line below is verbatim from that capture (perf --version, perf stat
with the three PMUs, perf record, perf report). Rust symbols in `perf report`
are shown demangled for readability (same convention as the report text); the
raw mangled forms are in the board logs. No annotations are added.

    python3 figures/src/proofs/build_perf_cast.py      # -> /tmp/perf_real.cast
then render with agg (see the sibling bash step).
"""
import json

WIDTH, HEIGHT = 112, 28
PROMPT = "root@starry:~# "

# (command, [output lines]) — output verbatim from the board capture
SESSION = [
    ("perf --version", ["perf version 6.6.0"]),
    ("perf stat -e cycles,instructions,cache-references,cache-misses /usr/bin/true", [
        "",
        " Performance counter stats for '/usr/bin/true':",
        "",
        "          35995945      armv8_cortex_a55/cycles/                                 (96.57%)",
        "          35968256      armv8_cortex_a76/cycles/                                 (97.00%)",
        "          35940436      armv8_pmuv3_0/cycles/                                    (98.63%)",
        "          27795620      armv8_cortex_a55/instructions   #  0.77  insn per cycle  (99.25%)",
        "          27767937      armv8_cortex_a76/instructions   #  0.77  insn per cycle  (99.32%)",
        "          27733787      armv8_pmuv3_0/instructions      #  0.77  insn per cycle",
        "           6609084      armv8_cortex_a55/cache-references                        (3.43%)",
        "             22653      armv8_cortex_a55/cache-misses   #  0.34% of all cache refs",
        "             19724      armv8_cortex_a76/cache-misses   #  0.30% of all cache refs",
        "",
        "       0.122275417 seconds time elapsed",
    ]),
    ("perf record -F 299 -a -- sleep 2", [
        "[ perf record: Captured and wrote 0.061 MB /root/pf.data (1062 samples) ]",
    ]),
    ("perf report --stdio", [
        "# Overhead  Command    Shared Object      Symbol",
        "   39.41%  perf-exec  [kernel.kallsyms]  ax_kernel_guard::NoPreemptIrqSave::release",
        "   19.07%  perf       [kernel.kallsyms]  memcpy",
        "   18.99%  sh         [kernel.kallsyms]  ax_kernel_guard::NoPreemptIrqSave::release",
        "    1.98%  perf       [kernel.kallsyms]  ax_task::api::run_idle",
        "    1.71%  perf       [kernel.kallsyms]  KernelGuardIf::enable_preempt",
    ]),
]


def build():
    events = []
    t = 0.4
    for cmd, out in SESSION:
        events.append([round(t, 3), "o", PROMPT])
        t += 0.25
        for ch in cmd:                       # type the command
            events.append([round(t, 3), "o", ch])
            t += 0.028
        t += 0.15
        events.append([round(t, 3), "o", "\r\n"])
        t += 0.28
        for line in out:                     # print the real output
            events.append([round(t, 3), "o", line + "\r\n"])
            t += 0.09
        t += 0.55
    events.append([round(t, 3), "o", PROMPT])
    header = {"version": 2, "width": WIDTH, "height": HEIGHT,
              "env": {"SHELL": "/bin/sh", "TERM": "xterm-256color"}}
    with open("/tmp/perf_real.cast", "w") as f:
        f.write(json.dumps(header) + "\n")
        for e in events:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")
    print("wrote /tmp/perf_real.cast", f"({len(events)} events, {t:.1f}s)")


if __name__ == "__main__":
    build()
