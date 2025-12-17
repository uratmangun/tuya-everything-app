# RTSP Person Detector Android App

Android app that captures RTSP video feed, sends frames to Tuya Cloud AI for person detection, and triggers your T5AI DevKit to play an alert.

## Architecture

```
┌─────────────────┐         ┌─────────────────┐         ┌─────────────────┐
│   RTSP Camera   │ ──────► │  Android Phone  │ ──────► │  Tuya Cloud AI  │
│   (Local)       │  RTSP   │  (This App)     │  HTTPS  │  Person Detect  │
└─────────────────┘         └────────┬────────┘         └────────┬────────┘
                                     │                           │
                                     │ If person detected        │
                                     ▼                           │
                            ┌─────────────────┐                  │
                            │  T5AI DevKit    │ ◄────────────────┘
                            │  (Play Alert)   │   Tuya Cloud Command
                            └─────────────────┘
```

## Prerequisites

1. **Tuya Cloud API Credentials**
   - Go to [iot.tuya.com](https://iot.tuya.com) → Cloud → Development
   - Create Cloud Project, enable "AI Image Recognition"
   - Copy Access ID and Access Secret

2. **Device ID** - Your T5AI DevKit device ID from Tuya IoT Platform

3. **RTSP Camera URL** - Already configured: `rtsp://admin:Admin1234@192.168.18.34:554/live/ch00_0`

## Quick Install (Pre-built APK)

```bash
# Build and install via ADB
cd android
./build.sh
```

## Build from Source

### Option 1: Android Studio (Recommended)
1. Open this folder in Android Studio
2. Edit `Config.kt` with your credentials
3. Click Run

### Option 2: Command Line
```bash
# Set Android SDK path
export ANDROID_HOME=~/Android/Sdk

# Build debug APK
./gradlew assembleDebug

# Install to connected device
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Configuration

Edit `app/src/main/java/com/tuya/persondetector/Config.kt`:

```kotlin
object Config {
    var accessId = "your_access_id"        // From Tuya Cloud Project
    var accessSecret = "your_access_secret" // From Tuya Cloud Project
    var apiEndpoint = "https://openapi.tuyaus.com"  // US/EU/CN
    var deviceId = "your_device_id"        // T5AI DevKit ID
    var rtspUrl = "rtsp://admin:Admin1234@192.168.18.34:554/live/ch00_0"
}
```

## Usage

1. Launch the app
2. Enter your Tuya credentials (or edit Config.kt before building)
3. Tap "Start"
4. The app will:
   - Connect to RTSP camera
   - Capture frames every 3 seconds
   - Send to Tuya Cloud AI for person detection
   - Trigger your devkit when person detected

## Requirements

- Android 7.0+ (API 24)
- Same network as RTSP camera
- Internet access for Tuya Cloud API
