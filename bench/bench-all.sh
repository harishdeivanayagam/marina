#!/usr/bin/env bash
# Runs the same todos+sqlite HTTP API under marina, node, bun, go on this box.
# Protocol per implementation: fresh DB -> seed 10 rows -> 10s warmup ->
# 30s GET /todos (wrk, 4 threads, 64 conns) -> 20s POST /todos -> peak RSS.
set -u
RESULTS=/root/results.txt
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

  (
    local MAX=0
    while kill -0 $PID 2>/dev/null; do
      local R
      R=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
      if [ -n "$R" ] && [ "$R" -gt "$MAX" ]; then MAX=$R; fi
      sleep 0.1
    done
    echo "$MAX" > "/tmp/${NAME}.peak"
  ) &
  local SMP=$!

  wrk -t2 -c32 -d10s "http://127.0.0.1:8080/todos" > /dev/null 2>&1
  wrk -t4 -c64 -d30s --latency "http://127.0.0.1:8080/todos" > "/tmp/${NAME}.get.txt" 2>&1
  wrk -t4 -c64 -d20s --latency -s /root/post.lua "http://127.0.0.1:8080/todos" > "/tmp/${NAME}.post.txt" 2>&1

  kill $PID $SMP 2>/dev/null
  wait 2>/dev/null

  local PEAK GRPS GREQS GP50 GP99 PRPS PREQS PP50
  PEAK=$(cat "/tmp/${NAME}.peak" 2>/dev/null || echo NA)
  GRPS=$(awk '/Requests\/sec/ {print $2}' "/tmp/${NAME}.get.txt")
  GREQS=$(awk '/requests in/ {print $1}' "/tmp/${NAME}.get.txt")
  GP50=$(awk '$1=="50%" {print $2}' "/tmp/${NAME}.get.txt")
  GP99=$(awk '$1=="99%" {print $2}' "/tmp/${NAME}.get.txt")
  PRPS=$(awk '/Requests\/sec/ {print $2}' "/tmp/${NAME}.post.txt")
  PREQS=$(awk '/requests in/ {print $1}' "/tmp/${NAME}.post.txt")
  PP50=$(awk '$1=="50%" {print $2}' "/tmp/${NAME}.post.txt")

  echo -e "$NAME\tGET_RPS=${GRPS}\tGET_total=${GREQS}\tGET_p50=${GP50}\tGET_p99=${GP99}\tPOST_RPS=${PRPS}\tPOST_total=${PREQS}\tPOST_p50=${PP50}\tpeak_rss_kb=${PEAK}" >> "$RESULTS"
  echo "=== done $NAME" >&2
}

run_impl marina /opt/marina/.marina/todos-http-sqlite
run_impl node  node  /opt/bench/node/main.mjs
run_impl bun   /root/.bun/bin/bun /opt/bench/bun/main.ts
run_impl go    /opt/bench/go/todo-go

echo "ALL_DONE"
column -t "$RESULTS"
