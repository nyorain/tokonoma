#!/bin/bash

# expected environment:
# $SDK: full path of android sdk root
# $BT: path to sdk build tools folder
# $PLATFORM: path to sdk android platform folder to use
arch=arm64-v8a

if [ $# -lt 2 ]; then
	echo "Usage: setup.sh <AndroidManifest.xml.in path> <output path>"
	exit 1
fi

out=$2
cd $out

mkdir -p lib/$arch

# the first argument must be the full path of AndroidManifest.xml.in
# we copy it to the build dir to allow using it in build.sh
cp $1 .

# link libraries to the lib/$arch dir since they
# have to be there when packaged with aapt
ln -sf \
	/opt/android-ndk/sources/cxx-stl/llvm-libc++/libs/$arch/libc++_shared.so \
	lib/$arch

d="$(pwd)/.."
ln -sf \
	$d/src/tkn/libtinygltf.so \
	$d/src/tkn/libtkn.so \
	$d/subprojects/ny/src/ny/libny.so \
	$d/subprojects/vpp/src/vpp/libvpp.so \
	$d/subprojects/dlg/libdlg.so \
	$d/subprojects/katachi/libkatachi.so \
	$d/subprojects/rvg/src/rvg/librvg.so \
	$d/subprojects/libpng-1.6.35/libpng16.so.16 \
	$d/subprojects/vkpp/libvkpp.so \
	$d/subprojects/vui/src/vui/libvui.so \
	lib/$arch

# generate dummy apk
apk=base.tkn.apk
rm -f $apk

sed "s/%appname%/DUMMY/g; \
	s/%libname%/DUMMY/g; \
	s/%pkgname%/DUMMY/g" \
	AndroidManifest.xml.in > AndroidManifest.xml

"${BT}/aapt" package -f -M AndroidManifest.xml \
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
