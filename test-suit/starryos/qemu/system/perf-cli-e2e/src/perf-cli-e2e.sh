#!/bin/sh
# End-to-end: the REAL `perf` CLI recording + decoding a kprobe on StarryOS.
#
# The probe is created through the kernel's own tracefs `kprobe_events`
# (`echo p:probe/hsc _stext+<off>`), NOT `perf probe`: `perf probe` (perf's
# symbol->kprobe_events resolver) requires libelf, and the CI perf is built
# NO_LIBELF (static libelf/elfutils for musl-cross is a separate rabbit hole).
# The kprobe_events write path is already covered by perf-cli-kprobe; what THIS
# test adds is the real perf CLI's record/report/script over that probe:
#   echo p:probe/hsc _stext+<off> > kprobe_events   (kernel makes the tracepoint)
#   perf record -e probe:hsc  (perf parses events/probe/hsc/format via
#                              libtraceevent, opens PERF_TYPE_TRACEPOINT; the
#                              kernel routes it to the kprobe -> PERF_SAMPLE_RAW)
#   perf report / perf script (perf reloads perf.data and decodes each raw
#                              record via the embedded tracing-data --
#                              header_page/header_event + the event format --
#                              resolving it by common_type == the tracefs id)
#
# This is exactly the libtraceevent-dependent path the kernel's tracing-data
# metadata (header_page/header_event/ftrace:print) + common_type stamping exist
# to support. Target: `handle_syscall` (fires once per syscall -- a moderate
# rate, NOT on the sample-emit path; `memcpy` would overwhelm the recording).
# A clean run ends with STARRY_PERF_CLI_E2E_OK.
PERF=/usr/bin/perf
TDIR=/sys/kernel/debug/tracing
KE=$TDIR/kprobe_events
echo "STARRY_PERF_CLI_E2E_BEGIN"
"$PERF" --version 2>&1 | sed 's/^/[ver] /'

# Resolve _stext and handle_syscall from kallsyms; the kernel accepts a
# SYMBOL+offset probe spec, and perf without a vmlinux writes probes relative to
# _stext, so _stext+(handle_syscall-_stext) lands exactly on the target.
STEXT=$(awk '$3=="_stext"{print $1; exit}' /proc/kallsyms)
HSC=$(awk '/handle_syscall/{print $1; exit}' /proc/kallsyms)
echo "[sym] _stext=${STEXT:-<none>} handle_syscall=${HSC:-<none>}"
if [ -z "$STEXT" ] || [ -z "$HSC" ]; then
  echo "STARRY_PERF_CLI_E2E_FAIL: _stext/handle_syscall not in /proc/kallsyms"
  exit 1
fi
OFF=$(( 0x$HSC - 0x$STEXT ))
echo "[off] _stext+$OFF"

# Create the kprobe through tracefs (the classic perf-probe-file protocol).
echo "p:probe/hsc _stext+$OFF" > "$KE" 2>/tmp/pke.out || {
  sed 's/^/[ke-write] /' /tmp/pke.out
  echo "STARRY_PERF_CLI_E2E_FAIL: could not write kprobe_events"
  exit 1
}
echo "[kprobe_events]"; cat "$KE" 2>&1 | sed 's/^/[ke] /'
if [ ! -f "$TDIR/events/probe/hsc/id" ]; then
  echo "STARRY_PERF_CLI_E2E_FAIL: events/probe/hsc/id absent (dynamic tracepoint not created)"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi
echo "[id] $(cat "$TDIR/events/probe/hsc/id" 2>&1)"

# perf record: open probe:hsc (perf parses its format via libtraceevent) while a
# syscall-heavy command runs.
"$PERF" record -e probe:hsc -o /tmp/pd -- ls -la / >/tmp/prec.out 2>&1
rc=$?
sed 's/^/[record] /' /tmp/prec.out
N=$(sed -n 's/.*(\([0-9][0-9]*\) samples).*/\1/p' /tmp/prec.out | head -1)
echo "[samples] ${N:-0}"
if [ "$rc" -ne 0 ]; then
  echo "STARRY_PERF_CLI_E2E_FAIL: perf record rc=$rc"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi
if [ -z "$N" ] || [ "$N" -le 0 ]; then
  echo "STARRY_PERF_CLI_E2E_FAIL: perf record captured 0 samples"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi

# perf report: prove the recording is a usable profile -- perf must reload
# perf.data, decode each PERF_SAMPLE_RAW record via the embedded tracing-data
# (header_page/header_event + the event format), and print an overhead row for
# the probe event. libtraceevent resolves the record by its common_type, which
# must equal the tracefs event id.
"$PERF" report --stdio -i /tmp/pd --kallsyms=/proc/kallsyms >/tmp/prep.out 2>&1
rrc=$?
sed 's/^/[report] /' /tmp/prep.out | head -20
if [ "$rrc" -ne 0 ]; then
  echo "STARRY_PERF_CLI_E2E_FAIL: perf report rc=$rrc"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi
if ! grep -q "probe:hsc" /tmp/prep.out; then
  echo "STARRY_PERF_CLI_E2E_FAIL: perf report missing the probe:hsc event"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi
if ! grep -qE '[0-9]+\.[0-9]+%' /tmp/prep.out; then
  echo "STARRY_PERF_CLI_E2E_FAIL: perf report produced no overhead rows"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi
echo "[report] OK: probe:hsc profile with overhead rows"

# perf script must also format each sample without crashing.
"$PERF" script -i /tmp/pd >/tmp/pscript.out 2>&1
src=$?
echo "[script] rc=$src ($(wc -l </tmp/pscript.out 2>/dev/null) lines)"
if [ "$src" -ne 0 ]; then
  echo "STARRY_PERF_CLI_E2E_FAIL: perf script rc=$src"
  echo "-:probe/hsc" > "$KE" 2>/dev/null
  exit 1
fi

# Remove the probe.
echo "-:probe/hsc" > "$KE" 2>/dev/null
echo "STARRY_PERF_CLI_E2E_OK"
exit 0
