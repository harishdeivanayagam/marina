#!/usr/bin/env bash
# Final benchmark: for each implementation —
#   fresh DB, seed 10 rows, idle RSS, GET @ c2, GET @ c64, POST @ c64, peak RSS.
set -u
RESULTS=/root/results2.txt
: > "$RESULTS"

run_impl() {
  local NAME=$1; shift
  echo "=== starting $NAME" >&2
  rm -f /tmp/todos-http-sqlite.db "/tmp/${NAME}.peak"

  "$@" > "/tmp/${NAME}.log" 2>&1 &
  local PID=$!
  local UP=0
  for i in $(seq 1 300); do
    if curl -s -m 1 -o /dev/null "http://127.0.0.1:8080/todos"; then UP=1; break; fi
    sleep 0.1
  done
  if [ "$UP" != "1" ]; then
    echo -e "$NAME\tFAILED_TO_START" >> "$RESULTS"
    kill $PID 2>/dev/null
    return
  fi

  for j in 1 2 3 4 5 6 7 8 9 10; do
    curl -s -m 2 -X POST "http://127.0.0.1:8080/todos" -d "{\"text\":\"seeded todo $j\"}" > /dev/null
  done

  local IDLE_RSS
  IDLE_RSS=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status")

  (
    MAX=0
    while kill -0 $PID 2>/dev/null; do
      R=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
      if [ -n "$R" ] && [ "$R" -gt "$MAX" ]; then MAX=$R; fi
      sleep 0.1
    done
    echo "$MAX" > "/tmp/${NAME}.peak"
  ) &
  local SMP=$!

  local GET2 GET64 POST64
  GET2=$(wrk -t2 -c2 -d10s --latency "http://127.0.0.1:8080/todos" 2>&1)
  GET64=$(wrk -t4 -c64 -d30s --latency "http://127.0.0.1:8080/todos" 2>&1)
  POST64=$(wrk -t4 -c64 -d20s --latency -s /root/post.lua "http://127.0.0.1:8080/todos" 2>&1)

  kill $PID 2>/dev/null
  # give the sampler a moment to observe the death and write its result
  for i in $(seq 1 30); do [ -f "/tmp/${NAME}.peak" ] && break; sleep 0.1; done
  kill $SMP 2>/dev/null
  wait 2>/dev/null

  local PEAK
  PEAK=$(cat "/tmp/${NAME}.peak" 2>/dev/null || echo NA)

  local G2RPS G64RPS G64REQ G64P50 G64P99 P64RPS P64REQ P64P50
  G2RPS=$(echo "$GET2"  | awk '/Requests\/sec/ {print $2}')
  G64RPS=$(echo "$GET64" | awk '/Requests\/sec/ {print $2}')
  G64REQ=$(echo "$GET64" | awk '/requests in/ {print $1}')
  G64P50=$(echo "$GET64" | awk '$1=="50%" {print $2}')
  G64P99=$(echo "$GET64" | awk '$1=="99%" {print $2}')
  P64RPS=$(echo "$POST64" | awk '/Requests\/sec/ {print $2}')
  P64REQ=$(echo "$POST64" | awk '/requests in/ {print $1}')
  P64P50=$(echo "$POST64" | awk '$1=="50%" {print $2}')

  echo -e "$NAME\tidle_rss_kb=${IDLE_RSS}\tpeak_rss_kb=${PEAK}\tGET_c2_rps=${G2RPS}\tGET_c64_rps=${G64RPS}\tGET_c64_total=${G64REQ}\tGET_c64_p50=${G64P50}\tGET_c64_p99=${G64P99}\tPOST_c64_rps=${P64RPS}\tPOST_c64_total=${P64REQ}\tPOST_c64_p50=${P64P50}" >> "$RESULTS"
  echo "=== done $NAME" >&2
}

run_impl marina /opt/marina/.marina/todos-http-sqlite
run_impl node  node  /opt/bench/node/main.mjs
run_impl bun   /root/.bun/bin/bun /opt/bench/bun/main.ts
run_impl go    /opt/bench/go/todo-go

echo "ALL_DONE"
cat "$RESULTS"
