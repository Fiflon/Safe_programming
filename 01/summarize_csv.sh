#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./summarize_csv.sh [input_csv]
# Example:
#   ./summarize_csv.sh results.csv

INPUT_CSV="${1:-results.csv}"

awk -F, '
  NR == 1 { next }
  {
    key = $1 "," $2
    cnt[key]++
    sum_time[key] += $6
    sum_thr[key] += $7
    sum_lat[key] += $8
  }
  END {
    print "mode,threads,avg_time_us,avg_throughput_ops_s,avg_latency_us_op"
    for (k in cnt) {
      printf "%s,%.2f,%.2f,%.6f\n", k, sum_time[k] / cnt[k], sum_thr[k] / cnt[k], sum_lat[k] / cnt[k]
    }
  }
' "$INPUT_CSV" | sort -t, -k1,1 -k2,2n
