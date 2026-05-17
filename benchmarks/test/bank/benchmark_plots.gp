set terminal pngcairo size 1400,900 enhanced font 'DejaVu Sans,12'
set style data linespoints
set grid

# ============================================================
# Plot 1: Throughput comparison (all backends)
# ============================================================
set output 'throughput.png'
set title 'Bank Benchmark Throughput (-r 0, 1024 accounts, 5s duration)'
set xlabel 'Threads'
set ylabel 'Throughput (txns/sec)'
set key top left
set yrange [0:*]

plot '/tmp/thru_tinystm_wbctl.dat' using 1:2:3 with yerrorlines title 'TinySTM WBCTL' lw 2 lc rgb '#2ca02c' pt 7, \
     '/tmp/thru_singlelock.dat' using 1:2:3 with yerrorlines title 'SingleGlobalLock' lw 2 lc rgb '#1f77b4' pt 7, \
     '/tmp/thru_tsxsgl.dat' using 1:2:3 with yerrorlines title 'TSXSGL' lw 2 lc rgb '#d62728' pt 7, \
     '/tmp/thru_uninstrumented.dat' using 1:2:3 with yerrorlines title 'Uninstrumented (t=1 only, rest fail)' lw 2 lc rgb '#7f7f7f' pt 7

# ============================================================
# Plot 2: Throughput (TM backends only, zoomed)
# ============================================================
set output 'throughput_tm.png'
set title 'Bank Benchmark Throughput - TM Backends (-r 0, 1024 accounts, 5s)'
set xlabel 'Threads'
set ylabel 'Throughput (txns/sec)'
set key top left
set yrange [0:6000000]

plot '/tmp/thru_tinystm_wbctl.dat' using 1:2:3 with yerrorlines title 'TinySTM WBCTL' lw 2 lc rgb '#2ca02c' pt 7, \
     '/tmp/thru_singlelock.dat' using 1:2:3 with yerrorlines title 'SingleGlobalLock' lw 2 lc rgb '#1f77b4' pt 7, \
     '/tmp/thru_tsxsgl.dat' using 1:2:3 with yerrorlines title 'TSXSGL' lw 2 lc rgb '#d62728' pt 7

# ============================================================
# Plot 3: TSXSGL TSX abort analysis
# ============================================================
set output 'tsx_abort_analysis.png'
set title 'TSXSGL: TSX Abort Rate vs Thread Count'
set xlabel 'Threads'
set ylabel 'Rate (%)'
set yrange [0:105]
set key top left

plot '/tmp/tsx_abort.dat' using 1:2 title 'TSX commit rate (% of TX)' lw 2 lc rgb '#2ca02c' pt 7 with lines, \
     '/tmp/tsx_abort.dat' using 1:3 title 'SGL fallback rate (% of TX)' lw 2 lc rgb '#d62728' pt 7 with lines, \
     '/tmp/tsx_abort.dat' using 1:4 title 'TSX abort rate (per _xbegin attempt)' lw 2 lc rgb '#1f77b4' pt 7 with lines

# ============================================================
# Plot 4: TSXSGL absolute counts
# ============================================================
set output 'tsx_counts.png'
set title 'TSXSGL: TSX Commits vs SGL Entries (per 5s run)'
set xlabel 'Threads'
set ylabel 'Count'
set key top left
set yrange [0:*]

plot '/tmp/tsx_abort.dat' using 1:8 title 'TSX commits' lw 2 lc rgb '#2ca02c' pt 7 with lines, \
     '/tmp/tsx_abort.dat' using 1:7 title 'SGL entries' lw 2 lc rgb '#d62728' pt 7 with lines
