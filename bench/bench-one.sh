#!/usr/bin/env bash
# Runs one server implementation, samples its peak RSS while an external load
# generator drives traffic, then reports and cleans up.
# Usage: bench-one.sh <name> <db-file> <binary-and-args...>
set -u
NAME=$1; shift
DB=$1; shift

rm -f "$DB" "/tmp/${NAME}.ready" "/tmp/${NAME}.peak"

"$@" > "/tmp/${NAME}.log" 2>&1 &
PID=$!

# wait for the server to accept connections (up to 30s)
UP=0
for i in $(seq 1 300); do
  if curl -s -m 1 -o /dev/null "http://127.0.0.1:8080/todos"; then UP=1; break; fi
  sleep 0.1
done
if [ "$UP" != "1" ]; then
  echo "${NAME} FAILED_TO_START"
  cat "/tmp/${NAME}.log"
  kill $PID 2>/dev/null
  exit 1
fi

# peak RSS sampler (KB @ 100ms)
(
  MAX_RSS=0
  while kill -0 $PID 2>/dev/null; do
    RSS=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
    if [ -n "$RSS" ] && [ "$RSS" -gt "$MAX_RSS" ]; then MAX_RSS=$RSS; fi
    sleep 0.1
  done
  echo "$MAX_RSS" > "/tmp/${NAME}.peak"
) &
SAMPLER=$!

echo "${NAME} READY pid=$PID"
touch "/tmp/${NAME}.ready"

# controller removes the ready file when the load test is done
while [ -f "/tmp/${NAME}.ready" ]; do sleep 0.2; done
sleep 0.5

kill $PID 2>/dev/null
kill $SAMPLER 2>/dev/null
wait 2>/dev/null

echo "${NAME} peak_rss_kb=$(cat /tmp/${NAME}.peak 2>/dev/null || echo NA)"
