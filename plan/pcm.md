# PCM Audio Streaming via UDP - Implementation Guide

## Overview

The T5AI DevKit captures audio from its onboard microphone and streams raw PCM data over UDP to the VPS server, which then encodes it to Opus and streams via WebRTC to the browser.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ T5AI DevKit                                                                 │
│                                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌───────────┐ │
│  │ Onboard Mic  │───▶│ Audio Driver │───▶│ Ring Buffer  │───▶│ UDP Send  │ │
│  │ (16kHz/16bit)│    │ (Callback)   │    │ (64KB)       │    │ (Port 5001)│ │
│  └──────────────┘    └──────────────┘    └──────────────┘    └───────────┘ │
│                                                                      │      │
└──────────────────────────────────────────────────────────────────────│──────┘
                                                                       │
                                                              UDP Packets
                                                              [SEQ:1][PCM:640]
                                                                       │
                                                                       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ VPS Server (Go)                                                              │
│                                                                              │
│  ┌───────────┐    ┌──────────────┐    ┌──────────────┐    ┌───────────────┐ │
│  │ UDP Recv  │───▶│ Opus Encoder │───▶│ WebRTC Track │───▶│ Browser       │ │
│  │ (Port 5001)│    │ (24kbps)     │    │ (Pion)       │    │ (Native Play) │ │
│  └───────────┘    └──────────────┘    └──────────────┘    └───────────────┘ │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `src/mic_streaming.c` | Main streaming logic, ring buffer management, tasks |
| `src/mic_streaming.h` | Public API for mic streaming |
| `src/udp_audio.c` | UDP socket handling, packet framing |
| `src/udp_audio.h` | UDP audio API |

---

## Audio Configuration

| Parameter | Value | Description |
|-----------|-------|-------------|
| Sample Rate | 16000 Hz | T5AI mic native rate |
| Bit Depth | 16-bit | Signed PCM |
| Channels | 1 (Mono) | Single channel |
| Frame Duration | 20 ms | Standard for voice |
| Frame Samples | 320 | 16000 × 0.020 = 320 |
| Frame Bytes | 640 | 320 × 2 bytes |

---

## UDP Packet Format

```
┌─────────┬────────────────────────────────────────┐
│ SEQ (1) │ PCM Data (640 bytes)                   │
└─────────┴────────────────────────────────────────┘
   │                    │
   │                    └── Raw 16-bit PCM samples (320 samples)
   │
   └── Sequence number (0-255, wraps around)
```

**Total packet size**: 641 bytes per 20ms frame

---

## Data Flow

### 1. Audio Capture (mic_streaming.c:71-112)

The audio driver calls `mic_audio_frame_callback()` when new audio is available:

```c
static void mic_audio_frame_callback(TDL_AUDIO_FRAME_FORMAT_E type, 
                                      TDL_AUDIO_STATUS_E status,
                                      uint8_t *data, uint32_t len)
{
    // Only handle PCM data
    if (type != TDL_AUDIO_FRAME_FORMAT_PCM) {
        return;
    }
    
    // Write to ring buffer
    uint32_t written = tuya_ring_buff_write(g_mic_ctx.ringbuf, data, len);
}
```

### 2. Ring Buffer (mic_streaming.c:246-253)

A 64KB ring buffer decouples the audio driver from UDP sending:

```c
// Create ring buffer - COVERAGE type overwrites old data when full
rt = tuya_ring_buff_create(MIC_RINGBUF_SIZE, OVERFLOW_PSRAM_COVERAGE_TYPE, &g_mic_ctx.ringbuf);
```

**Why ring buffer?**
- Audio driver runs in interrupt context - must be fast
- UDP sending may block briefly
- Prevents audio driver from stalling

### 3. Streaming Task (mic_streaming.c:159-217)

A dedicated task reads from ring buffer and sends via UDP:

```c
static void mic_streaming_task(void *arg)
{
    uint8_t pcm_buffer[MIC_FRAME_SIZE_PCM];  // 640 bytes
    
    while (g_mic_ctx.streaming) {
        // Check available data
        data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        
        // Process frames while we have enough data
        while (data_len >= MIC_FRAME_SIZE_PCM && udp_audio_is_ready()) {
            // Read one frame
            uint32_t read_len = tuya_ring_buff_read(g_mic_ctx.ringbuf, 
                                                     pcm_buffer, 
                                                     MIC_FRAME_SIZE_PCM);
            
            // Send raw PCM via UDP
            int16_t *pcm_samples = (int16_t *)pcm_buffer;
            uint32_t num_samples = read_len / 2;  // 2 bytes per sample
            
            udp_audio_send_pcm(pcm_samples, num_samples);
        }
        
        tal_system_sleep(10);  // Check every 10ms
    }
}
```

