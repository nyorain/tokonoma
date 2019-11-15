#!/bin/bash

if [ $# -ne 2 ]; then
	echo "Usage: build.sh <name> <module path>"
	exit
fi

name=$1
libpath=$2
pkgname=${name,,}

libfile=$(basename -- $libpath)
libname=$(echo $libfile | sed -e "s/^lib//; s/\.so$//")

sed "s/%appname%/$name/g; \
	s/%libname%/$libname/g; \
	s/%pkgname%/$pkgname/g" \
	AndroidManifest.xml.in > AndroidManifest.xml

apku=wip.tkn.unsigned.apk
apk=apk/$pkgname.tkn.apk

rm -f $apku

"${BT}/aapt" package -f -M AndroidManifest.xml \
	-I "${PLATFORM}/android.jar" \
	-F $apku 

cp /opt/android-ndk/sources/cxx-stl/llvm-libc++/libs/arm64-v8a/libc++_shared.so lib/arm64-v8a/
cp $libpath \
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
	lib/arm64-v8a

$BT/aapt add $apku \
	lib/arm64-v8a/$libfile \
	lib/arm64-v8a/libc++_shared.so \
	lib/arm64-v8a/libtinygltf.so \
	lib/arm64-v8a/libtkn.so \
	lib/arm64-v8a/libny.so \
	lib/arm64-v8a/libvpp.so \
	lib/arm64-v8a/libdlg.so \
	lib/arm64-v8a/libkatachi.so \
	lib/arm64-v8a/librvg.so \
	lib/arm64-v8a/libpng16.so.16 \
	lib/arm64-v8a/libvkpp.so \
	lib/arm64-v8a/libvui.so

"${BT}/apksigner" sign --ks keystore.jks 
--ks-key-alias androidkey --ks-pass pass:android \
	--key-pass pass:android --out $apk \
	$apku

adb install $apk
logcat -s ny:V
