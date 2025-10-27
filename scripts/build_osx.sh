#!/bin/bash
set -euo pipefail

# Check OS
if [[ "$(uname)" != "Darwin" ]]; then
  echo "This script is only for macOS." >&2
  exit 1
fi

# Check Xcode Command Line Tools
if ! xcode-select -p &>/dev/null; then
  echo "Xcode Command Line Tools not found. Please install them first:"
  echo "xcode-select --install"
  exit 1
fi

# Check Homebrew
if ! command -v brew &>/dev/null; then
  echo "Homebrew not found. Please install it first:"
  echo '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
  exit 1
fi

# Install dependencies only if not already installed
INSTALL_DEPENDENT_LIBS=${INSTALL_DEPENDENT_LIBS:-true}
if [[ ${INSTALL_DEPENDENT_LIBS} == true ]]; then
  brew update
  for pkg in cmake boost llvm python@3 pkg-config git rocksdb; do
    if ! brew list --versions "$pkg" >/dev/null 2>&1; then
      echo "Installing $pkg ..."
      brew install "$pkg"
    else
      echo "$pkg is already installed, skipping."
    fi
  done
fi

export CPU_CORES=${CPU_CORES:-$(sysctl -n hw.ncpu)}
export CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-"Release"}
export CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH:-"$(brew --prefix llvm)"}

# Set clang/clang++ path (prefer Homebrew llvm)
export CC="${CMAKE_PREFIX_PATH}/bin/clang"
export CXX="${CMAKE_PREFIX_PATH}/bin/clang++"
export PATH="${CMAKE_PREFIX_PATH}/bin:$PATH"

export BUILD_DIR=${BUILD_DIR:-"build"}

echo "Using LLVM from: $CMAKE_PREFIX_PATH"
"$CC" --version

git submodule update --init --recursive

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" ${CMAKE_ARGS:-} ..
make -j "$CPU_CORES" package

echo "Build finished successfully."