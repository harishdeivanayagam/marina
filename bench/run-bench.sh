#!/usr/bin/env bash
# Runs on the benchmark box. Launches one server implementation, samples RSS
# while the (externally driven) load test runs, and reports peak memory.
# Usage: run-bench.sh <name> <db-file> <command...>
set -u
NAME=$1; shift
DB=$1; shift

rm -f "$DB"

# launch server, capture its PID (direct child, not a shell wrapper)
"$@" > "/tmp/${NAME}.log" 2>&1 &
PID=$!

# wait for port 8080 to accept connections (up to 30s)
for i in $(seq 1 300); do
  if curl -s -m 1 -o /dev/null http://127.0.0.1:8080/todos; then
    break
  fi
  sleep 0.1
done

# peak RSS sampler (KB), 100ms interval
MAX_RSS=0
while kill -0 $PID 2>/dev/null; do
  RSS=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null)
  [ -n "${RSS:-}" ] && [ "$RSS" -gt "$MAX_RSS" ] && MAX_RSS=$RSS
  sleep 0.1
done &

SAMPLER=$!

# signal readiness for the load test
touch /tmp/${NAME}.ready

# wait for the load test to finish (controller removes the ready file)
while [ -f /tmp/${NAME}.ready ]; do sleep 0.2; done

kill $SAMPLER 2>/dev/null
# stop the server and its process group
kill $PID 2>/dev/null
wait $PID 2>/dev/null

echo "${NAME} peak_rss_kb=${MAX_RSS}"
