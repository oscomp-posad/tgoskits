#!/bin/sh
# perf-top-smoke: run `perf top --stdio` briefly against a workload and confirm it
# produces a live profile. This exercises perf top's whole path on StarryOS:
# system-wide (`-a`) per-CPU cycle sampling in frequency mode, side-band
# COMM/MMAP2 symbolization, /proc/kallsyms for kernel symbols, and the
# poll()/mmap-ring drain loop -- everything `perf record` uses plus the live
# reader. perf is built without slang, so `--stdio` (no TUI) is the mode.
#
# perf top runs until interrupted, so this backgrounds it, lets it take a couple
# of refresh intervals against a read()-heavy workload (so the PMU cycle counter
# advances under QEMU-TCG), then SIGINTs it and inspects the captured output.
#
# SUCCESS == perf top printed a live profile (its `PerfTop:` header or an overhead
# table line), i.e. it sampled and symbolized. Prints STARRY_PERF_TOP_OK.
P=/usr/bin/perf
OUT=/tmp/perf-top.out
echo "PERF_TOP_BEGIN"

if [ ! -x "$P" ]; then
    echo "STARRY_PERF_TOP_SKIPPED (no $P)"
    exit 0
fi

# Background read()-heavy workload so there is non-idle, counter-advancing work to
# profile (a pure-shell loop barely moves the TCG cycle counter).
(
    i=0
    while [ "$i" -lt 100000 ]; do
        dd if=/dev/zero of=/dev/null bs=8192 count=128 2>/dev/null
        i=$((i + 1))
    done
) &
WL=$!

# Run perf top in stdio mode, refreshing every 1s, in the background; give it a
# few intervals then interrupt it (perf top exits cleanly on SIGINT).
"$P" top --stdio -d 1 >"$OUT" 2>&1 &
TOP=$!
sleep 8
kill -INT "$TOP" 2>/dev/null
sleep 1
kill -KILL "$TOP" 2>/dev/null
wait "$TOP" 2>/dev/null
kill -KILL "$WL" 2>/dev/null
wait "$WL" 2>/dev/null

# Strip perf top's cursor/clear-screen escape codes so the diagnostic dump does
# not blank the captured console (perf top redraws with ESC[H ESC[2J each refresh).
echo "--- perf top output (tail 25, escapes stripped) ---"
tr -d '\033' <"$OUT" | tail -25
echo "--- perf top output (end) ---"

# A real live profile: an overhead-table row -- a percentage followed by a DSO
# (`N.NN%  /usr/bin/foo` or `N.NN%  [kernel.kallsyms]`). This only appears once
# perf top has sampled AND symbolized, so it proves the whole path (per-CPU `-a`
# sampling + side-band + /proc/kallsyms), unlike the `PerfTop:` header which prints
# even with zero samples.
if grep -Eq "[0-9]+\.[0-9]+% +[/[]" "$OUT" 2>/dev/null; then
    echo "STARRY_PERF_TOP_OK"
    exit 0
fi

echo "perf-top FAILED: no symbolized overhead entries (perf top produced no live profile)"
exit 1
