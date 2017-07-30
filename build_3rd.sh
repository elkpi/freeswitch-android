#!/bin/bash

TOPDIR=$(dirname `realpath $0`)
FS_VER=1.6.11

. env.sh

THIRD_PARTY=$TOPDIR/third_party

cd $THIRD_PARTY/opus-1.2.1 && {
    [ -f Makefile ] || {
        autoreconf -fi && ./configure --host=${HOST} CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS}"  LDFLAGS="${ANDROID_CFLAGS} -pie" \
                --prefix=${SYSROOT}/usr/ --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

#make -C $THIRD_PARTY/lua-5.1.5 linux CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE" || exit $?
make -C $THIRD_PARTY/cpu-features CC="$CC" AR="$AR rcu" CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE" SYSROOT="$SYSROOT" NDK_PATH=$NDK_PATH || exit $?

cd $THIRD_PARTY/ldns-1.7.0 && {
    [ -f Makefile ] || {
        autoreconf -fi && ./configure --host=${HOST} CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS}"  LDFLAGS="${ANDROID_CFLAGS} -pie" \
		--prefix=${SYSROOT}/usr/ --with-sysroot=${SYSROOT} --with-ssl=${SYSROOT}/usr/ --disable-dane-verify
    } && make && make install || exit $?
}

cd $THIRD_PARTY/curl-7.29.0 && {
    [ -f Makefile ] || {
        ./buildconf && CPP="$CC ${ANDROID_CFLAGS} -E" ./configure --prefix=${SYSROOT}/usr/  --host=${HOST} --target=arm-linux-androideabi --with-sysroot=${SYSROOT}  \
            CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE "  LDFLAGS="${ANDROID_CFLAGS} -pie ${CXX_STL_LDFLAGS} " --disable-dependency-tracking  --without-ca-bundle \
            --without-ca-path --enable-ipv6 --enable-http --enable-ftp --disable-file --disable-ldap --disable-ldaps --disable-rtsp --disable-proxy --disable-dict \
            --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-sspi --disable-manual 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/freetype-2.4.11 && {
    [ -f config.mk ] || {
        ./autogen.sh && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/  --host=${HOST} --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/libjpeg-turbo-1.5.2 && {
    [ -f Makefile ] || {
        autoreconf -fi && ./configure --host=${HOST} CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS}"  LDFLAGS="${ANDROID_CFLAGS} -pie" --without-simd --prefix=${SYSROOT}/usr/ 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/libogg-1.3.2 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/  --host=${HOST} --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/libpng-1.6.30 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"\
		LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/  --host=${HOST} \
            	--with-sysroot=${SYSROOT}  --enable-arm-neon=off 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/libsndfile-1.0.26 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE -gstabs "  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  \
            LDFLAGS="${ANDROID_CFLAGS} -pie" CXXFLAGS="${CXXFLAGS} -gstabs -fno-exceptions --sysroot=${SYSROOT} -I${SYSROOT}/usr/include ${CXX_STL_CPPFLAGS}"  \
            ./configure --prefix=${SYSROOT}/usr/  --host=${HOST} --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/libuuid-1.0.3 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/  \
		--host=${HOST} --with-sysroot=${SYSROOT} --with-pic
    } && make && make install || exit $?
}

cd $THIRD_PARTY/ncurses-5.9 && {
    [ -f Makefile ] || {
       	CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE -I${SYSROOT}/usr/include "  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  \
            ./configure --prefix=${SYSROOT}/usr/  --host=${HOST} --without-cxx --without-cxx-binding 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/openssl-1.0.1e && {
    [ -f Makefile ] || {
	CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE -I${SYSROOT}/usr/include "  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"\
        	ANDROID_DEV="${SYSROOT}/usr" ./Configure android-armv7 --openssldir=${SYSROOT}/usr/ssl/ --prefix=${SYSROOT}/usr/ shared no-ssl2 no-ssl3 no-comp no-hw
    } && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE -I${SYSROOT}/usr/include "  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie" \
		ANDROID_DEV="${SYSROOT}/usr" make && make install_sw || exit $?
}

cd $THIRD_PARTY/pcre-8.32 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE -I${SYSROOT}/usr/include"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib ${CXX_STL_CPPFLAGS}"  \
            LDFLAGS="${ANDROID_CFLAGS} -pie ${CXX_STL_LDFLAGS}" ./configure --prefix=${SYSROOT}/usr/ --build=x86_64-redhat-linux  \
            --host=arm  --with-sysroot=${SYSROOT} LIBS="-lgnustl_static" --enable-shared 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/speexdsp-Speex-1.2rc2 && {
    [ -f Makefile ] || {
        ./autogen.sh && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/ \
            --host=${HOST} --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/speex-1.2.0 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/  \
            --host=${HOST} --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

cd $THIRD_PARTY/sqlite-autoconf-3190300 && {
    [ -f Makefile ] || {
        autoreconf -fi && CFLAGS="${ANDROID_CFLAGS} -O3 -fPIE"  CPPFLAGS="${ANDROID_CFLAGS} -L${SYSROOT}/usr/lib"  LDFLAGS="${ANDROID_CFLAGS} -pie"  ./configure --prefix=${SYSROOT}/usr/  \
            --host=${HOST} --with-sysroot=${SYSROOT} 
    } && make && make install || exit $?
}

