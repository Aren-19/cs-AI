#!/bin/bash
# Health probe for an unattended run. Prints one line; non-zero if something is wrong.
cd "$(dirname "$0")/.."
OUT="C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Source/cstrike/addons/sourcemod/data/csai/out"

ACT=$(tasklist 2>/dev/null | grep -ci srcds_win64)
ERR=$(wc -c < logs/learner.err.log 2>/dev/null || echo 0)
GEN=$(tail -1 data/train_log.csv 2>/dev/null | cut -d, -f1)
AGE=$(( $(date +%s) - $(stat -c %Y data/train_log.csv 2>/dev/null || echo 0) ))
STALL=$(grep -ac "STALLED" logs/daemon.log 2>/dev/null || echo 0)

ORPH=0
NOW=$(date +%s)
if [ -d "$OUT" ]; then
  for b in "$OUT"/a*_batch_*.bin; do
    [ -e "$b" ] || continue
    case "$b" in *a99_batch_*) continue;; esac
    [ -e "${b%.bin}.done" ] && continue
    [ $(( NOW - $(stat -c %Y "$b") )) -gt 300 ] && ORPH=$((ORPH + 1))
  done
fi

printf "gen %-6s actors %-3s last-update %4ss  stderr %sb  orphans %-3s stalls %s\n" \
       "$GEN" "$ACT" "$AGE" "$ERR" "$ORPH" "$STALL"

BAD=0
[ "$ACT" -lt 1 ] && { echo "  PROBLEM: no actors running"; BAD=1; }
# A generation takes about 15 seconds. 300 is twenty times that, and still low
# enough to catch an outage in minutes rather than the half hour 600 allowed.
[ "$AGE" -gt 300 ] && { echo "  PROBLEM: no new generation for ${AGE}s"; BAD=1; }
[ "$ERR" -gt 0 ] && { echo "  PROBLEM: learner wrote to stderr:"; tail -3 logs/learner.err.log | sed 's/^/    /'; BAD=1; }
[ "$ORPH" -gt 2 ] && { echo "  PROBLEM: $ORPH batch files have been waiting over five minutes for a .done marker - the learner cannot see them"; BAD=1; }
exit $BAD
