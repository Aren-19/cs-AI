#!/bin/bash
# Compare results since a change against the settled window before it.
# Columns are looked up by name: the log has gained columns twice, and
# positional indices would have silently read the wrong ones.
cd "$(dirname "$0")/.."
BASE_GEN=${1:-8858}
WINDOW=${2:-258}
awk -F, -v bg="$BASE_GEN" -v w="$WINDOW" '
NR==1 { for (i = 1; i <= NF; i++) col[$i] = i
        need = "gen runs_from_start finished_from_start best_full_run_s median_full_run_s"
        n = split(need, want, " ")
        for (i = 1; i <= n; i++) if (!(want[i] in col)) { print "  train_log.csv has no column " want[i]; exit 1 }
        next }
{
  g = $(col["gen"]) + 0
  runs = $(col["runs_from_start"])
  if (runs == "") next
  fin = $(col["finished_from_start"])
  bst = $(col["best_full_run_s"]) + 0
  med = $(col["median_full_run_s"]) + 0
  if (g <= bg && g > bg - w) {
    bn += runs; bf += fin; bc++
    if (bst > 0 && (bb == 0 || bst < bb)) bb = bst
    if (med > 0) { bm += med; bmc++ }
  } else if (g > bg) {
    an += runs; af += fin; ac++
    if (bst > 0 && (ab == 0 || bst < ab)) ab = bst
    if (med > 0) { am += med; amc++ }
  }
}
END{
  if (bn) printf "  before (gens %d-%d, %d gens): %.1f%% finished, fastest %.3fs, median %s\n",
      bg-w+1, bg, bc, 100*bf/bn, bb, (bmc ? sprintf("%.3fs", bm/bmc) : "not recorded")
  if (!an) { print "  nothing since the change yet"; exit }
  printf "  after  (%d gens)%*s: %.1f%% finished, fastest %.3fs, median %s\n",
      ac, 14, "", 100*af/an, ab, (amc ? sprintf("%.3fs", am/amc) : "not recorded")
  if (!bn) { print "  no baseline in this log to compare against"; exit }
  printf "  change%*s: %+.1f points finished, %+.3fs fastest", 24, "", 100*af/an - 100*bf/bn, ab - bb
  if (amc && bmc) printf ", %+.3fs median", am/amc - bm/bmc
  printf "\n"
}' data/train_log.csv
