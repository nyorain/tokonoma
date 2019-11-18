# APK builder

Some scripts and meson utility to directly generate apk's for the individual
projects. Cross compiling tkn and its projects for android is not (and won't be)
supported on windows since it's complicated enough on linux.
Tutorial to build apks for android:

## Setting up cross compilation

There is a basic meson cross compilation file in `android_cross.txt`.
It expectes the android ndk (version r20) to be installed to `/opt/android-ndk`
and builds for armv8 (i.e. 64-bit) processors.

```
meson build/android --cross-file=apk/android_cross.txt -Dvkpp:regen=true
```

To will have to regenerate the vkpp api with a vk.xml version
that is compatible to what is provided in your android-ndk version.
Just paste it to subprojects/vkpp/vk.xml.
Compilation is not supported for all projects. See `meson.build` in this
directory for a list of apks that will be built.

## Building APKs

For projects that support being compiled into an apk, custom targets
will be created (see `meson.build`). But you apk building scripts
expect a set of environment variables you have to set before building:

- $SDK: full path of android sdk root
- $BT: path to sdk build tools folder
- $PLATFORM: path to sdk android platform folder to use

For instance:

```
export SDK=/opt/android-sdk
export BT=${SDK}/build-tools/25.0.2
export PLATFORM="${SDK}/platforms/android-24
```

I guess the platform version should match the version specified in
`AndroidManifest.xml.in` and the `android_cross.txt` file, not
sure what happens otherwise. The build scripts are furthermore
hardcoded for arm64-v8a architecture at the moment (64 bit armv8).

Furthermore, apks have to be signed. This will be done by the `build.sh`
script as well but it expects a file `keystore.jsk` in the `apk` directory
of the build folder. You can just generate a dummy key like this

```
keytool -genkeypair -keystore keystore.jks -alias androidkey \
    -validity 10000 -keyalg RSA -keysize 2048 \
    -storepass android -keypass android
```

and leave the fields empty. **Don't do it like this for apps you intend
to publish or distribute, read up on how to handle keys and apk signing!**

You can then simply run something like `ninja apk/rays.tkn.apk` to
build the apk for the respective tkn application.
To compile, install and monitor logs:

```
ninja apk/rays.tkn.apk && adb install apk/rays.tkn.apk && adb logcat -s ny:V
```
