#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# BST Benchmark Runner
# ============================================================================
# Usage:
#   ./benchmark.sh [output_csv] [ops_per_thread] [key_space] [repeats] [work_iters]
# Example:
#   ./benchmark.sh results.csv 50000 2000 5 500

OUTPUT_CSV="${1:-results.csv}"
OPS_PER_THREAD="${2:-50000}"
KEY_SPACE="${3:-2000}"
REPEATS="${4:-5}"
WORK_ITERS="${5:-0}"

THREADS=(1 2 4 8 16)

# ---- Build -----------------------------------------------------------------
echo "=== Building all variants ==="
g++ -std=c++20 -O2 -pthread singleLockBST.cpp -o singleLockBST
echo "  [ok] singleLockBST"
g++ -std=c++20 -O2 -pthread handOverBST.cpp   -o handOverBST
echo "  [ok] handOverBST"
g++ -std=c++20 -O2 -pthread BST.cpp           -o lockFreeBST
echo "  [ok] lockFreeBST"

# ---- CSV header ------------------------------------------------------------
echo "variant,threads,ops_per_thread,total_ops,rep,time_us,throughput_ops_s" \
    > "$OUTPUT_CSV"

# ---- Run benchmarks --------------------------------------------------------
BINARIES=("./singleLockBST" "./handOverBST" "./lockFreeBST")

total_runs=$(( ${#BINARIES[@]} * ${#THREADS[@]} * REPEATS ))
run=0

for bin in "${BINARIES[@]}"; do
  for t in "${THREADS[@]}"; do
    for rep in $(seq 1 "$REPEATS"); do
      run=$((run + 1))
      echo -ne "\r  [$run/$total_runs] $bin  threads=$t  rep=$rep    "

      line=$($bin benchmark "$t" "$OPS_PER_THREAD" "$KEY_SPACE" "$WORK_ITERS")

      # Parse key=value output into CSV row
      echo "$line" | awk -v rep="$rep" '
        {
          for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            v[kv[1]] = kv[2]
          }
          printf "%s,%s,%s,%s,%s,%s,%s\n", \
            v["variant"], v["threads"], \
            v["total_ops"] / v["threads"], v["total_ops"], \
            rep, v["time_us"], v["throughput_ops_s"]
        }
      ' >> "$OUTPUT_CSV"
    done
  done
done

echo ""
echo "=== Done: $OUTPUT_CSV ($total_runs runs) ==="
