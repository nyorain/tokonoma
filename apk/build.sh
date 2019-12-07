#!/bin/bash

# Expected arguments
# 1. architecture (e.g. arm64-v8a)
# 2. path to buildtools dir 
# 3. path to android platform folder to use
# 4. absolute module path
# 5. output path
# 6: tkn apk suffix

if [ $# -lt 6 ]; then
	echo "build.sh: Invalid arguments"
	exit 1
fi

arch=$1
BT=$2
PLATFORM=$3
libpath=$4
out=$5
suffix=$6

cd $out

libpath=$libpath
libfile=$(basename -- $libpath)
libname=$(echo $libfile | sed -e "s/^lib//; s/\.so$//")
pkgname=$libname
name=${libname^}

sed "s/TKN_APP/$name/g; \
	s/TKN_LIB/$libname/g; \
	s/TKN_SUFFIX/$suffix/g; \
	s/TKN_PKG/$pkgname/g" \
	AndroidManifest.xml.in > AndroidManifest.xml

baseapk=base.tkn.apk
apku=wip.tkn.unsigned.apk
apk=$pkgname.tkn.apk

# copy base apk and update manifest
cp $baseapk $apku
$BT/aapt package -u -M AndroidManifest.xml \
	-I "${PLATFORM}/android.jar" \
	-F $apku 

# make sure the module library is linked
ln -sf $libpath lib/$arch
$BT/aapt add $apku lib/$arch/$libfile

# sign apk
$BT/apksigner sign --ks keystore.jks \
	--ks-key-alias androidkey --ks-pass pass:android \
	--key-pass pass:android --out $apk \
	$apku

# adb install $apk
# adb logcat -s ny:V
