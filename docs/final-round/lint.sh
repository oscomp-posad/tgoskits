#!/usr/bin/env bash
set -u; fail=0
red(){ echo "LINT FAIL: $1"; fail=1; }
grep -rInE 'orangepi/orangepi|sudo |HANDOFF|约三周' report slides assets 2>/dev/null && red "secret/forbidden string"
grep -rInE '[0-9]{1,3}(\.[0-9]{1,3}){3}' report slides assets 2>/dev/null && red "raw IP"
grep -rInE '不是[^。]{1,40}而是|不只[^。]{1,40}而是|不止[^。]{1,40}而是|并非[^。]{1,40}而是|与其[^。]{1,40}不如' report slides assets 2>/dev/null && red "not-A-but-B 对比句式（改为直接陈述）"
for hx in 0D1B2A 2DD4BF F59E0B; do
  grep -q "$hx" style/tokens.tex && grep -qi "$hx" style/style.py || red "palette hex $hx out of sync"
done
n=$(grep -rho '\\abl{' data report slides 2>/dev/null | wc -l | tr -d ' ')
echo "INFO: $n ablation-pending (\\abl) markers"
[ $fail -eq 0 ] && echo "LINT OK" || exit 1
