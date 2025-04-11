#!/bin/bash

ARCH=$( uname )

[[ "$ARCH" != "Linux" ]] && echo "- ARCH $ARCH not support!" 1>&2 && exit 1
[[ ! -e /etc/os-release ]] && echo "- /etc/os-release not found!" 1>&2 && exit 1
. /etc/os-release
[[ "$NAME" != "Ubuntu" ]] && echo "- Only support Ubuntu, but got $NAME!" 1>&2 && exit 1
[[ "$VERSION_ID" != "22.04" ]] && echo "- Only support Ubuntu version 22.04, but got version $VERSION_ID!" 1>&2 && exit 1

CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-"Release"}
INSTALL_DEPENDENT_LIBS=${INSTALL_DEPENDENT_LIBS:-true}
if [[ ${INSTALL_DEPENDENT_LIBS} == true ]]; then
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        git \
        libcurl4-openssl-dev \
        libgmp-dev \
        llvm-11-dev \
        python3-numpy \
        file \
        zlib1g-dev
fi
export CPU_CORES=${CPU_CORES:-$(nproc)}
export BUILD_DIR=${BUILD_DIR:-"build"}
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}
cmake -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 $CMAKE_ARGS ..
make -j "${CPU_CORES}" package