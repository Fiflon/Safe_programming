#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./benchmark_to_csv.sh [output_csv] [accounts] [transfers] [repeats]
# Example:
#   ./benchmark_to_csv.sh results.csv 2000 200000 5

OUTPUT_CSV="${1:-results.csv}"
ACCOUNTS="${2:-2000}"
TRANSFERS="${3:-200000}"
REPEATS="${4:-5}"

THREADS=(1 2 4 8 16)
MODES=(broad safe)

echo "Building run binary..."
g++ -std=c++20 -O2 -pthread main.cpp -o run

echo "mode,threads,accounts,transfers,rep,time_us,throughput_ops_s,latency_us_op" > "$OUTPUT_CSV"

for mode in "${MODES[@]}"; do
  for t in "${THREADS[@]}"; do
    for rep in $(seq 1 "$REPEATS"); do
      line=$(./run "$t" "$ACCOUNTS" "$TRANSFERS" "$mode" 0)
      echo "$line" | awk -v rep="$rep" '
        {
          for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            vals[kv[1]] = kv[2]
          }
          printf "%s,%s,%s,%s,%s,%s,%s,%s\n", \
                 vals["mode"], vals["threads"], vals["accounts"], vals["transfers"], \
                 rep, vals["time_us"], vals["throughput_ops_s"], vals["latency_us_op"]
          delete vals
        }
      ' >> "$OUTPUT_CSV"
    done
  done
done

echo "Saved: $OUTPUT_CSV"
