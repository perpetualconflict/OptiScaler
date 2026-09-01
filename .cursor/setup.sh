#!/usr/bin/env bash
# Cloud Agent environment bootstrap for OptiScaler.
#
# OptiScaler itself is a Windows-only C++ project (MSBuild / Visual Studio 2022,
# v143 toolset) targeting DirectX 11/12, Vulkan and NVNGX, so the shipping
# DLL cannot be compiled or run on a Linux Cloud Agent. This script prepares the
# parts of the development workflow that ARE reproducible on Linux:
#   1. a complete source tree (all git submodules), and
#   2. clang-format 20 to run the enforced style check
#      (.github/workflows/clang-format.yml runs clang-format v20 on ubuntu-latest).
#
# It is intended to be run from the repository root and is safe to re-run.
set -euo pipefail

CF_VERSION="20.1.8"
VENV_DIR="$HOME/.venvs/optiscaler-tools"

echo ">> Initializing git submodules..."
git submodule update --init --recursive

echo ">> Ensuring python venv support is available..."
if ! python3 -c "import venv, ensurepip" >/dev/null 2>&1; then
    sudo apt-get update -qq
    sudo apt-get install -y -qq python3-venv || sudo apt-get install -y -qq python3.12-venv
fi

echo ">> Installing clang-format ${CF_VERSION} (matches CI)..."
if [ ! -x "$VENV_DIR/bin/clang-format" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet "clang-format==${CF_VERSION}"

# Expose clang-format on the default PATH without mutating shell profiles.
sudo ln -sf "$VENV_DIR/bin/clang-format" /usr/local/bin/clang-format

echo ">> OptiScaler development environment ready."
clang-format --version
