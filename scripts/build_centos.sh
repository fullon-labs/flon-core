#!/bin/bash

CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-"Release"}

ARCH=$( uname )
[[ "$ARCH" != "Linux" ]] && echo "- ARCH $ARCH not support!" 1>&2 && exit 1
[[ ! -e /etc/os-release ]] && echo "- /etc/os-release not found!" 1>&2 && exit 1
. /etc/os-release
# if [[ "$NAME" == "Alibaba Cloud Linux" ]] && [[ $VERSION_ID -eq 3 ]]; then
#     prepare_llvm_install
# else
#     echo "- Unsupported os version ${NAME}-${VERSION_ID}"
# fi

sudo yum update
sudo yum install -y \
        build-essential \
        cmake \
        git \
        libcurl libcurl-devel \
        gmp gmp-devel \
        llvm llvm-devel \
        python3-numpy \
        file \
        zlib1g-dev

export CPU_CORES=${CPU_CORES:-$(nproc)}
export BUILD_DIR=${BUILD_DIR:-"build"}
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}
cmake -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 ..
make -j "${CPU_CORES}" package