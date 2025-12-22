# Speaker Streaming Implementation

## Overview

This document describes the implementation of two-way audio (talk-back) functionality that allows a browser user to speak through the T5AI DevKit's speaker.

### Architecture

```
Browser Microphone
       │
       ▼ (WebRTC Opus Audio)
┌──────────────────┐
│   VPS Server     │
│   (Go WebApp)    │
│   Port 5002 UDP  │
└────────┬─────────┘
         │ (NAT Hole Punching)
         │ (Raw PCM via UDP)
         ▼
┌──────────────────┐
│  T5AI DevKit     │
│  speaker_udp     │
│  Port 5002 UDP   │
└────────┬─────────┘
         │
         ▼
     🔊 Speaker
```

### NAT Traversal

The DevKit is typically behind a NAT (router). To allow the VPS to send UDP packets to the DevKit:

1. DevKit sends periodic UDP "ping" packets (0xFE marker) to VPS port 5002
2. This creates a NAT mapping in the router
3. VPS tracks the DevKit's NAT-mapped address from incoming pings
4. VPS sends audio through this NAT "hole"

---

## Files Changed/Added

### 1. DevKit Firmware - Speaker Streaming Module (NEW FILE)

**File:** `apps/tuya_cloud/object_detection/src/speaker_streaming.c`

