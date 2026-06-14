#!/usr/bin/env bash
# run-benchmarks-ssh.sh
# Copies the TM API C++ repo to a remote host, builds plugin variants
# and benchmarks, runs them, and prints performance results.
#
# Usage:
#   ./run-benchmarks-ssh.sh [host] [benchmark...]
#
# Examples:
#   ./run-benchmarks-ssh.sh user@remote-host
#   ./run-benchmarks-ssh.sh user@remote-host bank stmbench7
#   ./run-benchmarks-ssh.sh 192.168.1.100 bank

set -euo pipefail

HOST="${1:-user@remote-host}"
REMOTE_DIR="~/tm_api_cpp"
SSH="ssh -o StrictHostKeyChecking=no"
SCP="scp -o StrictHostKeyChecking=no"
RSYNC="rsync -avz --delete"

# Parse benchmark arguments after the host
shift 2>/dev/null || true
BENCHMARKS=("$@")
if [ ${#BENCHMARKS[@]} -eq 0 ]; then
    BENCHMARKS=(bank stmbench7 ycsb eigenbench)
fi

echo "=== Syncing repo to $HOST:$REMOTE_DIR ==="
$RSYNC --exclude='.git' --exclude='*.o' --exclude='bin/' \
    -e "ssh" ./ "$HOST:$REMOTE_DIR/"

echo ""
echo "=== Building plugin variants on $HOST ==="
$SSH "$HOST" "cd $REMOTE_DIR && make -C plugin variants" || {
    echo "WARNING: Plugin build failed. Trying without variants..."
    $SSH "$HOST" "cd $REMOTE_DIR && make -C plugin"
}

echo ""
echo "=== Running benchmarks on $HOST ==="

for bench in "${BENCHMARKS[@]}"; do
    echo ""
    echo "--- $bench ---"
    case "$bench" in
        bank)
            $SSH "$HOST" "cd $REMOTE_DIR && make -C benchmarks/test/bank clean && make -C benchmarks/test/bank -j\$(nproc) 2>&1 | tail -20"
            $SSH "$HOST" "cd $REMOTE_DIR/benchmarks/test/bank && ./run_tests.sh 2>&1 | tail -30"
            ;;
        stmbench7)
            $SSH "$HOST" "cd $REMOTE_DIR && make -C benchmarks/stmbench7 clean && make -C benchmarks/stmbench7 -j\$(nproc) 2>&1 | tail -20"
            $SSH "$HOST" "cd $REMOTE_DIR/benchmarks/stmbench7 && bash run_tests.sh 2>&1 | tail -30"
            ;;
        ycsb)
            $SSH "$HOST" "cd $REMOTE_DIR && make -C benchmarks/ycsb clean && make -C benchmarks/ycsb -j\$(nproc) 2>&1 | tail -20"
            $SSH "$HOST" "cd $REMOTE_DIR/benchmarks/ycsb && ./ycsb_bench 2>&1 | tail -20"
            ;;
        eigenbench)
            $SSH "$HOST" "cd $REMOTE_DIR && make -C benchmarks/eigenbench clean && make -C benchmarks/eigenbench -j\$(nproc) 2>&1 | tail -20"
            $SSH "$HOST" "cd $REMOTE_DIR/benchmarks/eigenbench && ./eigenbench 2>&1 | tail -20"
            ;;
        *)
            echo "Unknown benchmark: $bench"
            ;;
    esac
done

echo ""
echo "=== All benchmarks completed on $HOST ==="
