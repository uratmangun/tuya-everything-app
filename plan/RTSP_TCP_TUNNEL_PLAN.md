# RTSP Camera TCP Tunnel via T5AI DevKit

## Overview

This document describes the implementation plan for creating a TCP tunnel using the T5AI DevKit to forward RTSP camera streams from a local network to a remote VPS.

## Problem Statement

- **RTSP Camera**: Located on local LAN, not accessible from internet
- **VPS**: Located on internet, cannot reach camera directly
- **T5AI DevKit**: Located on same LAN as camera, can reach both camera and VPS
- **Constraint**: T5AI has only 8MB RAM, cannot run FFmpeg locally

## Solution Architecture

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  RTSP Camera    │     │  T5AI DevKit    │     │     VPS         │
│  (Local LAN)    │ TCP │  (TCP Tunnel)   │ TCP │  (Internet)     │
│  Port 554       │◄────┤                 ├────►│  Port 8554      │
└─────────────────┘     │                 │     │                 │
                        │  1. Connect VPS │     │  FFmpeg/GStreamer│
                        │  2. Connect Cam │     │  processes stream│
                        │  3. Forward     │     │                 │
                        └─────────────────┘     └─────────────────┘
```

## How It Works

1. **T5AI initiates outbound connection** to VPS (bypasses NAT)
2. **VPS accepts** the connection on a designated port
3. **T5AI connects** to RTSP camera on local network
4. **T5AI forwards raw TCP bytes** bidirectionally between both connections
5. **VPS runs FFmpeg** to decode/process the RTSP stream

## Technical Findings from TuyaOpen Codebase

### Available TCP APIs

| Function | Description |
|----------|-------------|
| `tal_net_socket_create(PROTOCOL_TCP)` | Create TCP socket |
| `tal_net_str2addr(ip_string)` | Convert IP string to address |
| `tal_net_connect(sock_fd, ip, port)` | Connect to remote host |
| `tal_net_send(sock_fd, buffer, len)` | Send data |
| `tal_net_recv(sock_fd, buffer, len)` | Receive data |
| `tal_net_close(sock_fd)` | Close socket |

### Network Stack

- **lwIP 2.1.2** - Lightweight IP stack with TCP support
- **altcp** - Application layered TCP with proxy connect support
- Supports both WiFi and Wired connections

### Reference Examples

- `examples/protocols/tcp_client/` - TCP client implementation
- `examples/protocols/tcp_server/` - TCP server implementation
- `platform/T5AI/t5_os/ap/components/demos/net/tcp_client/` - Platform-specific demo

## Implementation Plan

### Phase 1: Core TCP Tunnel Application

Create `/home/uratmangun/CascadeProjects/TuyaOpen/apps/tcp_tunnel/`

#### Files to Create:

```
apps/tcp_tunnel/
├── CMakeLists.txt
├── Kconfig
├── app_default.config
├── config/
│   └── TUYA_T5AI_BOARD.config
├── include/
│   └── tcp_tunnel.h
└── src/
    ├── tuya_main.c
    └── tcp_tunnel.c
```

### Phase 2: Implementation Details

#### 2.1 Configuration (tcp_tunnel.h)

```c
// VPS Configuration
#define VPS_HOST        "your-vps.com"
#define VPS_PORT        8554

// RTSP Camera Configuration
#define CAMERA_IP       "192.168.1.100"
#define CAMERA_PORT     554

// WiFi Configuration
#define WIFI_SSID       "your-wifi-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

// Buffer Configuration (keep small for 8MB RAM)
#define TUNNEL_BUFFER_SIZE  4096
```

#### 2.2 Main Logic (tcp_tunnel.c)

```c
// Pseudocode for TCP tunnel implementation

typedef struct {
    int vps_sock;
    int cam_sock;
    bool running;
    uint8_t buffer[TUNNEL_BUFFER_SIZE];
} tunnel_ctx_t;

