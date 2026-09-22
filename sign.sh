#!/usr/bin/env bash
set -e

# Reconfigure the Xcode project so version metadata generated from CMake is current.
cmake -S . -B build-xcode -G Xcode \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build-xcode --config Release --target \
  PitchNet \
  PitchNetPlugin_VST3 \
  PitchNetPlugin_AU \
  PitchNetPlugin_AAX

verify_universal() {
  lipo -info "$1"
  lipo "$1" -verify_arch arm64 x86_64
}

verify_universal build-xcode/PitchNet_artefacts/Release/PitchNet.app/Contents/MacOS/PitchNet
verify_universal build-xcode/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3/Contents/MacOS/PitchNet
verify_universal build-xcode/PitchNetPlugin_artefacts/Release/AU/PitchNet.component/Contents/MacOS/PitchNet
verify_universal build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin/Contents/MacOS/PitchNet
verify_universal build-xcode/PitchNet_artefacts/Release/PitchNet.app/Contents/Frameworks/libonnxruntime.1.19.2.dylib
verify_universal build-xcode/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3/Contents/Frameworks/libonnxruntime.1.19.2.dylib
verify_universal build-xcode/PitchNetPlugin_artefacts/Release/AU/PitchNet.component/Contents/Frameworks/libonnxruntime.1.19.2.dylib
verify_universal build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin/Contents/Frameworks/libonnxruntime.1.19.2.dylib

codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNet_artefacts/Release/PitchNet.app/Contents/MacOS/PitchNet
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNet_artefacts/Release/PitchNet.app/Contents/Frameworks/libonnxruntime.1.19.2.dylib
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3/Contents/MacOS/PitchNet
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3/Contents/Frameworks/libonnxruntime.1.19.2.dylib
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/AU/PitchNet.component/Contents/MacOS/PitchNet
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/AU/PitchNet.component/Contents/Frameworks/libonnxruntime.1.19.2.dylib
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin/Contents/MacOS/PitchNet
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin/Contents/Frameworks/libonnxruntime.1.19.2.dylib
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNet_artefacts/Release/PitchNet.app
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/AU/PitchNet.component
codesign --sign "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" -f -o runtime --timestamp -v build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin
/Applications/PACEAntiPiracy/Eden/Fusion/Current/bin/wraptool sign --verbose --account 200gaga --password 52Guzheng --signid "Developer ID Application: SESSION LOOPS, INC. (29DGL5KQ37)" --wcguid 70091B10-74DF-11F1-B955-00505692AD3E --in build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin --out build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin
/Applications/PACEAntiPiracy/Eden/Fusion/Current/bin/wraptool verify --verbose --in build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin
codesign -vvv --deep --strict build-xcode/PitchNet_artefacts/Release/PitchNet.app
codesign -vvv --deep --strict build-xcode/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3
codesign -vvv --deep --strict build-xcode/PitchNetPlugin_artefacts/Release/AU/PitchNet.component
codesign -vvv --deep --strict build-xcode/PitchNetPlugin_artefacts/Release/AAX/PitchNet.aaxplugin

packagesbuild -v packaging/macos/PitchNet.pkgproj
productsign -s  "Developer ID Installer: SESSION LOOPS, INC. (29DGL5KQ37)" build/PitchNet.pkg build/PitchNet_Installer.pkg
pkgutil --check-signature build/PitchNet_Installer.pkg
xcrun notarytool submit build/PitchNet_Installer.pkg --keychain-profile "notarytool-password" --wait
xcrun stapler staple build/PitchNet_Installer.pkg
# xcrun notarytool log 7f629a28-1341-4ba5-abc8-a4295249df51 --keychain-profile "notarytool-password"
