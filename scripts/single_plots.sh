PLOT_SCRIPTS_DIR="plot_scripts"

py_bin="./venv/bin/python"

result_dir="results"

ORIG_PWD="$(pwd)"

# run_app <config_name> <command...>
# Example: run_app "cgups" "${CGUPS_DIR}/gups64-rw 16 move 30 kill 60"
run_app() {
  # if [ $# -lt 3 ]; then
  #   echo "Usage: run_app <config_name> <title> <output dir>"
  #   return 2
  # fi

  local config="$1"; shift
  local title="$1"; shift
  local out_dir="$1"; shift
  # The rest of the arguments form the command to run. Respect spaces & quoting.

  local app_dir="${ORIG_PWD}/${result_dir}/${config}"

  # Plots

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_cgups_mul.py" "${app_dir}/app.txt" "${app_dir}/throughput.png"
#   $py_bin "${PLOT_SCRIPTS_DIR}/plot_gapbs_mul.py" "${app_dir}/app.txt" "${app_dir}/gapbs_times.png"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "dram_free" "dram_used" "dram_size" "dram_cap" \
  #   --labels "" \
  #   -o "${app_dir}/dram_stats"

  $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats.py" \
    "${app_dir}/stats.txt" \
    "${app_dir}/accesses.png" \
    --metrics "dram_accesses" "rem_accesses" \
    --labels "Fast accesses" "Slow Accesses" \
    --title "Accesses" \
    --ylabel "Accesses"

  $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats.py" \
    "${app_dir}/stats.txt" \
    "${app_dir}/percent.png" \
    --metrics "percent_dram" \
    --labels "Percent Fast mem" \
    --title "Percent Fast Mem" \
    --ylabel "Percent (%)"

  $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats.py" \
    "${app_dir}/stats.txt" \
    "${app_dir}/mem.png" \
    --metrics "dram_used" "dram_size" "dram_free" "rem_used" \
    --labels "Fast Mem used" "Fast Mem size" "Fast Mem free" "Slow Mem used" \
    --title "Mem" \
    --ylabel "Mem used (Bytes)"

  $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats.py" \
    "${app_dir}/stats.txt" \
    "${app_dir}/migrations.png" \
    --metrics "promotions" "demotions" \
    --labels "Promotions" "Demotions" \
    --title "Migrations" \
    --ylabel "# Migrations"

  $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats.py" \
    "${app_dir}/stats.txt" \
    "${app_dir}/pages.png" \
    --metrics "cold_pages" "hot_pages" \
    --labels "Cold Pages" "Hot Pages" \
    --title "Pages" \
    --ylabel "# Pages"
  
  $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats.py" \
    "${app_dir}/stats.txt" \
    "${app_dir}/hot_pages.png" \
    --metrics "hot_pages" \
    --labels "Hot Pages" \
    --title "Pages" \
    --ylabel "# Pages"

  $py_bin "${PLOT_SCRIPTS_DIR}/plot_cluster_no_app.py" \
    "${app_dir}/pact_trace.bin" \
    --title "Memory Trace" \
    --output "${app_dir}/trace.png" \
    --start-percent 0 \
    --end-percent 100 \
    -fast

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "percent_dram" \
  #   --labels "" \
  #   -o "${app_dir}/percent"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "internal_mem_overhead" \
  #   -g2 "mem_allocated" \
  #   --labels "" \
  #   -o "${app_dir}/mem"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "promotions" "demotions" \
  #   -g2 "threshold" \
  #   --labels "" \
  #   -o "${app_dir}/migrations"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "pebs_resets" \
  #   --labels "" \
  #   -o "${app_dir}/resets"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "mig_move_time" \
  #   -g2 "mig_queue_time" \
  #   --labels "" \
  #   -o "${app_dir}/mig_time"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_stats_mul.py" \
  #   -f "${app_dir}/stats.txt" \
  #   -g1 "cold_pages" "hot_pages" \
  #   --labels "" \
  #   -o "${app_dir}/pages"

  # $py_bin "${PLOT_SCRIPTS_DIR}/plot_cluster_no_app.py" \
  #   "${app_dir}/pact_trace.bin" \
  #   --title "Memory Trace" \
  #   --output "${app_dir}/trace.png" \
  #   --start-percent 50 \
  #   --end-percent 55 \
  #   -fast
  
  
#   $py_bin "${PLOT_SCRIPTS_DIR}/plot_cluster_no_app.py" "${app_dir}/pact_trace.bin" -fast -c cpu

  return ${rc}
}

# run_app bfs-hem-2GB-100
run_app cgups-PAGR

# % modify plots to start y-axis at 0
# % add references to end of background bibtex
# % go through related work and summarize beyond hotness related section
# % move hemem vs local up to background
# % use lru for demotion in design

echo 3 > /proc/sys/vm/drop_caches
sync