// Main tunnel task
void tcp_tunnel_task(void *args) {
    tunnel_ctx_t ctx = {0};
    
    // Step 1: Connect to VPS (outbound - bypasses NAT)
    ctx.vps_sock = tal_net_socket_create(PROTOCOL_TCP);
    TUYA_IP_ADDR_T vps_ip = tal_net_str2addr(VPS_HOST);
    if (tal_net_connect(ctx.vps_sock, vps_ip, VPS_PORT) < 0) {
        PR_ERR("Failed to connect to VPS");
        goto cleanup;
    }
    PR_NOTICE("Connected to VPS %s:%d", VPS_HOST, VPS_PORT);
    
    // Step 2: Wait for VPS "CONNECT" command (optional handshake)
    // This allows VPS to signal when it's ready
    
    // Step 3: Connect to local RTSP camera
    ctx.cam_sock = tal_net_socket_create(PROTOCOL_TCP);
    TUYA_IP_ADDR_T cam_ip = tal_net_str2addr(CAMERA_IP);
    if (tal_net_connect(ctx.cam_sock, cam_ip, CAMERA_PORT) < 0) {
        PR_ERR("Failed to connect to camera");
        goto cleanup;
    }
    PR_NOTICE("Connected to camera %s:%d", CAMERA_IP, CAMERA_PORT);
    
    // Step 4: Bidirectional forwarding loop
    ctx.running = true;
    while (ctx.running) {
        int bytes_read;
        
        // Forward: VPS → Camera (RTSP commands)
        bytes_read = tal_net_recv_nd_fd(ctx.vps_sock, ctx.buffer, 
                                        TUNNEL_BUFFER_SIZE, 10);
        if (bytes_read > 0) {
            tal_net_send(ctx.cam_sock, ctx.buffer, bytes_read);
        }
        
        // Forward: Camera → VPS (RTSP/RTP response)
        bytes_read = tal_net_recv_nd_fd(ctx.cam_sock, ctx.buffer, 
                                        TUNNEL_BUFFER_SIZE, 10);
        if (bytes_read > 0) {
            tal_net_send(ctx.vps_sock, ctx.buffer, bytes_read);
        }
        
        tal_system_sleep(1); // Yield to other tasks
    }
    
cleanup:
    if (ctx.cam_sock > 0) tal_net_close(ctx.cam_sock);
    if (ctx.vps_sock > 0) tal_net_close(ctx.vps_sock);
}
```

### Phase 3: VPS-Side Setup

#### 3.1 Simple Relay Server (Python)

```python
#!/usr/bin/env python3
# vps_rtsp_relay.py

import socket
import subprocess
import threading

VPS_PORT = 8554

def handle_tunnel(client_sock):
    """Handle incoming T5AI connection and pipe to FFmpeg"""
    print(f"T5AI connected from {client_sock.getpeername()}")
    
    # Start FFmpeg to process the RTSP stream
    ffmpeg_cmd = [
        'ffmpeg',
        '-i', 'pipe:0',  # Read from stdin
        '-c:v', 'copy',  # Copy video codec
        '-f', 'flv',     # Output format
        'rtmp://localhost/live/camera'  # Or save to file
    ]
    
    # Alternative: Save to file
    # ffmpeg_cmd = ['ffmpeg', '-i', 'pipe:0', '-c', 'copy', 'output.mp4']
    
    ffmpeg = subprocess.Popen(ffmpeg_cmd, stdin=subprocess.PIPE)
    
    try:
        while True:
            data = client_sock.recv(4096)
            if not data:
                break
            ffmpeg.stdin.write(data)
    except Exception as e:
        print(f"Error: {e}")
    finally:
        ffmpeg.stdin.close()
        ffmpeg.wait()
        client_sock.close()

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', VPS_PORT))
    server.listen(1)
    print(f"Listening on port {VPS_PORT}...")
    
    while True:
        client, addr = server.accept()
        threading.Thread(target=handle_tunnel, args=(client,)).start()

if __name__ == '__main__':
    main()
```

#### 3.2 Using socat (One-liner)

```bash
# Listen on port 8554 and pipe to FFmpeg
socat TCP-LISTEN:8554,reuseaddr,fork \
    EXEC:'ffmpeg -i - -c copy -f flv rtmp://localhost/live/stream'
```

## Memory Considerations

| Component | Estimated RAM Usage |
|-----------|---------------------|
| TCP socket (VPS) | ~2-4 KB |
| TCP socket (Camera) | ~2-4 KB |
| Buffer (4KB) | 4 KB |
| lwIP stack overhead | ~20-40 KB |
| **Total** | **~30-50 KB** |

With 8MB RAM, this should be very comfortable.

## Build & Flash Instructions

```bash
# Navigate to TuyaOpen directory
cd /home/uratmangun/CascadeProjects/TuyaOpen

# Build the tunnel application
tos build apps/tcp_tunnel

# Flash to T5AI
tos flash apps/tcp_tunnel -p /dev/ttyUSB0

# Monitor output
tos monitor -p /dev/ttyUSB0
```

## Questions Before Implementation

1. **RTSP Camera URL**: What is your camera's RTSP URL format?
   - Example: `rtsp://192.168.1.100:554/stream1`

2. **VPS Port**: What port should VPS listen on?
   - Suggested: `8554`

3. **Reconnection**: Should T5AI automatically reconnect on failure?
   - Recommended: Yes, with exponential backoff

4. **Security**: Do you need TLS encryption for the tunnel?
   - Optional: T5AI supports TLS via `tuya_tls_transporter_connect`

5. **Authentication**: Should there be a simple handshake/password?
   - Optional: Can add simple token verification

## Alternative: Tuya Cloud Option

If you prefer using Tuya's cloud infrastructure:

- Use `apps/tuya_cloud/camera_demo/` as reference
- Requires Tuya Cloud account and product configuration
- Video streams through Tuya servers
- Access via Tuya Smart Life app

However, the TCP tunnel approach gives you:
- Direct control
- Lower latency
- No cloud dependency
- Custom processing on VPS

## Next Steps

1. Confirm configuration details (camera IP, VPS host, WiFi credentials)
2. Create the TCP tunnel application
3. Create VPS relay script
4. Test connection
5. Add error handling and reconnection logic
6. Optional: Add TLS encryption

---

*Document created: 2025-12-17*
*Status: Planning Phase*
