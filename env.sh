#!/bin/bash

NDK=/home/ping/android-ndk-r10e

ANDROID_VERSION=19
NDK_PATH=$NDK
HOST=arm-linux-androideabi
SYSROOT=${NDK_PATH}/platforms/android-${ANDROID_VERSION}/arch-arm
ANDROID_CFLAGS="-march=armv7-a -mfloat-abi=softfp -fprefetch-loop-arrays --sysroot=${SYSROOT}"
TOOLCHAIN_VERSION=4.9
BUILD_PLATFORM=linux-x86_64
CXX_STL=gnu-libstdc++
TARGET_ARCH=armeabi

TOOLCHAIN=${NDK_PATH}/toolchains/${HOST}-${TOOLCHAIN_VERSION}/prebuilt/${BUILD_PLATFORM}
CXX_STL_PATH=${NDK_PATH}/sources/cxx-stl/${CXX_STL}/${TOOLCHAIN_VERSION}/
CXX_STL_CPPFLAGS="-I${CXX_STL_PATH}/include -I${CXX_STL_PATH}/libs/${TARGET_ARCH}/include"
CXX_STL_LDFLAGS="-L${CXX_STL_PATH}/libs/${TARGET_ARCH}/"

export PKG_CONFIG_PATH="${SYSROOT}/usr/lib/pkgconfig/:${SYSROOT}/usr/local/lib/pkgconfig/"
export CPP=${TOOLCHAIN}/bin/${HOST}-cpp
export AR=${TOOLCHAIN}/bin/${HOST}-ar
export AS=${TOOLCHAIN}/bin/${HOST}-as
export NM=${TOOLCHAIN}/bin/${HOST}-nm
export CC=${TOOLCHAIN}/bin/${HOST}-gcc
export LD=${TOOLCHAIN}/bin/${HOST}-ld
export RANLIB=${TOOLCHAIN}/bin/${HOST}-ranlib
export OBJDUMP=${TOOLCHAIN}/bin/${HOST}-objdump
export STRIP=${TOOLCHAIN}/bin/${HOST}-strip
export CXX=${TOOLCHAIN}/bin/${HOST}-g++

export PATH=${TOOLCHAIN}/bin:$PATH

