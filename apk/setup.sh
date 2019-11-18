#!/bin/bash

arch=arm64-v8a

mkdir -p lib/$arch
mkdir apk

# link libraries to the lib/$arch dir since they
# have to be there when packaged with aapt
ln -s \
	../src/tkn/libtinygltf.so \
	../src/tkn/libtkn.so \
	../subprojects/ny/src/ny/libny.so \
	../subprojects/vpp/src/vpp/libvpp.so \
	../subprojects/dlg/libdlg.so \
	../subprojects/katachi/libkatachi.so \
	../subprojects/rvg/src/rvg/librvg.so \
	../subprojects/libpng-1.6.35/libpng16.so.16 \
	../subprojects/vkpp/libvkpp.so \
	../subprojects/vui/src/vui/libvui.so \
	lib/$arch

# generate dummy apk
apk=base.tkn.apk
"${BT}/aapt" package -f -M AndroidManifest.xml.in \
	-I "${PLATFORM}/android.jar" \
	-F $apk 

$BT/aapt add $apk \
	lib/$arch/libc++_shared.so \
	lib/$arch/libtinygltf.so \
	lib/$arch/libtkn.so \
	lib/$arch/libny.so \
	lib/$arch/libvpp.so \
	lib/$arch/libdlg.so \
	lib/$arch/libkatachi.so \
	lib/$arch/librvg.so \
	lib/$arch/libpng16.so.16 \
	lib/$arch/libvkpp.so \
	lib/$arch/libvui.so