```c
/**
 * @file speaker_streaming.c
 * @brief UDP Speaker Streaming Module for T5AI DevKit
 *
 * Receives PCM audio data via UDP from the web server and plays it
 * through the DevKit speaker. This enables two-way audio communication
 * where the browser user can talk to the DevKit.
 *
 * NAT Traversal:
 * - DevKit sends periodic UDP "ping" packets to VPS port 5002
 * - This creates a NAT mapping allowing VPS to send audio back
 * - VPS tracks DevKit's NAT-mapped address from these pings
 *
 * Audio Format:
 * - Sample Rate: 16kHz
 * - Channels: Mono
 * - Bit Depth: 16-bit signed
 * - Transport: UDP packets (raw PCM, no header)
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tal_api.h"
#include "tal_network.h"
#include "tdl_audio_manage.h"
#include "tuya_config.h"
#include "ai_audio_player.h"
#include "ai_audio.h"

/* UDP port for speaker audio (same on both DevKit and VPS) */
#define SPEAKER_UDP_PORT    5002

/* PCM buffer size (max UDP payload) */
#define PCM_BUF_SIZE        1400

/* Receive timeout in milliseconds */
#define RECV_TIMEOUT_MS     100

/* NAT keepalive interval in milliseconds (send ping every 5 seconds) */
#define NAT_KEEPALIVE_MS    5000

/* Ping packet marker (0xFE = speaker ping, different from mic ping 0xFF) */
#define SPEAKER_PING_MARKER 0xFE

/* Global state */
static bool g_speaker_active = false;
static int g_udp_socket = -1;
static TDL_AUDIO_HANDLE_T g_audio_hdl = NULL;
static THREAD_HANDLE g_speaker_thread = NULL;
static THREAD_HANDLE g_keepalive_thread = NULL;

/* VPS server address for NAT hole punching */
static TUYA_IP_ADDR_T g_vps_addr = 0;
static uint16_t g_vps_port = SPEAKER_UDP_PORT;

/* Statistics */
static uint32_t g_packets_received = 0;
static uint32_t g_bytes_received = 0;
static uint32_t g_play_errors = 0;
static uint32_t g_pings_sent = 0;

/* VPS host from compile-time definition */
#ifndef TCP_SERVER_HOST
#define TCP_SERVER_HOST "YOUR_VPS_IP"  /* Default fallback */
#endif

/**
 * @brief NAT keepalive task - sends periodic pings to VPS to keep NAT hole open
 */
static void speaker_keepalive_task(void *arg)
{
    uint8_t ping_pkt = SPEAKER_PING_MARKER;
    
    PR_INFO("[SPEAKER] NAT keepalive task started -> %s:%d", 
            tal_net_addr2str(g_vps_addr), g_vps_port);
    
    while (g_speaker_active) {
        /* Send ping to VPS to punch/maintain NAT hole */
        int sent = tal_net_send_to(g_udp_socket, &ping_pkt, 1, g_vps_addr, g_vps_port);
        if (sent == 1) {
            g_pings_sent++;
            if (g_pings_sent % 12 == 1) {  /* Log every minute */
                PR_DEBUG("[SPEAKER] NAT keepalive ping #%u sent", g_pings_sent);
            }
        } else {
            PR_WARN("[SPEAKER] NAT keepalive ping failed: %d", sent);
        }
        
        /* Wait before next ping */
        tal_system_sleep(NAT_KEEPALIVE_MS);
    }
    
    PR_INFO("[SPEAKER] NAT keepalive task stopped (sent %u pings)", g_pings_sent);
}

/**
 * @brief Speaker RX task - receives UDP audio and plays it
 */
static void speaker_rx_task(void *arg)
{
    uint8_t buf[PCM_BUF_SIZE];
    TUYA_IP_ADDR_T addr = 0;
    uint16_t port = 0;
    TUYA_ERRNO len;

    PR_INFO("[SPEAKER] UDP receive task started on port %d", SPEAKER_UDP_PORT);

    /* Find audio handle */
    OPERATE_RET rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_audio_hdl);
    if (rt != OPRT_OK || g_audio_hdl == NULL) {
        PR_ERR("[SPEAKER] Failed to find audio codec: %d", rt);
        return;
    }
    PR_INFO("[SPEAKER] Audio codec found: %s", AUDIO_CODEC_NAME);

    while (g_speaker_active) {
        /* Receive UDP packet (may timeout and return <= 0) */
        len = tal_net_recvfrom(g_udp_socket, buf, sizeof(buf), &addr, &port);
        
        if (len <= 0) {
            /* Timeout or error - continue waiting */
            continue;
        }
        
        /* Ignore ping responses (1 byte packets) */
        if (len == 1) {
            continue;
        }

        /* Got audio data - play it */
        g_packets_received++;
        g_bytes_received += len;

        if (g_packets_received % 100 == 1) {
            PR_DEBUG("[SPEAKER] Packet #%u: %d bytes from %s:%d", 
                     g_packets_received, (int)len, 
                     tal_net_addr2str(addr), port);
        }

        /* Play PCM data through speaker */
        rt = tdl_audio_play(g_audio_hdl, buf, len);
        if (rt != OPRT_OK) {
            g_play_errors++;
            if (g_play_errors % 100 == 1) {
                PR_WARN("[SPEAKER] Play error #%u: %d", g_play_errors, rt);
            }
        }
    }

    PR_INFO("[SPEAKER] UDP receive task stopped");
}

/**
 * @brief Initialize speaker streaming module
 */
OPERATE_RET speaker_streaming_init(void)
{
    OPERATE_RET rt;

    if (g_speaker_active) {
        PR_WARN("[SPEAKER] Already initialized");
        return OPRT_OK;
    }

    PR_INFO("[SPEAKER] Initializing UDP speaker streaming on port %d...", SPEAKER_UDP_PORT);

    /* Use compile-time VPS host */
    const char *vps_host = TCP_SERVER_HOST;
    PR_INFO("[SPEAKER] Using VPS host: %s", vps_host);
    
    g_vps_addr = tal_net_str2addr(vps_host);
    if (g_vps_addr == 0) {
        rt = tal_net_gethostbyname(vps_host, &g_vps_addr);
        if (rt != OPRT_OK || g_vps_addr == 0) {
            PR_ERR("[SPEAKER] Failed to resolve VPS address: %s", vps_host);
            return OPRT_COM_ERROR;
        }
    }
    PR_INFO("[SPEAKER] VPS address resolved: %s -> %s", 
            vps_host, tal_net_addr2str(g_vps_addr));

    /* Create UDP socket */
    g_udp_socket = tal_net_socket_create(PROTOCOL_UDP);
    if (g_udp_socket < 0) {
        PR_ERR("[SPEAKER] Failed to create UDP socket");
        return OPRT_COM_ERROR;
    }

    /* Set socket receive timeout */
    rt = tal_net_set_timeout(g_udp_socket, RECV_TIMEOUT_MS, TRANS_RECV);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to set socket timeout: %d", rt);
    }

    /* Bind to local port */
    rt = tal_net_bind(g_udp_socket, TY_IPADDR_ANY, SPEAKER_UDP_PORT);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to bind to port %d, using ephemeral port", SPEAKER_UDP_PORT);
    }

    /* Mark as active */
    g_speaker_active = true;
    
    /* Start receiver thread */
    THREAD_CFG_T rx_cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_3,
        .thrdname = "speaker_udp"
    };
    rt = tal_thread_create_and_start(&g_speaker_thread, NULL, NULL, 
                                      speaker_rx_task, NULL, &rx_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[SPEAKER] Failed to create receiver thread: %d", rt);
        g_speaker_active = false;
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
        return rt;
    }

    /* Start NAT keepalive thread */
    THREAD_CFG_T ka_cfg = {
        .stackDepth = 2048,
        .priority = THREAD_PRIO_4,
        .thrdname = "spk_keepalive"
    };
    rt = tal_thread_create_and_start(&g_keepalive_thread, NULL, NULL, 
                                      speaker_keepalive_task, NULL, &ka_cfg);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to create keepalive thread: %d", rt);
    }

    PR_NOTICE("[SPEAKER] Speaker streaming initialized successfully");
    PR_NOTICE("[SPEAKER] NAT hole punching to %s:%d every %dms", 
              tal_net_addr2str(g_vps_addr), g_vps_port, NAT_KEEPALIVE_MS);
    
    return OPRT_OK;
}

/**
 * @brief Stop speaker streaming
 */
OPERATE_RET speaker_streaming_stop(void)
{
    if (!g_speaker_active) {
        return OPRT_OK;
    }

    g_speaker_active = false;
    tal_system_sleep(200);

    if (g_udp_socket >= 0) {
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
    }

    if (g_speaker_thread) {
        tal_thread_delete(g_speaker_thread);
        g_speaker_thread = NULL;
    }
    if (g_keepalive_thread) {
        tal_thread_delete(g_keepalive_thread);
        g_keepalive_thread = NULL;
    }

    PR_INFO("[SPEAKER] Stats: %u packets, %u bytes, %u errors, %u pings",
            g_packets_received, g_bytes_received, g_play_errors, g_pings_sent);

    return OPRT_OK;
}

bool speaker_streaming_is_active(void) { return g_speaker_active; }

void speaker_streaming_get_stats(uint32_t *packets, uint32_t *bytes, uint32_t *errors)
{
    if (packets) *packets = g_packets_received;
    if (bytes) *bytes = g_bytes_received;
    if (errors) *errors = g_play_errors;
}
```

