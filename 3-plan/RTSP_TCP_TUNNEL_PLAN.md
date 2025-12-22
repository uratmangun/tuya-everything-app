# RTSP Camera TCP Tunnel via T5AI DevKit - Implementation Guide

## Overview

This document describes the implemented TCP tunnel for forwarding RTSP camera streams from a local network to a remote VPS through the T5AI DevKit.

## Architecture

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  RTSP Camera    │     │  T5AI DevKit    │     │     VPS         │
│  (Local LAN)    │ TCP │  (TCP Tunnel)   │ TCP │  YOUR_VPS_IP    │
│  192.168.x.x    │◄────┤                 ├────►│                 │
│  Port 554       │     │                 │     │  Port 8554      │
└─────────────────┘     │                 │     │  (RTSP Relay)   │
                        │                 │     │                 │
                        │  Control: :5000 │────►│  Port 5000      │
                        │  (Web App TCP)  │     │  (Web App)      │
                        └─────────────────┘     │                 │
                                                │  Port 8555      │
                                                │  (Local RTSP)   │
                                                │                 │
                                                │  FFmpeg/VLC     │
                                                │  processes      │
                                                └─────────────────┘
```

## How It Works

1. **DevKit connects to VPS** on port 5000 (existing web app connection)
2. **VPS sends command** `rtsp start` via web app
3. **DevKit connects to local camera** on port 554 (RTSP)
4. **DevKit connects to VPS** on port 8554 (RTSP tunnel)
5. **DevKit forwards raw TCP bytes** bidirectionally between camera and VPS
6. **VPS runs FFmpeg** or other tools to process the stream locally

## Files Implemented

### DevKit Firmware

| File | Purpose |
|------|---------|
| `src/rtsp_tunnel.h` | RTSP tunnel API header |
| `src/rtsp_tunnel.c` | Bidirectional TCP tunnel implementation |
| `src/tuya_config.h` | RTSP configuration defines |
| `src/tuya_main.c` | Integrated RTSP tunnel commands |
| `build_with_env.fish` | Updated build script with RTSP flags |
| `.env` / `.env.example` | Configuration file |

### VPS

| File | Purpose |
|------|---------|
| `webapp/rtsp_relay.js` | Node.js RTSP relay server |

## Configuration

### DevKit (.env file)

```bash
# RTSP Camera Configuration
export RTSP_CAMERA_HOST="192.168.1.100"   # Your camera's IP
export RTSP_CAMERA_PORT="554"              # Usually 554

# VPS Tunnel Endpoint
export RTSP_VPS_HOST="YOUR_VPS_IP"         # Your VPS IP
export RTSP_VPS_PORT="8554"                # RTSP relay port

# Auto-start: "1" to start on boot, "0" for manual
export RTSP_TUNNEL_ENABLED="0"
```

### VPS Environment

```bash
# Optional: customize ports
export RTSP_DEVKIT_PORT=8554    # DevKit connects here
export RTSP_LOCAL_PORT=8555     # Local RTSP access
export RTSP_STATUS_PORT=8556    # Status API
```

## Deployment Guide

### Step 1: Configure DevKit

1. Edit `/home/uratmangun/CascadeProjects/TuyaOpen/apps/tuya_cloud/object_detection/.env`:
   ```bash
   # Set your actual camera IP
   export RTSP_CAMERA_HOST="YOUR_CAMERA_IP"
   ```

2. Build and flash:
   ```bash
   cd /home/uratmangun/CascadeProjects/TuyaOpen/apps/tuya_cloud/object_detection
   ./build_with_env.fish flash
   ```

### Step 2: Deploy VPS RTSP Relay

1. SSH to your VPS:
   ```bash
   ssh ubuntu@100.89.99.57
   ```

2. Copy the relay script or create it:
   ```bash
   cd /path/to/webapp
   node rtsp_relay.js
   ```

   Or run with PM2:
   ```bash
   pm2 start rtsp_relay.js --name rtsp-relay
   ```

3. Open firewall port 8554:
   ```bash
   sudo ufw allow 8554/tcp
   ```

### Step 3: Start the Tunnel

**Option A: Via Web App**
1. Open the web UI
2. Send command: `rtsp start`
3. Check status: `rtsp status`

**Option B: Auto-start**
1. Set `RTSP_TUNNEL_ENABLED="1"` in .env
2. Rebuild and flash firmware

### Step 4: Access the Stream

On the VPS, use FFmpeg or VLC:

```bash
# Play the stream
ffplay tcp://localhost:8555

# Save to file
ffmpeg -i tcp://localhost:8555 -c copy output.mp4

# Re-stream as HLS
ffmpeg -i tcp://localhost:8555 -c:v libx264 -f hls /var/www/html/stream.m3u8

# View with VLC
vlc tcp://localhost:8555
```

## Commands Reference

Commands sent via the web app TCP connection:

| Command | Response | Description |
|---------|----------|-------------|
| `rtsp start` | `ok:rtsp_tunnel_started cam=IP:PORT vps=IP:PORT` | Start tunnel |
| `rtsp stop` | `ok:rtsp_tunnel_stopped` | Stop tunnel |
| `rtsp status` | JSON with stats | Get tunnel status |

### Status Response Example

```json
{
  "rtsp_active": true,
  "connected": true,
  "bytes_to_vps": 1234567,
  "bytes_to_camera": 12345
}
```

## VPS Status API

The RTSP relay server provides a status API:

```bash
# Check status
curl http://localhost:8556/status

# Get help
curl http://localhost:8556/help
```

## Memory Usage

| Component | RAM Usage |
|-----------|-----------|
| RTSP tunnel task | ~4 KB stack |
| TCP socket (VPS) | ~2-4 KB |
| TCP socket (Camera) | ~2-4 KB |
| Buffer (4KB) | 4 KB |
| **Total** | **~14-16 KB** |

With 8MB RAM, this is very comfortable.

## Troubleshooting

### Tunnel won't connect to camera
- Verify camera IP: `ping 192.168.x.x` from a device on same LAN
- Check camera RTSP port: usually 554
- Ensure DevKit is on same WiFi network as camera

### Tunnel won't connect to VPS
- Check VPS firewall: `sudo ufw status`
- Verify port 8554 is open
- Check if relay server is running: `curl localhost:8556/status`

### No video on FFmpeg
- RTSP uses interleaved mode over TCP
- Make sure to wait for RTSP SETUP/PLAY handshake
- Try: `ffplay -rtsp_transport tcp rtsp://localhost:8555/stream`

### High latency
- Increase buffer size in rtsp_tunnel.c if needed
- Consider using UDP instead of TCP for RTP

## Security Notes

1. The tunnel uses raw TCP without encryption
2. Consider adding TLS if security is important
3. VPS relay only listens on localhost for local access
4. DevKit port is open publicly - consider firewall rules

---

*Document updated: 2025-12-18*
*Status: Implemented*
