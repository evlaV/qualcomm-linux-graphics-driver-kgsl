#!/bin/bash

set -e

rm -rf install
INSTALLDIR=install
TARGETLIBDIR=/usr/lib/adreno
LIBDIR=$INSTALLDIR/$TARGETLIBDIR
VULKANDIR=$INSTALLDIR/usr/share/vulkan

mkdir -p $INSTALLDIR
mkdir -p $LIBDIR
mkdir -p $VULKANDIR/implicit_layer.d/
mkdir -p $VULKANDIR/icd.d/

aarch64-linux-gnu-gcc libgbm.c -shared -o $LIBDIR/libgbM.so
aarch64-linux-gnu-gcc libdmabufheap.c -shared -o $LIBDIR/libdmabufheap.so.0
aarch64-linux-gnu-gcc libsync.c -shared -o $LIBDIR/libsync.so.0
aarch64-linux-gnu-gcc liblog.c -shared -o $LIBDIR/liblog.so.0

aarch64-linux-gnu-gcc -DVK_USE_PLATFORM_XCB_KHR -DVK_USE_PLATFORM_XLIB_KHR -fvisibility=hidden -Wall wsi.c -lxcb -lxcb-dri3 -lxcb-present -lxcb-sync -lxshmfence -lX11-xcb -lvulkan -o $LIBDIR/wsi_xcb.so -shared -fPIC

WAYLAND_PROTOCOLS=/usr/share/wayland-protocols/unstable
DMABUF_PROTOCOL=$WAYLAND_PROTOCOLS/linux-dmabuf/linux-dmabuf-unstable-v1.xml
SYNC_PROTOCOL=$WAYLAND_PROTOCOLS/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml
wayland-scanner private-code $DMABUF_PROTOCOL linux-dmabuf-unstable-v1-protocol.c
wayland-scanner client-header $DMABUF_PROTOCOL linux-dmabuf-unstable-v1-client-protocol.h
wayland-scanner private-code $SYNC_PROTOCOL linux-explicit-synchronization-unstable-v1-protocol.c
wayland-scanner client-header $SYNC_PROTOCOL linux-explicit-synchronization-unstable-v1-client-protocol.h
aarch64-linux-gnu-gcc -DVK_USE_PLATFORM_WAYLAND_KHR -fvisibility=hidden -Wall wsi.c -lwayland-client -lvulkan linux-dmabuf-unstable-v1-protocol.c linux-explicit-synchronization-unstable-v1-protocol.c -o $LIBDIR/wsi_wayland.so -shared -fPIC

patchelf --set-rpath $TARGETLIBDIR libgsl.so
patchelf --set-rpath $TARGETLIBDIR libcutils.so.0
patchelf --set-rpath $TARGETLIBDIR libbase.so.0
patchelf --set-rpath $TARGETLIBDIR libadreno_utils.so
cp -v *.so libcutils.so.0 $LIBDIR/

cp -v VkLayer_WSI_*.json $VULKANDIR/implicit_layer.d/
cp -v adreno_icd.json $VULKANDIR/icd.d/

pushd install
tar -cf ../install.tar *
popd
gzip -f install.tar
ls -al install.tar.gz

#tar xf install.tar.gz -C /