---

### 2. DevKit Firmware - Speaker Streaming Header (NEW FILE)

**File:** `apps/tuya_cloud/object_detection/src/speaker_streaming.h`

```c
/**
 * @file speaker_streaming.h
 * @brief UDP Speaker Streaming Module API
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __SPEAKER_STREAMING_H__
#define __SPEAKER_STREAMING_H__

#include "tuya_cloud_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize speaker streaming module
 *
 * Creates UDP socket on port 5002 and starts receiver thread.
 * Audio received will be played through the DevKit speaker.
 *
 * @return OPRT_OK on success
 */
OPERATE_RET speaker_streaming_init(void);

/**
 * @brief Stop speaker streaming
 *
 * @return OPRT_OK on success
 */
OPERATE_RET speaker_streaming_stop(void);

/**
 * @brief Check if speaker streaming is active
 *
 * @return true if active
 */
bool speaker_streaming_is_active(void);

/**
 * @brief Get speaker streaming statistics
 *
 * @param packets Output: number of packets received
 * @param bytes Output: total bytes received
 * @param errors Output: number of play errors
 */
void speaker_streaming_get_stats(uint32_t *packets, uint32_t *bytes, uint32_t *errors);

#ifdef __cplusplus
}
#endif

#endif /* __SPEAKER_STREAMING_H__ */
```

---

### 3. DevKit Firmware - Main Application (MODIFIED)

**File:** `apps/tuya_cloud/object_detection/src/tuya_main.c`

**Changes:**

1. Added include for speaker streaming:
```c
/* Microphone streaming for web app */
#include "mic_streaming.h"

/* Speaker streaming for two-way audio (talk-back from browser) */
#include "speaker_streaming.h"
```

