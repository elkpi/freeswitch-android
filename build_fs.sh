#!/bin/bash

TOPDIR=$(dirname `realpath $0`)
FS_VER=1.6.19

. env.sh

cd $TOPDIR/freeswitch-$FS_VER/libs/libvpx && {
    [ -f Makefile ] || {
        CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie ${CXX_STL_LDFLAGS}" CPPLDFLAGS="${ANDROID_CFLAGS} ${CXX_STL_LDFLAGS}"  \
		./configure --prefix=${SYSROOT}/usr/  --target=armv7-android-gcc --enable-pic --disable-docs --disable-examples --disable-install-bins --disable-install-srcs  --cpu=cortex-a8 --sdk-path=$NDK 
    } && make
}

cd $TOPDIR/freeswitch-$FS_VER && {
    [ -f Makefile ] || {
        autoreconf -fi && ./rebootstrap.sh && PKG_CONFIG_PATH=$PKG_CONFIG_PATH ./configure --with-sysroot=${SYSROOT} --host=${HOST} --prefix=${SYSROOT}/usr/ --disable-core-libedit-support \
            	CC="$CC" LD="$LD" NM="$NM" AR="$AR" CPP="$CPP" CXX="$CXX" CFLAGS="${ANDROID_CFLAGS}  -fPIE -I${SYSROOT}/usr/inclue ${CXX_STL_CPPFLAGS}"  CPPFLAGS="${ANDROID_CFLAGS} ${CXX_STL_CPPFLAGS}" \
        	LDFLAGS="${ANDROID_CFLAGS} -pie -Wl,-rpath-link=${SYSROOT}/usr/lib -L${SYSROOT}/usr/lib -L${SYSROOT}/usr/lib ${CXX_STL_LDFLAGS}" CPPLDFLAGS="${ANDROID_CFLAGS} ${CXX_STL_LDFLAGS}" \
		CXXFLAGS="${ANDROID_CFLAGS} ${CXX_STL_CPPFLAGS}" CXXLDFLAGS="${ANDROID_CFLAGS} ${CXX_STL_LDFLAGS}" \
		--with-pkgconfigdir="$PKG_CONFIG_PATH" LIBS="-lgnustl_static -lsupc++ -lcpu-features"
    } && make 
}

