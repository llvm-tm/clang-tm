#!/usr/bin/env bash
# Kill any stale benchmark/fuzz processes that might be lingering
set -e
PIDS=$(ps aux | grep -E 'stamp_|tpcc_|stmbench|ycsb_|bank|fuzz_bank|fuzz_counter|run_compare|dudetm|persist_dudetm' | grep -v grep | awk '{print $2}')
if [ -n "$PIDS" ]; then
    echo "Killing stale processes: $PIDS"
    kill -9 $PIDS 2>/dev/null
    sleep 1
    REMAINING=$(ps aux | grep -E 'stamp_|tpcc_|stmbench|ycsb_|bank|fuzz_bank|fuzz_counter|run_compare|dudetm|persist_dudetm' | grep -v grep | wc -l)
    echo "Remaining: $REMAINING"
else
    echo "No stale processes found."
fi
