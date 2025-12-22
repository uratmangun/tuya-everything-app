Yes, we can absolutely enable two-way communication ("Talk Back"). To do this, we need to reverse the flow you currently have: capture the microphone in the browser, send it to the server via WebRTC, decode it, and stream it to the DevKit's speaker via UDP.

Here is the complete implementation plan and code for all three components: **Firmware**, **Server**, and **Web Client**.

### 1. Firmware: Speaker Streaming (DevKit)

We need a new module on the DevKit that listens for UDP audio packets and plays them through the speaker.

**Create a new file:** `object_detection/src/speaker_streaming.c`

```c
/**
 * @file speaker_streaming.c
 * @brief UDP Speaker Streaming: Receives raw PCM (16kHz, 16-bit, Mono) and plays it.
 */
#include "tal_api.h"
#include "tal_network.h"
#include "tdl_audio_manage.h"
#include "tuya_config.h"

// Port to listen for incoming audio
#define SPEAKER_UDP_PORT 5002
#define PCM_BUF_SIZE     1400  // Safe UDP payload size

static bool g_speaker_active = false;
static THREAD_HANDLE g_speaker_thread = NULL;
static int g_udp_socket = -1;
static TDL_AUDIO_HANDLE_T g_audio_hdl = NULL;

static void speaker_rx_task(void *arg) {
    uint8_t recv_buf[PCM_BUF_SIZE];
    TUYA_IP_ADDR_T client_addr;
    uint16_t client_port;
    OPERATE_RET rt;
    
    PR_NOTICE("Speaker streaming thread started (UDP Port %d)", SPEAKER_UDP_PORT);
    
    // Find audio handle (already opened in tuya_main.c)
    rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_audio_hdl);
    if (rt != OPRT_OK) {
        PR_ERR("Speaker: Could not find audio handle!");
        return;
    }

    while (g_speaker_active) {
        // 1. Receive raw PCM data packet
        int len = tal_net_recv_from(g_udp_socket, recv_buf, sizeof(recv_buf), &client_addr, &client_port);
        
        if (len > 0) {
            // 2. Play immediately to hardware driver
            // Assumes incoming data is already 16kHz, 16-bit, Mono PCM
            tdl_audio_play(g_audio_hdl, recv_buf, len);
        } else {
            // No data or error, brief sleep to prevent tight loop on error
            tal_system_sleep(5);
        }
    }
    
    // Cleanup
    if (g_udp_socket >= 0) {
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
    }
}

OPERATE_RET speaker_streaming_init(void) {
    if (g_speaker_active) return OPRT_OK;

    // 1. Create UDP Socket
    g_udp_socket = tal_net_socket_create(PROTOCOL_UDP);
    if (g_udp_socket < 0) {
        PR_ERR("Speaker socket create failed");
        return OPRT_SOCK_ERR;
    }
    
    // 2. Bind to Port 5002
    // 0 = INADDR_ANY (Listen on all interfaces)
    OPERATE_RET rt = tal_net_bind(g_udp_socket, 0, SPEAKER_UDP_PORT);
    if (rt != OPRT_OK) {
        PR_ERR("Speaker bind failed: %d", rt);
        tal_net_close(g_udp_socket);
        return rt;
    }
    
    // 3. Start Receive Thread
    g_speaker_active = true;
    THREAD_CFG_T cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_3, // Slightly lower than mic to prioritize capture
        .thrdname = "speaker_udp"
    };
    
    rt = tal_thread_create_and_start(&g_speaker_thread, NULL, NULL, speaker_rx_task, NULL, &cfg);
    if (rt == OPRT_OK) {
        PR_INFO("Speaker streaming initialized successfully");
    }
    return rt;
}

```

**Modify:** `object_detection/src/tuya_main.c`

Add the initialization call in `user_main`.

```c
// [Add at the top with other includes]
// extern OPERATE_RET speaker_streaming_init(void); /* Or include header */

// [Inside user_main, AFTER audio init but BEFORE the main loop]
/* ... existing audio init code ... */

// Initialize Speaker Streaming
PR_INFO("Initializing Speaker Streaming (UDP 5002)...");
// Declare the function prototype here if you didn't make a .h file
extern OPERATE_RET speaker_streaming_init(void); 
rt = speaker_streaming_init();
if (rt != OPRT_OK) {
    PR_ERR("Failed to init speaker streaming: %d", rt);
}

// [Continue to main loop]

```

---

### 2. Server: WebRTC -> UDP Bridge (Go)

Update the Go server to accept audio from the browser, decode it (Opus -> PCM), and send it to the DevKit.

**Modify:** `webapp/main.go`

