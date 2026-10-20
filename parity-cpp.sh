#!/bin/bash
# Compare C++ implementation outputs against the captured Rust baseline.
# Usage: ./parity-cpp.sh
set -u
cd "$(dirname "$0")"
OUT=/tmp/marina-parity/cpp
RUST=/tmp/marina-parity/rust
mkdir -p "$OUT"
WAVE=build/wave

EXAMPLES=(
  hello-world calculate for-loop const-test array-list array-extra
  async-example objects generics interfaces fs-crypto
)

fail=0
for ex in "${EXAMPLES[@]}"; do
  "$WAVE" run "examples/$ex.tsm" >"$OUT/$ex.out" 2>"$OUT/$ex.err"
  echo "exit=$?" >>"$OUT/$ex.out"
  if ! diff -q "$RUST/$ex.out" "$OUT/$ex.out" >/dev/null 2>&1; then
    echo "FAIL stdout: $ex"
    diff "$RUST/$ex.out" "$OUT/$ex.out" | head -10
    fail=1
  fi
  # stderr may differ in clang -Woverride-module noise; compare only errors
  grep -v "override-module\|1 warning generated\|^warning:\|^$" "$RUST/$ex.err" >"$RUST/$ex.err.clean" || true
  grep -v "override-module\|1 warning generated\|^warning:\|^$" "$OUT/$ex.err" >"$OUT/$ex.err.clean" || true
  if ! diff -q "$RUST/$ex.err.clean" "$OUT/$ex.err.clean" >/dev/null 2>&1; then
    echo "FAIL stderr: $ex"
    diff "$RUST/$ex.err.clean" "$OUT/$ex.err.clean" | head -10
    fail=1
  fi
done

for f in /tmp/marina-parity/bad/*.tsm; do
  name=$(basename "$f" .tsm)
  "$WAVE" run "$f" >"$OUT/diag-$name.out" 2>"$OUT/diag-$name.err"
  echo "exit=$?" >>"$OUT/diag-$name.out"
  if ! diff -q "$RUST/diag-$name.out" "$OUT/diag-$name.out" >/dev/null 2>&1; then
    echo "FAIL stdout: diag-$name"
    diff "$RUST/diag-$name.out" "$OUT/diag-$name.out" | head -10
    fail=1
  fi
  # normalise the source path (temp dir is shared, so should match)
  if ! diff -q "$RUST/diag-$name.err" "$OUT/diag-$name.err" >/dev/null 2>&1; then
    echo "FAIL stderr: diag-$name"
    diff "$RUST/diag-$name.err" "$OUT/diag-$name.err" | head -10
    fail=1
  fi
done

if [ "$fail" = 0 ]; then
  echo "ALL PARITY CHECKS PASSED"
else
  echo "PARITY FAILURES FOUND"
fi
