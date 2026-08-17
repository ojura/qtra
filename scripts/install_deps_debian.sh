#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "run as root (or invoke through sudo)" >&2
  exit 2
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  cmake \
  ninja-build \
  python3 \
  qt6-base-dev \
  qt6-base-dev-tools \
  libqt6opengl6-dev \
  libgl1-mesa-dri \
  mesa-utils \
  xvfb \
  xauth