2. Added speaker streaming initialization after audio init:
```c
    /* Note: mic_streaming_init() was called earlier, before tdl_audio_open() */

    /* Initialize speaker streaming for two-way audio (talk-back from browser) */
    PR_INFO("Initializing speaker streaming for two-way audio...");
    rt = speaker_streaming_init();
    if (rt != OPRT_OK) {
        PR_WARN("Failed to initialize speaker streaming: %d (talk-back disabled)", rt);
    } else {
        PR_NOTICE("Speaker streaming initialized - listening on UDP port 5002");
    }
```

---

### 4. Go Web Server - Main Application (MODIFIED)

**File:** `webapp/main.go`

**Changes:**

1. Added DevKit speaker NAT address tracking variables:
```go
// DevKit connection
var (
    devkitConn          net.Conn
    devkitAuthenticated bool
    devkitMutex         sync.RWMutex
    devkitIP            net.IP // Track DevKit IP for speaker UDP (from TCP - may not work through NAT)
)

// Speaker UDP configuration
const speakerUDPPort = 5002
const speakerPingMarker = 0xFE // DevKit sends this to register its NAT address

// DevKit speaker NAT address (from UDP ping - this is the NAT-translated address)
var (
    devkitSpeakerAddr   *net.UDPAddr // NAT-mapped address for sending speaker audio
    devkitSpeakerMutex  sync.RWMutex
    speakerPingCount    uint64
)
```

2. Added Speaker UDP Server for NAT hole punching:
```go
// Global UDP connection for sending speaker audio (reused for efficiency)
var speakerUDPConn *net.UDPConn

func startSpeakerUDPServer() {
    addr, err := net.ResolveUDPAddr("udp", fmt.Sprintf("0.0.0.0:%d", speakerUDPPort))
    if err != nil {
        log.Fatalf("[SPEAKER-UDP] Failed to resolve address: %v", err)
    }
    
    conn, err := net.ListenUDP("udp", addr)
    if err != nil {
        log.Fatalf("[SPEAKER-UDP] Failed to start: %v", err)
    }
    speakerUDPConn = conn
    
    log.Printf("[SPEAKER-UDP] Listening on port %d for DevKit pings", speakerUDPPort)
    
    buf := make([]byte, 16)
    
    for {
        n, remoteAddr, err := conn.ReadFromUDP(buf)
        if err != nil {
            log.Printf("[SPEAKER-UDP] Read error: %v", err)
            continue
        }
        
        // Check for speaker ping marker (0xFE)
        if n == 1 && buf[0] == speakerPingMarker {
            speakerPingCount++
            
            // Update DevKit speaker NAT address
            devkitSpeakerMutex.Lock()
            oldAddr := devkitSpeakerAddr
            devkitSpeakerAddr = remoteAddr
            devkitSpeakerMutex.Unlock()
            
            // Log address changes or periodically
            if oldAddr == nil || oldAddr.String() != remoteAddr.String() {
                log.Printf("[SPEAKER-UDP] DevKit NAT address registered: %s", remoteAddr.String())
            } else if speakerPingCount%60 == 0 {
                log.Printf("[SPEAKER-UDP] Ping #%d from %s (keepalive)", speakerPingCount, remoteAddr.String())
            }
        }
    }
}

// sendSpeakerAudio sends PCM audio to DevKit through NAT hole
func sendSpeakerAudio(pcmBytes []byte) error {
    devkitSpeakerMutex.RLock()
    addr := devkitSpeakerAddr
    devkitSpeakerMutex.RUnlock()
    
    if addr == nil {
        return fmt.Errorf("no DevKit speaker address registered")
    }
    
    if speakerUDPConn == nil {
        return fmt.Errorf("speaker UDP connection not ready")
    }
    
    _, err := speakerUDPConn.WriteToUDP(pcmBytes, addr)
    return err
}
```

