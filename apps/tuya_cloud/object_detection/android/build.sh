#!/bin/bash
# Build script for Person Detector Android App

set -e

echo "🔧 Person Detector Android App Builder"
echo "======================================="

# Check Java version
JAVA_VER=$(java -version 2>&1 | head -1 | cut -d'"' -f2 | cut -d'.' -f1)
if [ "$JAVA_VER" -lt 17 ]; then
    echo "⚠️  Java 17+ required. Current: Java $JAVA_VER"
    echo ""
    echo "Install Java 17:"
    echo "  sudo apt install openjdk-17-jdk"
    echo "  export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64"
    echo ""
    echo "Or open this project in Android Studio instead."
    exit 1
fi

# Set Android SDK if not set
if [ -z "$ANDROID_HOME" ]; then
    if [ -d "$HOME/Android/Sdk" ]; then
        export ANDROID_HOME="$HOME/Android/Sdk"
    else
        echo "⚠️  ANDROID_HOME not set and ~/Android/Sdk not found"
        echo "Install Android SDK or open in Android Studio"
        exit 1
    fi
fi

echo "📱 ANDROID_HOME: $ANDROID_HOME"
echo "☕ JAVA_HOME: $JAVA_HOME"

# Build
echo ""
echo "🔨 Building APK..."
./gradlew assembleDebug

APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
if [ -f "$APK_PATH" ]; then
    echo ""
    echo "✅ Build successful!"
    echo "📦 APK: $APK_PATH"
    
    # Check for connected device
    if adb devices | grep -q "device$"; then
        echo ""
        echo "📲 Installing to device..."
        adb install -r "$APK_PATH"
        echo "✅ Installed! Launch 'Person Detector' on your phone."
    else
        echo ""
        echo "No device connected. Install manually:"
        echo "  adb install -r $APK_PATH"
    fi
else
    echo "❌ Build failed"
    exit 1
fi