### 4. UDP Sending (udp_audio.c:82-121)

Frames the PCM data with sequence number and sends:

```c
OPERATE_RET udp_audio_send_pcm(const int16_t *pcm_data, uint32_t pcm_samples)
{
    uint32_t pcm_bytes = pcm_samples * 2;
    
    // Build packet: [SEQ:1][PCM_DATA:N]
    g_send_buf[0] = g_udp.seq;
    memcpy(&g_send_buf[1], pcm_data, pcm_bytes);
    
    uint32_t packet_len = 1 + pcm_bytes;
    
    // Send via UDP
    int sent = tal_net_send_to(g_udp.socket_fd, g_send_buf, packet_len, 
                                g_udp.server_addr, g_udp.server_port);
    
    // Increment sequence (wraps at 255)
    g_udp.seq++;
    
    return OPRT_OK;
}
```

---

## UDP Keepalive (NAT Traversal)

Routers drop UDP port mappings after 30-60 seconds of inactivity. A keepalive task sends pings when audio isn't flowing:

```c
// udp_audio.c:175-195
OPERATE_RET udp_audio_send_ping(void)
{
    // Send minimal 1-byte ping packet (0xFF = ping marker)
    uint8_t ping_pkt = 0xFF;
    
    int sent = tal_net_send_to(g_udp.socket_fd, &ping_pkt, 1,
                               g_udp.server_addr, g_udp.server_port);
    return OPRT_OK;
}

// mic_streaming.c:121-151 - Keepalive task
static void udp_keepalive_task(void *arg)
{
    while (udp_audio_is_ready()) {
        uint32_t time_since_last_send = now - g_mic_ctx.last_send_time;
        
        // If no audio sent in last 20 seconds, send keepalive
        if (time_since_last_send > (20 * 1000)) {
            udp_audio_send_ping();
        }
        
        tal_system_sleep(25000);  // Check every 25 seconds
    }
}
```

---

## Bandwidth Calculation

| Direction | Data Rate | Calculation |
|-----------|-----------|-------------|
| DevKit → VPS | ~256 kbps | 641 bytes × 50 fps × 8 bits |
| VPS → Browser | ~24 kbps | Opus compressed |

---

## API Usage

### Start Streaming

```c
// Initialize (call once at startup)
mic_streaming_init();

// Start streaming to VPS
mic_streaming_start("13.212.218.43", 5001);
```

### Stop Streaming

```c
mic_streaming_stop();
```

### Check Status

```c
if (mic_streaming_is_active()) {
    uint32_t bytes, frames;
    mic_streaming_get_stats(&bytes, &frames);
}
```

---

## Server-Side Processing (Go)

The VPS receives UDP packets and encodes to Opus:

```go
// Read UDP packet
n, _, _ := conn.ReadFromUDP(buf)

// Extract sequence and PCM
seq := buf[0]
pcmData := buf[1:n]

// Convert bytes to int16 samples
for i := 0; i < len(pcmData)/2; i++ {
    pcmBuffer[i] = int16(pcmData[i*2]) | int16(pcmData[i*2+1])<<8
}

// Encode to Opus
opusData, _ := opusEncoder.Encode(pcmBuffer, opusBuffer)

// Write to WebRTC track
audioTrack.WriteSample(media.Sample{
    Data:     opusData,
    Duration: 20 * time.Millisecond,
})
```

---

## Why Raw PCM Instead of G.711?

| Aspect | Raw PCM | G.711 |
|--------|---------|-------|
| Quality | Best (uncompressed) | Good (8-bit) |
| Bandwidth | 256 kbps | 128 kbps |
| Server Opus | Optimal input | Transcoding loss |
| Complexity | Simple | Encoder on device |

**Decision**: Raw PCM provides the best quality source for Opus encoding. The extra bandwidth (256 vs 128 kbps) is negligible on WiFi.


---

# Server-Side Implementation (Go/WebRTC)

## Files

| File | Purpose |
|------|---------|
| `webapp/main.go` | Main server with UDP, Opus, WebRTC |
| `webapp/public/index.html` | Web client with WebRTC player |

---

