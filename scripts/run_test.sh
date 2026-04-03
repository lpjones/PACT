#!/usr/bin/env bash
# run_test.sh Usage: ./run_test.sh              # runs the example at the bottom
# OR source this file and call test "myconfig" "<full command line>"

PRELOAD="/proj/TppPlus/tpp/pact/src/libpact.so"
CGUPS_DIR="../workloads/cgups"
MGUPS_DIR="../../scripts/my_gups"
HGUPS_DIR="../workloads/hgups"
GAPBS_DIR="../workloads/gapbs"
RESNET_DIR="../workloads/resnet"
STREAM_DIR="../workloads/stream"
ycsb_dir="../workloads/YCSB"
memcache_dir="../workloads/memcached"
PLOT_SCRIPTS_DIR="plot_scripts"

result_dir="results"

ORIG_PWD="$(pwd)"

# run once to pin/disable CPUs if you have such a helper if [ -x
# ./disable_cpus.sh ]; then echo "Running disable_cpus.sh" ./disable_cpus.sh
#   else echo "Warning: disable_cpus.sh not found or not executable; skipping."
#   fi

# Ensure result_dir exists
mkdir -p "${result_dir}"

# Normalize environment: a helper to prepare system before each run
_prepare_system() {
  # clear caches, turn off swap, disable automatic NUMA balancing, flush
  echo "Dropping caches, turning off swap, disabling numa balancing..."
  echo 3 > /proc/sys/vm/drop_caches || echo "Failed to drop caches (permission?)"
  swapoff -a || echo "swapoff failed or no swap configured"
  echo 0 > /proc/sys/kernel/numa_balancing || echo "Failed to set numa_balancing"
  sync
}

run_make() {
  cd ../src || return 1

  if ! make clean > build.log 2>&1; then
    cat build.log
    exit 1
  fi

  if ! make "$@" > build.log 2>&1; then
    cat build.log
    exit 1
  fi

  rm -f build.log
  cd ../scripts || return 1
  echo "$@" > make_config.txt
}

