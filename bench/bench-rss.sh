#!/usr/bin/env bash
# Measures peak RSS under load for each implementation.
set -u
RESULTS=/root/results_rss.txt
: > "$RESULTS"

rss_pass() {
  local NAME=$1; shift
  rm -f /tmp/todos-http-sqlite.db "/tmp/${NAME}.peak"
  "$@" > "/tmp/${NAME}.log" 2>&1 &
  local PID=$!
  for i in $(seq 1 300); do
    curl -s -m 1 -o /dev/null "http://127.0.0.1:8080/todos" && break
    sleep 0.1
  done
  for j in 1 2 3 4 5 6 7 8 9 10; do
    curl -s -m 2 -X POST "http://127.0.0.1:8080/todos" -d "{\"text\":\"seeded todo $j\"}" > /dev/null
  done
  (
    MAX=0
    while kill -0 $PID 2>/dev/null; do
      R=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
      if [ -n "$R" ] && [ "$R" -gt "$MAX" ]; then MAX=$R; fi
      sleep 0.05
    done
    echo "$MAX" > "/tmp/${NAME}.peak"
  ) &
  local SMP=$!
  wrk -t4 -c64 -d20s "http://127.0.0.1:8080/todos" > /dev/null 2>&1
  wrk -t4 -c64 -d10s -s /root/post.lua "http://127.0.0.1:8080/todos" > /dev/null 2>&1
  kill $PID 2>/dev/null
  for i in $(seq 1 50); do [ -f "/tmp/${NAME}.peak" ] && break; sleep 0.1; done
  kill $SMP 2>/dev/null
  wait 2>/dev/null
  echo -e "$NAME\toaded_peak_rss_kb=$(cat "/tmp/${NAME}.peak" 2>/dev/null || echo NA)" >> "$RESULTS"
  echo "=== rss done $NAME" >&2
}

rss_pass marina /opt/marina/.marina/todos-http-sqlite
rss_pass node  node  /opt/bench/node/main.mjs
rss_pass bun   /root/.bun/bin/bun /opt/bench/bun/main.ts
rss_pass go    /opt/bench/go/todo-go
echo "RSS_DONE"
cat "$RESULTS"