## Server Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│ Go Server (main.go)                                                             │
│                                                                                 │
│  ┌─────────────────┐                                                            │
│  │ UDP Server      │ ◄── UDP packets from DevKit [SEQ:1][PCM:640]              │
│  │ (Port 5001)     │                                                            │
│  └────────┬────────┘                                                            │
│           │                                                                     │
│           ▼                                                                     │
│  ┌─────────────────┐                                                            │
│  │ Opus Encoder    │ PCM int16[] → Opus bytes (~20-40 bytes per frame)         │
│  │ (24kbps VoIP)   │                                                            │
│  └────────┬────────┘                                                            │
│           │                                                                     │
│           ▼                                                                     │
│  ┌─────────────────┐    ┌─────────────────┐                                     │
│  │ WebRTC Track    │───▶│ Peer Connection │───▶ Browser (native <audio>)       │
│  │ (Opus/48kHz)    │    │ (ICE/DTLS/SRTP) │                                     │
│  └─────────────────┘    └─────────────────┘                                     │
│                                                                                 │
│  ┌─────────────────┐                                                            │
│  │ HTTP Server     │ ◄── POST /webrtc-offer (SDP exchange)                     │
│  │ (Port 3000)     │ ──▶ JSON response (SDP answer)                            │
│  └─────────────────┘                                                            │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 1. UDP Server (main.go:423-515)

Receives raw PCM packets from DevKit:

```go
func startUDPServer() {
    addr, _ := net.ResolveUDPAddr("udp", "0.0.0.0:"+udpPort)
    conn, _ := net.ListenUDP("udp", addr)
    
    buf := make([]byte, 2048)
    pcmBuffer := make([]int16, frameSizeSamples)  // 320 samples

    for {
        n, remoteAddr, _ := conn.ReadFromUDP(buf)

        // Handle keepalive ping (1-byte 0xFF)
        if n == 1 && buf[0] == 0xFF {
            udpPingCount++
            continue
        }

        // Extract sequence number (first byte)
        seq := buf[0]
        pcmData := buf[1:n]

        // Convert bytes (Little Endian) to int16 samples
        sampleCount := len(pcmData) / 2
        for i := 0; i < sampleCount; i++ {
            pcmBuffer[i] = int16(pcmData[i*2]) | int16(pcmData[i*2+1])<<8
        }

        // Encode to Opus
        opusData, _ := encodePCMToOpus(pcmBuffer[:sampleCount])

        // Write to WebRTC track
        writeAudioToTrack(opusData)
    }
}
```

---

## 2. Opus Encoder (main.go:186-210)

Initializes and encodes PCM to Opus:

```go
func initOpusEncoder() error {
    opusEncoder, err = opus.NewEncoder(sampleRate, channels, opus.AppVoIP)
    opusEncoder.SetBitrate(24000)  // 24kbps for good voice quality
    return nil
}

func encodePCMToOpus(pcmData []int16) ([]byte, error) {
    opusMutex.Lock()
    defer opusMutex.Unlock()

    opusBuffer := make([]byte, 1024)
    n, err := opusEncoder.Encode(pcmData, opusBuffer)
    return opusBuffer[:n], err
}
```

**Opus Configuration:**
| Parameter | Value | Description |
|-----------|-------|-------------|
| Sample Rate | 16000 Hz | Input from DevKit |
| Channels | 1 (Mono) | Single channel |
| Application | VoIP | Optimized for speech |
| Bitrate | 24 kbps | Good quality, low bandwidth |

---

## 3. WebRTC Track (main.go:214-247)

Creates a shared audio track for all peer connections:

```go
func initWebRTCTrack() error {
    audioTrack, err = webrtc.NewTrackLocalStaticSample(
        webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus},
        "audio", "doorbell-audio",
    )
    return nil
}

func writeAudioToTrack(opusData []byte) {
    audioTrackMutex.RLock()
    track := audioTrack
    audioTrackMutex.RUnlock()

    // Write sample with duration for jitter buffer
    track.WriteSample(media.Sample{
        Data:     opusData,
        Duration: time.Millisecond * 20,  // 20ms per frame
    })
}
```

---

## 4. WebRTC Signaling (main.go:643-778)

Handles SDP offer/answer exchange:

```go
func handleWebRTCOffer(w http.ResponseWriter, r *http.Request) {
    var offer webrtc.SessionDescription
    json.NewDecoder(r.Body).Decode(&offer)

    // Create peer connection with STUN + TURN
    config := webrtc.Configuration{
        ICEServers: []webrtc.ICEServer{
            {URLs: []string{"stun:stun.l.google.com:19302"}},
            {
                URLs:       []string{"turn:13.212.218.43:3478"},
                Username:   "turnuser",
                Credential: "TuyaT5DevKit2024",
            },
        },
    }

    peerConnection, _ := webrtc.NewPeerConnection(config)

    // Add the shared audio track
    peerConnection.AddTrack(audioTrack)

    // Set remote description (offer from browser)
    peerConnection.SetRemoteDescription(offer)

    // Create answer
    answer, _ := peerConnection.CreateAnswer(nil)

    // Wait for ICE gathering
    gatherComplete := webrtc.GatheringCompletePromise(peerConnection)
    peerConnection.SetLocalDescription(answer)
    <-gatherComplete

    // Send answer with ICE candidates
    json.NewEncoder(w).Encode(peerConnection.LocalDescription())
}
```

