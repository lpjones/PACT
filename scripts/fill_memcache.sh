#!/bin/bash

HOST=127.0.0.1
PORT=11211

memtier_benchmark \
  --protocol=memcache_text \
  --server=$HOST \
  --port=$PORT \
  --ratio=1:0 \
  --data-size=4096 \
  --key-pattern=S:S \
  --key-maximum=50000000 \
  --clients=64 \
  --threads=8 \
  --test-time=1000000 &
PID=$!

E0=$(printf "stats\n" | nc -w 1 $HOST $PORT | awk '/^STAT evictions / {print $3}' | tr -d '\r')
E0=${E0:-0}

echo "Starting evictions: $E0"

while true; do
  E=$(printf "stats\n" | nc -w 1 $HOST $PORT | awk '/^STAT evictions / {print $3}' | tr -d '\r')
  E=${E:-0}

#   echo "Evictions: $E"

  if (( E > E0 )); then
    echo "Evictions started. Cache is full. Stopping memtier..."
    kill $PID
    wait $PID 2>/dev/null
    break
  fi

  sleep 1
done
