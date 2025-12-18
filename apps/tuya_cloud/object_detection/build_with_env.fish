#!/usr/bin/env fish
# build_with_env.fish - Build switch_demo with credentials from environment or .env file
#
# Usage:
#   ./build_with_env.fish              # Uses .env file or environment variables
#   ./build_with_env.fish flash        # Build and flash
#   ./build_with_env.fish clean        # Clean build

set -l SCRIPT_DIR (dirname (status -f))
set -l PROJECT_ROOT (realpath "$SCRIPT_DIR/../../..")

# Load .env file if it exists
if test -f "$SCRIPT_DIR/.env"
    echo "Loading credentials from .env file..."
    source "$SCRIPT_DIR/.env"
end

# Check if credentials are set
if test -z "$TUYA_PRODUCT_ID"
    echo "ERROR: TUYA_PRODUCT_ID not set"
    echo "Please set environment variable or create .env file from .env.example"
    exit 1
end

if test -z "$TUYA_OPENSDK_UUID"
    echo "ERROR: TUYA_OPENSDK_UUID not set"
    echo "Please set environment variable or create .env file from .env.example"
    exit 1
end

if test -z "$TUYA_OPENSDK_AUTHKEY"
    echo "ERROR: TUYA_OPENSDK_AUTHKEY not set"
    echo "Please set environment variable or create .env file from .env.example"
    exit 1
end

echo "Building with credentials:"
echo "  TUYA_PRODUCT_ID: $TUYA_PRODUCT_ID"
echo "  TUYA_OPENSDK_UUID: $TUYA_OPENSDK_UUID"
echo "  TUYA_OPENSDK_AUTHKEY: [hidden]"

# Create a temporary config override file
set -l TEMP_CONFIG "$SCRIPT_DIR/.build_credentials.config"
echo "# Auto-generated credentials config - DO NOT COMMIT" > $TEMP_CONFIG
echo "CONFIG_TUYA_PRODUCT_ID=\"$TUYA_PRODUCT_ID\"" >> $TEMP_CONFIG

# Set compile definitions for C preprocessor
# Tuya credentials
set -l BUILD_DEFS "-DTUYA_PRODUCT_ID=\\\"$TUYA_PRODUCT_ID\\\" -DTUYA_OPENSDK_UUID=\\\"$TUYA_OPENSDK_UUID\\\" -DTUYA_OPENSDK_AUTHKEY=\\\"$TUYA_OPENSDK_AUTHKEY\\\""

# TCP Server configuration
if test -n "$TCP_SERVER_HOST"
    set BUILD_DEFS "$BUILD_DEFS -DTCP_SERVER_HOST=\\\"$TCP_SERVER_HOST\\\""
end
if test -n "$TCP_SERVER_PORT"
    set BUILD_DEFS "$BUILD_DEFS -DTCP_SERVER_PORT=$TCP_SERVER_PORT"
end
if test -n "$TCP_AUTH_TOKEN"
    set BUILD_DEFS "$BUILD_DEFS -DTCP_AUTH_TOKEN=\\\"$TCP_AUTH_TOKEN\\\""
end

# RTSP Tunnel configuration
if test -n "$RTSP_CAMERA_HOST"
    set BUILD_DEFS "$BUILD_DEFS -DRTSP_CAMERA_HOST=\\\"$RTSP_CAMERA_HOST\\\""
    echo "  RTSP_CAMERA: $RTSP_CAMERA_HOST:$RTSP_CAMERA_PORT"
end
if test -n "$RTSP_CAMERA_PORT"
    set BUILD_DEFS "$BUILD_DEFS -DRTSP_CAMERA_PORT=$RTSP_CAMERA_PORT"
end
if test -n "$RTSP_VPS_HOST"
    set BUILD_DEFS "$BUILD_DEFS -DRTSP_VPS_HOST=\\\"$RTSP_VPS_HOST\\\""
    echo "  RTSP_VPS: $RTSP_VPS_HOST:$RTSP_VPS_PORT"
end
if test -n "$RTSP_VPS_PORT"
    set BUILD_DEFS "$BUILD_DEFS -DRTSP_VPS_PORT=$RTSP_VPS_PORT"
end
if test "$RTSP_TUNNEL_ENABLED" = "1"
    set BUILD_DEFS "$BUILD_DEFS -DRTSP_TUNNEL_ENABLED=1"
    echo "  RTSP_TUNNEL: AUTO-START ENABLED"
else
    echo "  RTSP_TUNNEL: Manual start (send 'rtsp start' via web app)"
end

set -x CFLAGS "$CFLAGS $BUILD_DEFS"
set -x CXXFLAGS "$CXXFLAGS $BUILD_DEFS"

# Execute build command
cd $SCRIPT_DIR

set -l ACTION $argv[1]

switch $ACTION
    case clean
        echo "Cleaning..."
        python "$PROJECT_ROOT/tos.py" clean
    case flash
        echo "Building and flashing..."
        python "$PROJECT_ROOT/tos.py" build
        if test $status -eq 0
            python "$PROJECT_ROOT/tos.py" flash
        end
    case '*'
        echo "Building..."
        python "$PROJECT_ROOT/tos.py" build
end

# Clean up temp file
rm -f $TEMP_CONFIG
