#!/bin/bash
cd "$(dirname "$0")/.."

STEP=${STEP:-8}                 # ticks added per promotion
MAX=${MAX:-64}                  # never hand over more than this
PROMOTE=${PROMOTE:-90}          # finish rate (%) that earns the next step
COLLAPSE=${COLLAPSE:-55}        # finish rate (%) that triggers a rollback
SLOWER=${SLOWER:-0.40}          # seconds of median run time
NEED=${NEED:-120}               # generations to judge either on
BASE_CKPT=${BASE_CKPT:-data/ckpt_baseline_gen8858.npz}
DARGS_COMMON="-Map surf_demise -Power high -FrameSkip 2 -StateMix 0.3 -StateLo 0.66 -StateHi 0.77 -Entropy 0.01 -EntFinal 0.002 -EntAnneal 3000 -TimeCost 0.08 -FinishBonus 50 -FinishFloor 0.5 -TrimCost 0.05 -SwitchCost 0.40"

gen_now() { tail -1 data/train_log.csv | cut -d, -f1; }

since() {
  awk -F, -v g0="$1" -v trail="${TRAIL:-80}" '
    NR>1 && $18!="" && $1+0>g0 { c++; N[c]=$18; F[c]=$19; M[c]=$21+0; if($20+0>0 && (b==0||$20+0<b)) b=$20+0 }
    END{
      if(!c) { printf "0 0 0 0 0"; exit }
      lo = c - trail + 1; if (lo < 1) lo = 1
      for (i = lo; i <= c; i++) { tn += N[i]; tf += F[i]; if (M[i] > 0) { tm += M[i]; tmc++ } }
      printf "%d %d %.1f %.3f %.3f", c, tn, (tn ? 100*tf/tn : 0), b, (tmc ? tm/tmc : 0)
    }' data/train_log.csv
}
restart() {   # $1 = prelearn ticks
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/daemon.ps1 -Stop > /dev/null 2>&1
  sleep 12
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/daemon.ps1 $DARGS_COMMON -PreLearn "$1" > /dev/null 2>&1 &
  sleep 150
}

PRE=${PRE:-8}
MARK=${MARK:-$(gen_now)}
# The median run time this is allowed to drift from, measured before the first
# step. Without it there is nothing to compare against and slowing down is free.
base_median() {
  awk -F, -v g="$1" 'NR>1 && $21+0>0 && $1+0<=g && $1+0>g-200 {m+=$21; c++}
    END{ if(c) printf "%.3f", m/c; else printf "0" }' data/train_log.csv
}
BASE_MED=${BASE_MED:-$(base_median "$MARK")}
BAD=0
echo "$(date +%H:%M) curriculum starting at PreLearn=$PRE, marker gen $MARK, median to hold near ${BASE_MED}s"

for i in $(seq 1 200); do
  if bash tools/watch.sh > /tmp/w.txt 2>&1; then BAD=0; else
    BAD=$((BAD+1)); echo "$(date +%H:%M) problem ($BAD in a row):"; cat /tmp/w.txt
    if [ "$BAD" -ge 2 ]; then
      echo "$(date +%H:%M) restarting the daemon at PreLearn=$PRE"
      restart "$PRE"; BAD=0
    fi
  fi

  read -r C N RATE BEST MED <<< "$(since "$MARK")"
  if [ "${C:-0}" -ge "$NEED" ]; then
    SLOW=$(awk "BEGIN{ if ($MED > 0 && $BASE_MED > 0 && $MED - $BASE_MED > $SLOWER) print 1; else print 0 }")
    if awk "BEGIN{exit !($RATE < $COLLAPSE)}" || [ "$SLOW" = "1" ]; then
      if [ "$SLOW" = "1" ]; then
        echo "$(date +%H:%M) TOO SLOW at PreLearn=$PRE: median ${MED}s against a baseline of ${BASE_MED}s - restoring and stopping"
      else
        echo "$(date +%H:%M) COLLAPSED at PreLearn=$PRE: $RATE% over $C gens - restoring the baseline and stopping"
      fi
      powershell -NoProfile -ExecutionPolicy Bypass -File tools/daemon.ps1 -Stop > /dev/null 2>&1
      sleep 10; cp "$BASE_CKPT" data/ckpt.npz
      echo "  checkpoint restored; left stopped for a human"
      break
    fi
    if awk "BEGIN{exit !($RATE >= $PROMOTE)}"; then
      if [ "$PRE" -ge "$MAX" ]; then
        echo "$(date +%H:%M) PreLearn=$PRE is the cap; holding. $RATE% over $C gens, fastest ${BEST}s"
      else
        PRE=$((PRE+STEP)); [ "$PRE" -gt "$MAX" ] && PRE=$MAX
        echo "$(date +%H:%M) recovered ($RATE% over $C gens, fastest ${BEST}s) - promoting to PreLearn=$PRE"
        cp data/ckpt.npz "data/ckpt_prelearn_$((PRE-STEP)).npz"
        restart "$PRE"; MARK=$(gen_now)
      fi
    fi
  fi

  if [ $((i % 6)) -eq 0 ]; then
    echo "$(date +%H:%M) PreLearn=$PRE  $(head -1 /tmp/w.txt)  |  $C gens since: $RATE% finished, median ${MED}s, fastest ${BEST}s"
    python tools/report.py > /dev/null 2>&1
  fi
  sleep 300
done
echo "=== final ==="; bash tools/watch.sh; bash tools/compare.sh 8858 258