---

## 5. Browser Client (index.html)

### WebRTC Connection

```javascript
async function connectWebRTC() {
    const config = {
        iceServers: [
            { urls: 'stun:stun.l.google.com:19302' },
            {
                urls: 'turn:13.212.218.43:3478',
                username: 'turnuser',
                credential: 'TuyaT5DevKit2024'
            }
        ]
    };

    peerConnection = new RTCPeerConnection(config);

    // Handle incoming audio track
    peerConnection.ontrack = function (event) {
        const audioEl = document.createElement('audio');
        audioEl.srcObject = event.streams[0];
        audioEl.play();  // Native browser playback
    };

    // Add receive-only audio transceiver
    peerConnection.addTransceiver('audio', { direction: 'recvonly' });

    // Create and send offer
    const offer = await peerConnection.createOffer();
    await peerConnection.setLocalDescription(offer);

    // Exchange SDP with server
    const response = await fetch('/webrtc-offer', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(offer)
    });

    const answer = await response.json();
    await peerConnection.setRemoteDescription(answer);
}
```

---

## WebRTC Signaling Flow

```
Browser                          Server                          DevKit
   │                               │                               │
   │  1. POST /webrtc-offer        │                               │
   │  ─────────────────────────▶   │                               │
   │  (SDP Offer)                  │                               │
   │                               │                               │
   │  2. Create PeerConnection     │                               │
   │  3. Add audioTrack            │                               │
   │  4. SetRemoteDescription      │                               │
   │  5. CreateAnswer              │                               │
   │  6. SetLocalDescription       │                               │
   │  7. Gather ICE candidates     │                               │
   │                               │                               │
   │  8. JSON Response             │                               │
   │  ◀─────────────────────────   │                               │
   │  (SDP Answer + ICE)           │                               │
   │                               │                               │
   │  9. SetRemoteDescription      │                               │
   │                               │                               │
   │  ═══════════════════════════════════════════════════════════  │
   │           ICE Connectivity Check (STUN/TURN)                  │
   │  ═══════════════════════════════════════════════════════════  │
   │                               │                               │
   │                               │  UDP [SEQ][PCM]               │
   │                               │  ◀─────────────────────────   │
   │                               │                               │
   │                               │  Opus Encode                  │
   │                               │                               │
   │  SRTP (Opus Audio)            │                               │
   │  ◀════════════════════════    │                               │
   │                               │                               │
   │  Native <audio> playback      │                               │
   │                               │                               │
```

---

## ICE (Interactive Connectivity Establishment)

### STUN vs TURN

| Type | Purpose | When Used |
|------|---------|-----------|
| STUN | Discover public IP | ~80% of connections (direct P2P) |
| TURN | Relay all traffic | ~20% (strict NAT/firewall) |

### ICE Servers Configuration

```go
// Server-side (main.go)
ICEServers: []webrtc.ICEServer{
    {URLs: []string{"stun:stun.l.google.com:19302"}},      // Google STUN
    {URLs: []string{"stun:stun1.l.google.com:19302"}},     // Backup STUN
    {
        URLs:       []string{"turn:13.212.218.43:3478"},   // Self-hosted TURN
        Username:   "turnuser",
        Credential: "TuyaT5DevKit2024",
    },
}
```

---

## Complete Data Flow Summary

```
1. DevKit captures 16kHz/16-bit PCM from mic
2. DevKit sends UDP packet: [SEQ:1][PCM:640] to VPS:5001
3. VPS receives UDP, extracts PCM bytes
4. VPS converts Little Endian bytes → int16 samples
5. VPS encodes int16[] → Opus bytes (24kbps)
6. VPS writes Opus to WebRTC track with 20ms duration
7. WebRTC sends SRTP packets to browser via ICE connection
8. Browser receives Opus in native <audio> element
9. Browser's jitter buffer handles packet loss/reordering
10. User hears audio!
```

---

## Latency Breakdown

| Stage | Latency | Notes |
|-------|---------|-------|
| Mic capture | ~20ms | One frame |
| Ring buffer | ~10ms | Task polling |
| UDP transit | ~30-50ms | Network dependent |
| Opus encode | <1ms | Very fast |
| WebRTC jitter | ~40-80ms | Adaptive buffer |
| **Total** | **~100-160ms** | End-to-end |
