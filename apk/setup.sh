#!/bin/bash

# Expected arguments
# 1. architecture (e.g. arm64-v8a)
# 2. path to buildtools dir 
# 3. path to android platform folder to use
# 4. path to AndroidManifest.xml.in
# 5. path to output folder
# 6. path of font file to use as 'font.ttf'

if [ $# -lt 6 ]; then
	echo "setup.sh: Invalid arguments"
	exit 1
fi

arch=$1
BT=$2
PLATFORM=$3
manifestInput=$4
out=$5
font=$6

NDK='/opt/android-ndk'

cd $out

mkdir -p lib/$arch
mkdir -p assets

# copy assets
cp $font assets/font.ttf

# the first argument must be the full path of AndroidManifest.xml.in
# we copy it to the build dir to allow using it in build.sh
cp $manifestInput ./AndroidManifest.xml.in

# link libraries to the lib/$arch dir since they
# have to be there when packaged with aapt
ln -sf \
	$NDK/sources/cxx-stl/llvm-libc++/libs/$arch/libc++_shared.so \
	lib/$arch

d="$(pwd)/.."
b="$d/../.."
ln -sf \
	$d/src/tkn/libtinygltf.so \
	$d/src/tkn/libtkn.so \
	$d/subprojects/swa/libswa.so \
	$d/subprojects/vpp/src/vpp/libvpp.so \
	$d/subprojects/dlg/libdlg.so \
	$d/subprojects/katachi/libkatachi.so \
	$d/subprojects/rvg/src/rvg/librvg.so \
	$d/subprojects/libpng-1.6.37/libpng16.so \
	$d/subprojects/vkpp/libvkpp.so \
	$d/subprojects/vui/src/vui/libvui.so \
	$d/subprojects/cubeb/libcubeb.so \
	$d/subprojects/b2d/Box2D/libBox2D.so \
	$b/subprojects/steamaudio_api/lib/Android/libphonon.so \
	$NDK/sources/third_party/vulkan/src/build-android/jniLibs/$arch/libVkLayer_khronos_validation.so \
	lib/$arch

# generate dummy apk
apk=base.tkn.apk
rm -f $apk

cp AndroidManifest.xml.in AndroidManifest.xml
$BT/aapt package -f -M AndroidManifest.xml \
	-I "${PLATFORM}/android.jar" \
	-F $apk 

$BT/aapt add $apk \
	lib/$arch/libc++_shared.so \
	lib/$arch/libswa.so \
	lib/$arch/libcubeb.so \
	lib/$arch/libdlg.so
	# lib/$arch/libvpp.so \
	# lib/$arch/libtinygltf.so \
	# lib/$arch/libtkn.so \
	# lib/$arch/libkatachi.so \
	# lib/$arch/librvg.so \
	# lib/$arch/libpng16.so \
	# lib/$arch/libvkpp.so \
	# lib/$arch/libvui.so \
	# lib/$arch/libBox2D.so \
	# lib/$arch/libphonon.so \
	# lib/$arch/libVkLayer_khronos_validation.so
	# assets/font.ttf \
	# assets/test.ogg \
	# assets/model.glb \
	# assets/brdflut.ktx \
	# assets/convolution.ktx \
	# assets/irradiance.ktx