3. Updated WebRTC OnTrack handler to use NAT-aware sending:
```go
// Handle incoming audio track from browser (talk-back to DevKit)
peerConnection.OnTrack(func(track *webrtc.TrackRemote, receiver *webrtc.RTPReceiver) {
    log.Printf("[WEBRTC] Incoming track: %s (codec: %s)", track.Kind(), track.Codec().MimeType)

    if track.Kind() != webrtc.RTPCodecTypeAudio {
        return
    }

    decoder, err := opus.NewDecoder(sampleRate, channels)
    if err != nil {
        log.Printf("[WEBRTC] Failed to create Opus decoder: %v", err)
        return
    }

    // Wait for DevKit speaker address to be registered
    devkitSpeakerMutex.RLock()
    addr := devkitSpeakerAddr
    devkitSpeakerMutex.RUnlock()

    if addr == nil {
        log.Printf("[WEBRTC] No DevKit speaker address registered - waiting for ping")
        for i := 0; i < 10; i++ {
            time.Sleep(500 * time.Millisecond)
            devkitSpeakerMutex.RLock()
            addr = devkitSpeakerAddr
            devkitSpeakerMutex.RUnlock()
            if addr != nil { break }
        }
        if addr == nil {
            log.Printf("[WEBRTC] DevKit speaker not registered after 5s")
            return
        }
    }

    log.Printf("[WEBRTC] Speaker audio bridge started -> %s (NAT-mapped)", addr.String())

    rtpBuf := make([]byte, 1500)
    pcmSamples := make([]int16, 1920)
    var packetsSent uint64

    for {
        n, _, readErr := track.Read(rtpBuf)
        if readErr != nil {
            if readErr == io.EOF {
                log.Printf("[WEBRTC] Track ended (sent %d packets)", packetsSent)
            }
            return
        }

        samplesDecoded, decodeErr := decoder.Decode(rtpBuf[:n], pcmSamples)
        if decodeErr != nil || samplesDecoded == 0 { continue }

        // Convert int16 samples to bytes (little-endian)
        pcmBytes := make([]byte, samplesDecoded*2)
        for i := 0; i < samplesDecoded; i++ {
            pcmBytes[i*2] = byte(pcmSamples[i])
            pcmBytes[i*2+1] = byte(pcmSamples[i] >> 8)
        }

        // Send PCM to DevKit speaker via NAT hole
        if err := sendSpeakerAudio(pcmBytes); err == nil {
            packetsSent++
        }
    }
})
```

4. Added speaker UDP server startup in main():
```go
// Start Speaker UDP server (NAT hole punching for talk-back)
go startSpeakerUDPServer()
```

---

### 5. Web Client - Frontend (MODIFIED)

**File:** `webapp/public/index.html`

**Changes:**

1. Added Talk buttons UI:
```html
<!-- Two-way audio: Talk back to DevKit -->
<div style="margin-top: 1rem; padding-top: 1rem; border-top: 1px solid var(--border-color);">
    <div style="margin-bottom: 0.5rem; font-size: 0.85rem;">
        <span style="color: var(--accent-cyan);">🔊 Talk to DevKit Speaker</span>
        <span id="talk-status" style="color: var(--text-secondary); margin-left: 0.5rem;">(click Start to talk)</span>
    </div>
    <div class="btn-group">
        <button id="btn-talk-start" class="btn btn-primary" onclick="startTalking()"
            style="background: linear-gradient(135deg, #f59e0b 0%, #d97706 100%);">
            🎙️ Start Talking
        </button>
        <button id="btn-talk-stop" class="btn btn-secondary" onclick="stopTalking()" disabled>
            ⏹ Stop
        </button>
    </div>
</div>
```