```go
// [Add imports]
import (
    "io" // Add this
    // ... existing imports
)

// [Global Variable to store DevKit IP]
var (
    // ... existing vars
    devKitAddrMutex sync.RWMutex
    devKitIP        net.IP
)

// [Update handleDevKitConnection]
// We capture the IP here so we know where to send UDP audio packets
func handleDevKitConnection(conn net.Conn) {
    // Save DevKit IP
    if tcpAddr, ok := conn.RemoteAddr().(*net.TCPAddr); ok {
        devKitAddrMutex.Lock()
        devKitIP = tcpAddr.IP
        devKitAddrMutex.Unlock()
        log.Printf("[SPEAKER] Target DevKit IP set to: %s", devKitIP)
    }
    
    // ... existing auth logic ...
}

// [Update handleWebRTCOffer]
func handleWebRTCOffer(w http.ResponseWriter, r *http.Request) {
    // ... existing setup ...
    
    peerConnection, err := webrtc.NewPeerConnection(config)
    // ... error checks ...

    // --- ADD THIS BLOCK ---
    // Handle Incoming Audio Track (Browser Mic -> DevKit Speaker)
    peerConnection.OnTrack(func(track *webrtc.TrackRemote, receiver *webrtc.RTPReceiver) {
        log.Printf("[WEBRTC] Receiving audio track: %s", track.ID())

        // Create Opus Decoder (16kHz, 1 channel)
        decoder, err := opus.NewDecoder(16000, 1)
        if err != nil {
            log.Printf("Opus decoder error: %v", err)
            return
        }

        // Get current DevKit IP
        devKitAddrMutex.RLock()
        targetIP := devKitIP
        devKitAddrMutex.RUnlock()
        
        if targetIP == nil {
            log.Println("DevKit not connected, cannot stream audio")
            return
        }

        // Connect to DevKit UDP Port 5002
        serverAddr, _ := net.ResolveUDPAddr("udp", fmt.Sprintf("%s:5002", targetIP.String()))
        conn, err := net.DialUDP("udp", nil, serverAddr)
        if err != nil {
            log.Printf("UDP Dial error: %v", err)
            return
        }
        defer conn.Close()

        // Buffers
        rtpBuf := make([]byte, 1500)
        pcmBuf := make([]int16, 1000) // 16kHz * 60ms max

        for {
            // 1. Read Opus packet from WebRTC
            n, _, readErr := track.Read(rtpBuf)
            if readErr != nil {
                if readErr != io.EOF { log.Println("Track read stopped") }
                return
            }

            // 2. Decode Opus -> PCM
            numSamples, decodeErr := decoder.Decode(rtpBuf[:n], pcmBuf)
            if decodeErr != nil {
                continue
            }

            // 3. Convert int16 -> byte array (Little Endian)
            data := make([]byte, numSamples*2)
            for i := 0; i < numSamples; i++ {
                data[i*2] = byte(pcmBuf[i])
                data[i*2+1] = byte(pcmBuf[i] >> 8)
            }

            // 4. Send to DevKit
            conn.Write(data)
        }
    })
    // --- END BLOCK ---

    // ... existing Offer/Answer logic ...
}

```

---

### 3. Web Client: Talk Button (HTML/JS)

Add a button to start the microphone and logic to restart the WebRTC connection with the mic track included.

**Modify:** `webapp/public/index.html`

Add this new logic to your `<script>`:

```javascript
// Global var to hold local mic stream
let localMicStream = null;

// Function to toggle Talk Back mode
async function toggleTalkBack() {
    const btn = document.getElementById('btn-talk');
    
    if (localMicStream) {
        // STOP TALKING
        stopTalkBack();
        btn.textContent = '🎙️ Start Talking';
        btn.className = 'btn btn-primary';
    } else {
        // START TALKING
        try {
            // 1. Get Mic Permission & Stream
            localMicStream = await navigator.mediaDevices.getUserMedia({
                audio: {
                    sampleRate: 16000,
                    channelCount: 1,
                    echoCancellation: true, // Important! Prevents you hearing yourself
                    noiseSuppression: true
                }
            });
            
            // 2. Re-connect WebRTC to include the new track
            // (We must restart the connection because the current simple server 
            // implementation doesn't support renegotiation on the fly)
            log('info', 'Mic active, restarting WebRTC...');
            await connectWebRTC();
            
            btn.textContent = '⏹ Stop Talking';
            btn.className = 'btn btn-secondary'; // Change style to indicate active
            
        } catch (err) {
            log('error', 'Mic access failed: ' + err.message);
        }
    }
}

function stopTalkBack() {
    if (localMicStream) {
        localMicStream.getTracks().forEach(track => track.stop());
        localMicStream = null;
        log('info', 'Mic stopped');
        // Re-connect to restore listen-only mode
        connectWebRTC();
    }
}

// UPDATE connectWebRTC function
async function connectWebRTC() {
    // ... existing setup ...
    peerConnection = new RTCPeerConnection(config);

    // --- ADD THIS ---
    // If we have a mic stream, add it to the connection
    if (localMicStream) {
        localMicStream.getTracks().forEach(track => {
            peerConnection.addTrack(track, localMicStream);
            log('info', 'Added microphone track to connection');
        });
    }
    // ----------------

    // ... existing ontrack / receive logic ...
    // ... existing offer/answer logic ...
}

```

**Add Button in HTML:**
Place this inside your "Quick Commands" or "Mic Status" card:

```html
<button id="btn-talk" class="btn btn-primary" onclick="toggleTalkBack()" style="margin-top: 10px;">
    🎙️ Start Talking
</button>

```

### Summary of Data Flow

1. **You speak** → Browser Mic (`getUserMedia`)
2. **Browser** → Server (WebRTC `opus` track)
3. **Server** → Decodes `opus` to `PCM`
4. **Server** → DevKit IP:5002 (UDP)
5. **DevKit** → Speaker (`tdl_audio_play`)