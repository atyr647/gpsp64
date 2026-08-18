#!/bin/bash
# Bisect auto-generated AOT correctness using the benchmark harness.
#
# Pass/fail signal: a healthy run ends parked in the game's idle loop
# (PC 0x080008ce) with ~38k interpreted insns/frame.  A run whose AOT
# corrupts state derails into executing data (PC 0x080048f0) and the
# instruction count jumps.  Both are exactly reproducible.
#
# Handwritten AOT is disabled throughout so only auto-generated code is
# under test.
set -u
cd "$(dirname "$0")/.."

export BENCH_FRAMES="${BENCH_FRAMES:-600}"
export BENCH_WARMUP="${BENCH_WARMUP:-1200}"

HEALTHY_PC="0x080008ce"

run_cfg() {
  local label="$1"; shift
  ./native/bench.sh "$label" -DAOT_NO_HANDWRITTEN "$@" >/tmp/bx_$label.log 2>&1
  local pc insns
  pc=$(grep -m1 "final PC" /tmp/bx_$label.log | awk '{print $3}')
  insns=$(grep -m1 "interpreted insns" /tmp/bx_$label.log | awk '{print $3}')
  if [ "$pc" = "$HEALTHY_PC" ]; then
    printf "  %-14s OK      PC=%s  %s insns/frame\n" "$label" "$pc" "$insns"
  else
    printf "  %-14s BROKEN  PC=%s  %s insns/frame\n" "$label" "${pc:-?}" "${insns:-?}"
  fi
}

echo "reference points:"
run_cfg noaot_ref -DAOT_DISABLE
run_cfg full_auto

echo
echo "additive bisect (LO base < 0x080068A0, plus one group at a time):"
run_cfg add_base -DAOT_ADDITIVE
for g in $(seq 1 13); do
  run_cfg "add_g$g" -DAOT_ADDITIVE -DAOT_ADD_GROUP=$g
done
