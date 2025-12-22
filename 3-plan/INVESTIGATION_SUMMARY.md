# T5AI DevKit Investigation Summary

## Date: 2025-12-17

## Questions Investigated

### 1. Can we install Dropbear (SSH) on T5AI-CORE?

**Answer: No**

- T5AI runs **TuyaOS (RTOS)**, not Linux
- RTOS doesn't support installing standard Linux packages
- No `apt`, `opkg`, or similar package managers
- The build system is custom (`tos` CLI tool)

### 2. Is there any reverse SSH capability?

**Answer: No built-in, but possible to build**

- No built-in reverse shell or SSH functionality
- TCP client/server capabilities exist via lwIP stack
- Could implement custom reverse connection using TCP APIs
- Audio debug module demonstrates outbound TCP connections are possible

### 3. Can we reverse proxy RTSP camera through T5AI?

**Answer: YES - This is feasible!**

See: [RTSP_TCP_TUNNEL_PLAN.md](./RTSP_TCP_TUNNEL_PLAN.md)

## Key Technical Findings

### Platform Capabilities

| Feature | Status | Notes |
|---------|--------|-------|
| TCP Client | ✅ | `tal_net_connect()`, `tal_net_send()`, `tal_net_recv()` |
| TCP Server | ✅ | Socket listening supported |
| TLS | ✅ | `tuya_tls_transporter_connect()` |
| WiFi | ✅ | Full WiFi support |
| Video Codec | ✅ | H.264, H.265, MJPEG |
| RTSP Client | ❌ | Not built-in, would need implementation |
| SSH/Shell | ❌ | Not supported on RTOS |
| FFmpeg | ❌ | Too large for 8MB RAM |

### Available Network APIs

```c
// Socket operations
tal_net_socket_create(PROTOCOL_TCP)
tal_net_str2addr("192.168.1.1")
tal_net_connect(sock_fd, ip_addr, port)
tal_net_send(sock_fd, buffer, length)
tal_net_recv(sock_fd, buffer, length)
tal_net_close(sock_fd)

// For TLS connections
tuya_tls_transporter_connect(host, port)
```

### Reference Code Locations

- **TCP Client**: `examples/protocols/tcp_client/src/example_tcp_client.c`
- **TCP Server**: `examples/protocols/tcp_server/src/example_tcp_server.c`
- **Camera Demo**: `apps/tuya_cloud/camera_demo/`
- **Network Camera**: `platform/T5AI/t5_os/ap/components/multimedia/camera/net_camera.c`
- **Audio Debug TCP**: `ai_audio_debug.c` (demonstrates outbound TCP streaming)

### Memory Constraints

- **Total RAM**: 8MB
- **Available for app**: Variable, likely 1-4MB
- **TCP tunnel estimate**: ~30-50KB (very feasible)

## Resources Used

- DeepWiki: `tuya/TuyaOpen` repository
- Local codebase exploration
- TuyaOpen documentation

## Recommended Solution

**TCP Tunnel via T5AI** - Create a simple application that:
1. Connects outbound to VPS (bypasses NAT)
2. Connects locally to RTSP camera
3. Forwards TCP bytes bidirectionally
4. VPS runs FFmpeg to process the stream

---

*Investigation completed: 2025-12-17*
