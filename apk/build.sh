#!/bin/bash

# expected environment:
# $SDK: full path of android sdk root
# $BT: path to sdk build tools folder
# $PLATFORM: path to sdk android platform folder to use
arch=arm64-v8a

if [ $# -lt 2 ]; then
	echo "Usage: build.sh <absolute module path> <output path>"
	exit 1
fi

libpath=$1
out=$2
cd $out

libfile=$(basename -- $libpath)
libname=$(echo $libfile | sed -e "s/^lib//; s/\.so$//")
pkgname=$libname
name=${libname^}

sed "s/%appname%/$name/g; \
	s/%libname%/$libname/g; \
	s/%pkgname%/$pkgname/g" \
	AndroidManifest.xml.in > AndroidManifest.xml

baseapk=base.tkn.apk
apku=wip.tkn.unsigned.apk
apk=$pkgname.tkn.apk

# copy base apk and update manifest
cp $baseapk $apku
"${BT}/aapt" package -u -M AndroidManifest.xml \
	-I "${PLATFORM}/android.jar" \
	-F $apku 

# make sure the module library is linked
ln -sf $libpath lib/$arch
$BT/aapt add $apku lib/$arch/$libfile

# sign apk
"${BT}/apksigner" sign --ks keystore.jks \
	--ks-key-alias androidkey --ks-pass pass:android \
	--key-pass pass:android --out $apk \
	$apku

# adb install $apk
# adb logcat -s ny:V
