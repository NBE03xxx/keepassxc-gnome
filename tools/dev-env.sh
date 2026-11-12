#!/usr/bin/env bash
# Source this file to use the project-local development prefix.
kpxc_project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kpxc_prefix="$kpxc_project/.cache/development/sysroot/usr"
kpxc_arch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
kpxc_lib="$kpxc_prefix/lib/$kpxc_arch"
export PATH="$kpxc_prefix/bin:$PATH"
export CMAKE_PREFIX_PATH="$kpxc_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$kpxc_lib/pkgconfig:$kpxc_prefix/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$kpxc_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$kpxc_lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
