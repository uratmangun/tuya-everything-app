
# Two-Way Audio (Talk Back) Implementation Guide  
**Tuya T5AI DevKit – Web Client ↔ DevKit Speaker & Mic**

This document provides a **complete, end-to-end implementation** to enable **two-way audio communication** between:

- **T5AI DevKit microphone → Web browser**
- **Web browser microphone → T5AI DevKit speaker**

It is designed to work with the original repository **without restructuring**, only adding and modifying files as described.

---

## 1. Repository Folders Involved

### Web Server & Client
```
webapp/
├── main.go
├── go.mod
├── go.sum
└── public/
    └── index.html
```

### DevKit Firmware
```
apps/tuya_cloud/object_detection/
└── src/
    ├── tuya_main.c
    └── speaker_streaming.c   (NEW)
```

---

## 2. Audio Data Flow Overview

```
[BROWSER MIC]
   ↓ WebRTC (Opus)
[GO SERVER]
   ↓ Decode Opus → PCM
   ↓ UDP (16kHz PCM)
[T5AI DEVKIT SPEAKER]

[T5AI DEVKIT MIC]
   ↓ PCM → Opus
   ↓ WebRTC
[BROWSER SPEAKER]
```

---

## 3. Firmware Changes (T5AI DevKit)

### 3.1 Create Speaker Streaming Module

**File:**  
```
apps/tuya_cloud/object_detection/src/speaker_streaming.c
```

```c
#include "tal_api.h"
#include "tal_network.h"
#include "tdl_audio_manage.h"
#include "tuya_config.h"

#define SPEAKER_UDP_PORT 5002
#define PCM_BUF_SIZE 1400

static bool g_speaker_active = false;
static int g_udp_socket = -1;
static TDL_AUDIO_HANDLE_T g_audio_hdl = NULL;
static THREAD_HANDLE g_thread = NULL;

static void speaker_rx_task(void *arg) {
    uint8_t buf[PCM_BUF_SIZE];
    TUYA_IP_ADDR_T addr;
    uint16_t port;

    tdl_audio_find(AUDIO_CODEC_NAME, &g_audio_hdl);

    while (g_speaker_active) {
        int len = tal_net_recv_from(g_udp_socket, buf, sizeof(buf), &addr, &port);
        if (len > 0) {
            tdl_audio_play(g_audio_hdl, buf, len);
        }
    }
}

OPERATE_RET speaker_streaming_init(void) {
    g_udp_socket = tal_net_socket_create(PROTOCOL_UDP);
    tal_net_bind(g_udp_socket, 0, SPEAKER_UDP_PORT);

    g_speaker_active = true;
    THREAD_CFG_T cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_3,
        .thrdname = "speaker_udp"
    };

    return tal_thread_create_and_start(&g_thread, NULL, NULL, speaker_rx_task, NULL, &cfg);
}
```

---

### 3.2 Initialize Speaker Streaming

**Modify file:**  
```
apps/tuya_cloud/object_detection/src/tuya_main.c
```

Add near the top:
```c
extern OPERATE_RET speaker_streaming_init(void);
```

Inside `user_main()` **after audio init**:
```c
speaker_streaming_init();
```

---

## 4. Server Changes (Go WebApp)

### 4.1 Track DevKit IP

**Modify:** `webapp/main.go`

Add imports:
```go
import "io"
```

Add globals:
```go
var (
    devKitIP net.IP
    devKitAddrMutex sync.RWMutex
)
```

Inside `handleDevKitConnection()`:
```go
if tcpAddr, ok := conn.RemoteAddr().(*net.TCPAddr); ok {
    devKitAddrMutex.Lock()
    devKitIP = tcpAddr.IP
    devKitAddrMutex.Unlock()
}
```

---

### 4.2 WebRTC Audio → UDP Speaker Bridge

Inside `handleWebRTCOffer()`:

```go
peerConnection.OnTrack(func(track *webrtc.TrackRemote, receiver *webrtc.RTPReceiver) {
    decoder, _ := opus.NewDecoder(16000, 1)

    devKitAddrMutex.RLock()
    ip := devKitIP
    devKitAddrMutex.RUnlock()
    if ip == nil { return }

    addr, _ := net.ResolveUDPAddr("udp", ip.String()+":5002")
    conn, _ := net.DialUDP("udp", nil, addr)
    defer conn.Close()

    rtpBuf := make([]byte, 1500)
    pcm := make([]int16, 1000)

    for {
        n, _, err := track.Read(rtpBuf)
        if err == io.EOF { return }

        samples, err := decoder.Decode(rtpBuf[:n], pcm)
        if err != nil { continue }

        out := make([]byte, samples*2)
        for i := 0; i < samples; i++ {
            out[i*2] = byte(pcm[i])
            out[i*2+1] = byte(pcm[i] >> 8)
        }
        conn.Write(out)
    }
})
```

---

## 5. Web Client (Browser)

### 5.1 Talk Button

**Modify:** `webapp/public/index.html`

Add button:
```html
<button id="btn-talk" onclick="toggleTalkBack()">🎙 Start Talking</button>
```

---

### 5.2 JavaScript Logic

```html
<script>
let localMicStream = null;

async function toggleTalkBack() {
    if (localMicStream) {
        localMicStream.getTracks().forEach(t => t.stop());
        localMicStream = null;
        connectWebRTC();
        return;
    }

    localMicStream = await navigator.mediaDevices.getUserMedia({
        audio: {
            sampleRate: 16000,
            channelCount: 1,
            echoCancellation: true
        }
    });
    connectWebRTC();
}

async function connectWebRTC() {
    peerConnection = new RTCPeerConnection(config);

    if (localMicStream) {
        localMicStream.getTracks().forEach(t => {
            peerConnection.addTrack(t, localMicStream);
        });
    }

    // existing offer / answer logic
}
</script>
```

---

## 6. Audio Format Requirements

| Direction | Codec |
|--------|------|
| Browser → DevKit | Opus → PCM |
| DevKit → Browser | PCM → Opus |
| Sample Rate | 16 kHz |
| Channels | Mono |
| Bit Depth | 16-bit |
| Transport | WebRTC + UDP |

---

## 7. Result

✔ Real-time two-way audio  
✔ Echo cancelled browser mic  
✔ No cloud dependency  
✔ Low latency UDP speaker path  

---

## 8. Troubleshooting

- **No sound on DevKit** → Check UDP port 5002
- **Delay / choppy audio** → Reduce PCM buffer size
- **Echo** → Enable browser echoCancellation

---

## 9. Done 🎉

Your T5AI DevKit now supports **full duplex talk-back audio**.