# test <config_name> <command...> Example: test "cgups"
# "${CGUPS_DIR}/gups64-rw 16 move 30 kill 60"
test() {
  if [ $# -lt 3 ]; then
    echo "Usage: test <config_name> <dir> <command...>"
    return 2
  fi

  local config="$1"; shift
  local work_dir="$1"; shift
  # The rest of the arguments form the command to run. Respect spaces & quoting.
  local cmd=( "$@" )

  local app_dir="${ORIG_PWD}/${result_dir}/${config}"
  rm -rf "${app_dir}"
  mkdir -p "${app_dir}"

  echo
  echo "=== Running config='${config}' cmd='${cmd[*]}' ==="
  _prepare_system

  # Files inside app_dir
  local stdout_file="${app_dir}/app.txt"
  local stderr_file="${app_dir}/stderr.txt"

  cd "${work_dir}"
  numactl -N0 env LD_PRELOAD="${PRELOAD}" "${cmd[@]}" > "${stdout_file}" 2> "${stderr_file}" &
  # numactl -N0 "${cmd[@]}" > "${stdout_file}" 2> "${stderr_file}" &
  # gdb --args  numactl -N0 env LD_PRELOAD="${PRELOAD}" "${cmd[@]}"
  # monitor until app exits
  local app_pid=$!

  echo "Started (pid=$app_pid)"

  while kill -0 "$app_pid" 2>/dev/null; do
      {
          date +%s
          cat /proc/vmstat
          echo
      } >> "$app_dir/vmstat.txt"

      {
          date +%s
          numactl -H
          echo
      } >> "$app_dir/numactl.txt"
      sleep 1
  done

  wait "$app_pid"

  mv -f stats.txt "${app_dir}"
  mv -f debuglog.txt "${app_dir}"
  mv -f pact_trace.bin "${app_dir}"
  mv -f pebs.bin "${app_dir}"
  mv -f promote.bin "${app_dir}"
  mv -f demote.bin "${app_dir}"
  mv -f perf.data "${app_dir}"


  cd "${ORIG_PWD}"
  cp make_config.txt "${app_dir}"

  echo "Run finished (rc=${rc}). stdout -> ${stdout_file}, stderr -> ${stderr_file}"
}

function run_ycsb() {
  ### Start of memcached
  local config=$1; shift
  local memcache_size=$1; shift

  local workloads=( "$@" )

  local app="${ORIG_PWD}/${result_dir}/${config}/app.txt"
  local err="${ORIG_PWD}/${result_dir}/${config}/err.txt"

  # start up memcached server
  test $config "${memcache_dir}" "./memcached" -m $memcache_size -u root -p 11211 -t 4 &
  sleep 2

  cd $ycsb_dir

  # Load RAM
  echo "Loading"

  ./bin/ycsb load memcached \
    -p memcached.hosts=127.0.0.1:11211 \
    -threads 32 -s \
    -P workloads/sequential >> $app 2>> $err &

  YCSB_PID=$!

  while true; do
      sleep 1
      evict=$(printf "stats\r\n" \
        | nc -N 127.0.0.1 11211 \
        | awk '$2=="evictions" {print $3}' \
        | tr -d '\r')

      if [[ "$evict" =~ ^[0-9]+$ ]] && [ "$evict" -gt 0 ]; then
          echo "Memcached full — stopping load"
          kill $YCSB_PID
          break
      fi
  done

  wait $YCSB_PID

  for workload in "${workloads[@]}"; do
    sleep 2
    echo "Workload $workload"
    echo "Workload $workload" >> $app
    date +%s >> $app
    
    ./bin/ycsb run memcached -p memcached.hosts=127.0.0.1:11211 -threads 4 -s -P workloads/$workload >> $app 2>> $err
  done

  cd -

  echo "killing memcached..."
  pkill -9 memcache

}


f_buf=536870912
record=0
period=100
page_size=4096

# Make  
run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
  bfs_algo=0 dfs_algo=1 lru_algo=1 sample_period=$period \
  record=$record fast_buffer=$f_buf page_size=$page_size

# The actual trials are like 1GB
# test "bfs-pagr" "${GAPBS_DIR}" "./bfs" -g 27 -n 64 -r 0
# test "resnet-pagr" "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

# test "cgups-pagr" "${CGUPS_DIR}" "./gups64-rw" 8 move 60 kill 120

run_ycsb "memcache-dis" 32000 "workloada" "workloadb" "workloadc" "workloadd" "workloade" "workloadf" "hotspot"

# test "stream-pagr" "${STREAM_DIR}" "./stream" 12288 50



# run_ycsb "memcache-pagr" 32000 "workloada"

# test "resnet_tf-PAGR-${app}" "${RESNET_DIR}" "./build/resnet_train"
# ./single_plots "resnet-PAGR-${app}"

# ./single_plots "cgups-PAGR-${app}"


# test "bfs-pagr" "${GAPBS_DIR}" "./bfs" -f "com-friendster.ungraph.sg" -n 64 -r 0


# test "bfs-PAGR-${app}" "${GAPBS_DIR}" "./bfs" -g 27 -n 64 -r 0
# ./single_plots "bfs-PAGR-${app}"

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "resnet-hem-${app}" "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

# echo always |  tee /sys/kernel/mm/transparent_hugepage/enabled
# echo always |  tee /sys/kernel/mm/transparent_hugepage/defrag
# # CGUPS
# run_make cluster_algo=1 hem_algo=0 dfs_algo=1 lru_algo=1 \
#   dec_down=0.0001 dec_up=0.01 sample_period=$period record=$record fast_buffer=$f_buf
# test "cgups-PAGR-${app}" "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 lru_algo=0 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "cgups-hem-${app}" "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# echo never |  tee /sys/kernel/mm/transparent_hugepage/enabled
# echo never |  tee /sys/kernel/mm/transparent_hugepage/defrag

# BFS
# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 lru_algo=1 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "bfs-PAGR-${app}" "${GAPBS_DIR}" "./bfs" -g 27 -n 64 -r 0

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "bfs-hem-${app}" "${GAPBS_DIR}" "./bfs" -g 27 -n 64 -r 0

# # Stream
# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 lru_algo=1 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "stream-PAGR-${app}" "${STREAM_DIR}" "./stream" 16384 50

# run_make cluster_algo=0 hem_algo=1 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "stream-hem-${app}" "${STREAM_DIR}" "./stream" 16384 50

# # BC
# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 lru_algo=1 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "bc-PAGR-${app}" "${GAPBS_DIR}" "./bc" -g 27 -n 64 -r 0

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 sample_period=$period \
#   record=$record fast_buffer=$f_buf
# test "bc-hem-${app}" "${GAPBS_DIR}" "./bc" -g 27 -n 64 -r 0

# echo never |  tee /sys/kernel/mm/transparent_hugepage/enabled
# echo never |  tee /sys/kernel/mm/transparent_hugepage/defrag

# run_make cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 bfs_algo=0 dfs_algo=1 fast_buffer=33554432 sample_period=12800 record=0
# test "resnet-PAGR1" "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"


# echo always |  tee /sys/kernel/mm/transparent_hugepage/enabled
# echo always |  tee /sys/kernel/mm/transparent_hugepage/defrag

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 fast_buffer=0 fast_size=2147483648
# run_make cluster_algo=1 hem_algo=0 dfs_algo=1 all_algo=0 fast_buffer=1073741824 lru_algo=1 sample_period=100
# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 fast_buffer=1073741824
# test "cgups-PAGR" "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 bfs_algo=0 dfs_algo=1 fast_buffer=1073741824
# test "cgups-PAGR" "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

#resnet current best

# THP echo always |  tee /sys/kernel/mm/transparent_hugepage/enabled echo
# always |  tee /sys/kernel/mm/transparent_hugepage/defrag

# Regular echo never |  tee /sys/kernel/mm/transparent_hugepage/enabled echo
# never |  tee /sys/kernel/mm/transparent_hugepage/defrag



# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 test "resnet-hem"
# "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

# run_make cluster_algo=0 hem_algo=0 dfs_algo=0 test "resnet-no-155"
# "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"




# run_make pebs_stats=1 cluster_algo=0 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 page_size=4096 bfs_algo=0 test "resnet-best-4KB"
# "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

# run_make cluster_algo=0 hem_algo=1 page_size=4096 test "resnet-hem-4KB"
# "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"


# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 page_size=1048576 test "resnet-best-1MB" "${RESNET_DIR}"
# "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

# run_make cluster_algo=0 hem_algo=1 page_size=1048576 test "resnet-hem-1MB"
# "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

# run_make cluster_algo=1 hem_algo=0 page_size=262144 test
# "resnet-cluster-8MB" "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python"
# "resnet_train.py"

# run_make cluster_algo=1 hem_algo=1 page_size=262144 test "resnet-both-8MB"
# "${RESNET_DIR}" "${ORIG_PWD}/venv/bin/python" "resnet_train.py"

#cgups

# run_make cluster_algo=0 hem_algo=0 dfs_algo=0 test "cgups-no"
# "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60



# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.00005 dec_up=0.1 \
#   max_neighbors=8 dfs_algo=1 test "cgups-PAGR" "${CGUPS_DIR}" "./gups64-rw"
# 8 move 30 kill 60

# run_make cluster_algo=1 hem_algo=1 dfs_algo=1 test "cgups-both"
# "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# echo never |  tee /sys/kernel/mm/transparent_hugepage/enabled echo never |
#  tee /sys/kernel/mm/transparent_hugepage/defrag

# run_make cluster_algo=0 hem_algo=0 dfs_algo=1 test "cgups-no-reg"
# "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# run_make cluster_algo=0 hem_algo=1 dfs_algo=1 test "cgups-hem-reg"
# "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# run_make cluster_algo=1 hem_algo=0 dfs_algo=1 test "cgups-cluster-reg"
# "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# run_make cluster_algo=1 hem_algo=1 dfs_algo=1 test "cgups-both"
# "${CGUPS_DIR}" "./gups64-rw" 8 move 30 kill 60

# #bfs THP echo always |  tee /sys/kernel/mm/transparent_hugepage/enabled
# echo always |  tee /sys/kernel/mm/transparent_hugepage/defrag


# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 
# test "bfs-PAGR" "${GAPBS_DIR}" "./bfs" -f "twitter-2010.sg" -n 64 -r 0

# Regular echo never |  tee /sys/kernel/mm/transparent_hugepage/enabled echo
# never |  tee /sys/kernel/mm/transparent_hugepage/defrag

# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 test "bfs-PAGR" "${GAPBS_DIR}" "./bfs" -f
# "twitter-2010.sg" -n 64 -r 0

# run_make cluster_algo=0 hem_algo=0 dfs_algo=0 test "bfs-no" "${GAPBS_DIR}"
# "./bfs" -f "twitter-2010.sg" -n 64 -r 0

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 test "bfs-hem" "${GAPBS_DIR}"
# "./bfs" -f "twitter-2010.sg" -n 64 -r 0


# run_make cluster_algo=1 hem_algo=1 test "bfs-both" "${GAPBS_DIR}" "bfs" -f
# "twitter-2010.sg" -n 64 -r 0

# #stream

# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 test "stream-PAGR" "${STREAM_DIR}" "./stream"
# 2048 50

# echo never |  tee /sys/kernel/mm/transparent_hugepage/enabled echo never |
#  tee /sys/kernel/mm/transparent_hugepage/defrag

# run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 test "stream-PAGR" "${STREAM_DIR}" "./stream"
# 2048 50

# run_make cluster_algo=0 hem_algo=0 test "stream-no" "${STREAM_DIR}"
# "./stream" 2048 50

# run_make cluster_algo=0 hem_algo=1 test "stream-hem" "${STREAM_DIR}"
# "./stream" 2048 50

# run_make cluster_algo=1 hem_algo=0 test "stream-cluster" "${STREAM_DIR}"
# "./stream" 2048 50

# run_make cluster_algo=1 hem_algo=1 test "stream-both" "${STREAM_DIR}"
# "./stream" 2048 50

# bc run_make pebs_stats=1 cluster_algo=1 hem_algo=0 \
#   his_size=8 pred_depth=16 dec_down=0.0001 dec_up=0.01 \
#   max_neighbors=8 dfs_algo=1 test "bc-PAGR" "${GAPBS_DIR}" "./bc" -f
# "twitter-2010.sg" -n 64 -r 0

# run_make cluster_algo=0 hem_algo=0 dfs_algo=0 test "bc-no" "${GAPBS_DIR}"
# "./bc" -f "twitter-2010.sg" -n 64 -r 0

# run_make cluster_algo=0 hem_algo=1 dfs_algo=0 test "bc-hem" "${GAPBS_DIR}"
# "./bc" -f "twitter-2010.sg" -n 64 -r 0
