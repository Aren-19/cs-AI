#!/bin/bash
cd "$(dirname "$0")/.."
BASE_GEN=${1:-8858}
WINDOW=${2:-258}
awk -F, -v bg="$BASE_GEN" -v w="$WINDOW" '
NR>1 && $18!="" {
  g=$1+0
  if (g<=bg && g>bg-w) {
    bn+=$18; bf+=$19; bc++
    if ($20+0>0 && (bb==0||$20+0<bb)) bb=$20+0
    if ($21+0>0) { bm+=$21; bmc++ }
  } else if (g>bg) {
    an+=$18; af+=$19; ac++
    if ($20+0>0 && (ab==0||$20+0<ab)) ab=$20+0
    if ($21+0>0) { am+=$21; amc++ }
  }
}
END{
  if (bn) printf "  before (gens %d-%d, %d gens): %.1f%% finished, fastest %.3fs, median %s\n",
      bg-w+1, bg, bc, 100*bf/bn, bb, (bmc ? sprintf("%.3fs", bm/bmc) : "not recorded")
  if (!an) { print "  nothing since the change yet"; exit }
  printf "  after  (%d gens)%*s: %.1f%% finished, fastest %.3fs, median %s\n",
      ac, 14, "", 100*af/an, ab, (amc ? sprintf("%.3fs", am/amc) : "not recorded")
  if (!bn) { print "  no baseline in this log to compare against"; exit }
  printf "  change%*s: %+.1f points finished, %+.3fs fastest", 24, "",
      100*af/an - 100*bf/bn, ab - bb
  if (amc && bmc) printf ", %+.3fs median", am/amc - bm/bmc
  printf "\n"
}' data/train_log.csv
