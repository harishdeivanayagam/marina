#!/bin/sh
# Install Marina (wave + marina-lsp) on macOS or Linux.
#   curl -fsSL https://marina-lang.org/install.sh | sh
set -eu

REPO="https://github.com/marina-lang-hq/marina.git"
PREFIX="${MARINA_HOME:-$HOME/.marina}"
SRC="$PREFIX/src"
BIN_DIR="${MARINA_BIN:-$HOME/.local/bin}"

die() {
  echo "marina: $*" >&2
  exit 1
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "missing '$1'. $2"
}

need git "Install git, then re-run this script."
need cmake "Install CMake 3.20 or newer, then re-run this script."
need clang "Install clang. It links the LLVM IR wave emits."

if ! command -v c++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
  die "missing a C++20 compiler (clang++ or g++)."
fi

echo "marina: installing into $PREFIX"
mkdir -p "$PREFIX"

if [ -d "$SRC/.git" ]; then
  echo "marina: updating $SRC"
  git -C "$SRC" fetch --depth 1 origin main
  git -C "$SRC" checkout -f -B main FETCH_HEAD
else
  echo "marina: cloning $REPO"
  rm -rf "$SRC"
  git clone --depth 1 --branch main "$REPO" "$SRC"
fi

echo "marina: building wave and marina-lsp (this compiles the C++ compiler and runtime)"
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build" -j --target wave marina-lsp

mkdir -p "$BIN_DIR"
ln -sfn "$SRC/build/wave" "$BIN_DIR/wave"
ln -sfn "$SRC/build/marina-lsp" "$BIN_DIR/marina-lsp"

echo "marina: installed"
echo "  $BIN_DIR/wave"
echo "  $BIN_DIR/marina-lsp"
echo "  source tree: $SRC  (leave this in place; wave links its runtime from here)"

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *)
    echo ""
    echo "Add $BIN_DIR to your PATH, then open a new shell:"
    echo "  export PATH=\"$BIN_DIR:\$PATH\""
    ;;
esac