2. Added JavaScript for talk-back functionality:
```javascript
// ==================== Two-Way Audio (Talk Back) ====================
let localMicStream = null;
let talkActive = false;
let talkPeerConnection = null;

// Start talking (Start/Stop button mode)
async function startTalking() {
    if (talkActive) return;

    try {
        log('info', '🎙️ Starting talk-back...');
        
        // Update UI - disable start, enable stop
        document.getElementById('btn-talk-start').disabled = true;
        document.getElementById('btn-talk-stop').disabled = false;
        document.getElementById('talk-status').textContent = '🔴 Connecting...';
        document.getElementById('talk-status').style.color = 'var(--accent-red)';

        // Request microphone access
        localMicStream = await navigator.mediaDevices.getUserMedia({
            audio: {
                sampleRate: 16000,
                channelCount: 1,
                echoCancellation: true,
                noiseSuppression: true,
                autoGainControl: true
            }
        });

        log('info', 'Browser mic captured, connecting WebRTC for talk-back...');

        // Connect WebRTC with send capability
        await connectTalkWebRTC();

        talkActive = true;
        document.getElementById('talk-status').textContent = '🎙️ Talking...';
        document.getElementById('talk-status').style.color = 'var(--accent-green)';

    } catch (err) {
        log('error', 'Talk-back error: ' + err.message);
        stopTalkingLocal();
    }
}

// Stop talking
function stopTalking() {
    if (!talkActive && !localMicStream) return;
    log('info', '🎙️ Stopping talk-back...');
    stopTalkingLocal();
}

// Stop talking (local cleanup only)
function stopTalkingLocal() {
    talkActive = false;

    if (localMicStream) {
        localMicStream.getTracks().forEach(t => t.stop());
        localMicStream = null;
    }

    if (talkPeerConnection) {
        talkPeerConnection.close();
        talkPeerConnection = null;
    }

    // Update UI
    document.getElementById('btn-talk-start').disabled = false;
    document.getElementById('btn-talk-stop').disabled = true;
    document.getElementById('talk-status').textContent = '(click Start to talk)';
    document.getElementById('talk-status').style.color = 'var(--text-secondary)';
}

// Connect WebRTC for talk-back (send audio to server)
async function connectTalkWebRTC() {
    const config = {
        iceServers: [
            { urls: 'stun:stun.l.google.com:19302' },
            { urls: 'stun:stun1.l.google.com:19302' },
            { urls: 'turn:YOUR_VPS_IP:3478', username: 'turnuser', credential: 'TuyaT5DevKit2024' },
            { urls: 'turn:YOUR_VPS_IP:3478?transport=tcp', username: 'turnuser', credential: 'TuyaT5DevKit2024' }
        ]
    };

    talkPeerConnection = new RTCPeerConnection(config);

    // Add local mic track
    if (localMicStream) {
        localMicStream.getTracks().forEach(track => {
            talkPeerConnection.addTrack(track, localMicStream);
            log('info', 'Added mic track: ' + track.kind);
        });
    }

    talkPeerConnection.onconnectionstatechange = function () {
        log('info', `Talk WebRTC: ${talkPeerConnection.connectionState}`);
        if (talkPeerConnection.connectionState === 'connected') {
            document.getElementById('talk-status').textContent = '🎙️ Transmitting...';
            document.getElementById('talk-status').style.color = 'var(--accent-green)';
        }
    };

    // Create offer and exchange with server
    const offer = await talkPeerConnection.createOffer();
    await talkPeerConnection.setLocalDescription(offer);

    const response = await fetch('/webrtc-offer', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(offer)
    });

    if (!response.ok) throw new Error(`Server: ${response.status}`);

    const answer = await response.json();
    await talkPeerConnection.setRemoteDescription(answer);
    log('info', '✅ Talk-back WebRTC connected');
}
```

---

### 6. Dockerfile (MODIFIED)

**File:** `webapp/Dockerfile`

**Change:** Added UDP port 5002 exposure:
```dockerfile
# Expose ports: HTTP, TCP, UDP mic, UDP speaker
EXPOSE 3000 5000 5001/udp 5002/udp
```

---

## AWS Lightsail Firewall

Make sure to open the following ports in AWS Lightsail Networking tab:

| Protocol | Port | Purpose |
|----------|------|---------|
| TCP | 3000 | HTTP Web UI |
| TCP | 5000 | DevKit TCP commands |
| UDP | 5001 | Microphone audio (DevKit → VPS) |
| UDP | 5002 | Speaker audio (VPS → DevKit NAT hole) |

---

## Testing

1. Flash the DevKit with the updated firmware
2. Deploy the updated web app container
3. Wait for DevKit to boot and connect to VPS
4. Check VPS logs for: `[SPEAKER-UDP] DevKit NAT address registered: <ip>:<port>`
5. Open web UI, click "🎙️ Start Talking"
6. Speak into your browser - audio should play on DevKit speaker
7. Click "⏹ Stop" when done
