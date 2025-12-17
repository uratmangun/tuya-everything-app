# Audio Streaming Guide for T5AI DevKit

This guide explains how to stream and play audio on the T5AI DevKit using different transport protocols: TCP, WebSocket, and MQTT.

## Table of Contents

1. [Audio System Overview](#audio-system-overview)
2. [Supported Audio Formats](#supported-audio-formats)
3. [Audio Playback APIs](#audio-playback-apis)
4. [Method 1: TCP Streaming](#method-1-tcp-streaming)
5. [Method 2: WebSocket Streaming](#method-2-websocket-streaming)
6. [Method 3: MQTT Commands](#method-3-mqtt-commands)
7. [VPS Integration Architecture](#vps-integration-architecture)
8. [Code Examples](#code-examples)

---

## Audio System Overview

### Audio Pipeline

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│  Audio Source   │───►│   MP3 Decoder   │───►│   PCM Buffer    │───►│  I2S Hardware   │
│  (MP3 stream)   │    │   (minimp3)     │    │  (16-bit PCM)   │    │  (ES8311 Codec) │
└─────────────────┘    └─────────────────┘    └─────────────────┘    └─────────────────┘
```

### Key Components

| Component | Description | Location |
|-----------|-------------|----------|
| `ai_audio_player` | High-level MP3 streaming player | `apps/tuya.ai/ai_components/ai_audio/` |
| `minimp3` | MP3 decoder library | `apps/tuya.ai/ai_components/ai_audio/minimp3/` |
| `tdl_audio` | Driver layer for audio hardware | `src/peripherals/audio_codecs/tdl_audio/` |
| `tkl_audio` | Kernel layer audio interface | Platform-specific |

---

## Supported Audio Formats

### Input Formats

| Format | Support | Notes |
|--------|---------|-------|
| **MP3** | ✅ Native | Recommended for streaming (smaller bandwidth) |
| **WAV** | ⚠️ Convert | Must convert to MP3 or raw PCM |
| **Raw PCM** | ✅ Direct | Use lower-level APIs (`tkl_ao_put_frame`) |

### Audio Specifications

| Parameter | Value | Notes |
|-----------|-------|-------|
| Sample Rate | 16 kHz | Hardware configured for 16kHz |
| Bit Depth | 16-bit | Signed 16-bit samples |
| Channels | Mono/Stereo | Mono recommended for voice |
| Encoding | Little-endian | For raw PCM data |

### Converting WAV to MP3

```bash
# Using FFmpeg on VPS
ffmpeg -i input.wav -ar 16000 -ac 1 -b:a 128k output.mp3

# Parameters:
# -ar 16000  : Sample rate 16kHz
# -ac 1      : Mono channel
# -b:a 128k  : Audio bitrate 128kbps
```

### Converting WAV to Raw PCM

```bash
# Using FFmpeg on VPS
ffmpeg -i input.wav -f s16le -ar 16000 -ac 1 output.pcm

# Parameters:
# -f s16le   : Signed 16-bit little-endian
# -ar 16000  : Sample rate 16kHz
# -ac 1      : Mono channel
```

---

## Audio Playback APIs

### High-Level API (MP3 Streaming)

```c
#include "ai_audio_player.h"

// Initialize the audio player
OPERATE_RET ai_audio_player_init(void);

// Start playback session
OPERATE_RET ai_audio_player_start(char *id);

// Write MP3 data to the stream buffer
// is_eof = 1 for the last chunk
OPERATE_RET ai_audio_player_data_write(char *id, uint8_t *data, uint32_t len, uint8_t is_eof);

// Stop playback
OPERATE_RET ai_audio_player_stop(void);

// Check if currently playing
uint8_t ai_audio_player_is_playing(void);
```

### Lower-Level API (Direct PCM Output)

```c
#include "tkl_audio.h"

// Initialize audio hardware
OPERATE_RET tkl_ai_init(TKL_AUDIO_CONFIG_T *config, int count);

// Start audio input/output
OPERATE_RET tkl_ai_start(int card, int chn);

// Put PCM frame directly to speaker
OPERATE_RET tkl_ao_put_frame(int card, int chn, void *handle, TKL_AUDIO_FRAME_INFO_T *frame);

// Set speaker volume (0-100)
OPERATE_RET tkl_ao_set_vol(int card, int chn, void *handle, int vol);
```

### TDL Audio API (Driver Layer)

```c
#include "tdl_audio_manage.h"

// Find audio device by name
OPERATE_RET tdl_audio_find(char *name, TDL_AUDIO_HANDLE_T *handle);

// Open audio device
OPERATE_RET tdl_audio_open(TDL_AUDIO_HANDLE_T handle, TDL_AUDIO_MIC_CB mic_cb);

// Play PCM audio data
OPERATE_RET tdl_audio_play(TDL_AUDIO_HANDLE_T handle, uint8_t *data, uint32_t len);

// Stop playback
OPERATE_RET tdl_audio_play_stop(TDL_AUDIO_HANDLE_T handle);

// Set volume
OPERATE_RET tdl_audio_volume_set(TDL_AUDIO_HANDLE_T handle, uint8_t volume);
```

---

## Method 1: TCP Streaming

### Architecture

```
┌─────────────────┐         TCP Tunnel         ┌─────────────────┐
│      VPS        │◄─────────────────────────►│    T5AI DevKit  │
│                 │                            │                 │
│  TCP Server     │   Binary audio stream      │  TCP Client     │
│  Port: 5000     │ ─────────────────────────► │  Ring Buffer    │
│                 │                            │  MP3 Decoder    │
└─────────────────┘                            └─────────────────┘
```

### Protocol Definition

```c
// Command header structure
typedef struct {
    uint8_t cmd_type;      // Command type
    uint32_t data_len;     // Length of following data
} __attribute__((packed)) TCP_CMD_HEADER_T;

// Command types
#define TCP_CMD_AUDIO_START     0x01  // Start audio playback
#define TCP_CMD_AUDIO_DATA      0x02  // Audio data chunk
#define TCP_CMD_AUDIO_STOP      0x03  // Stop playback
#define TCP_CMD_SET_VOLUME      0x04  // Set volume (data = uint8_t volume)
#define TCP_CMD_HEARTBEAT       0x05  // Keep-alive
```

### DevKit TCP Client Implementation

```c
#include "tkl_net.h"
#include "ai_audio_player.h"

#define VPS_SERVER_IP    "YOUR_VPS_IP"
#define VPS_SERVER_PORT  5000
#define RECV_BUF_SIZE    4096

static int g_tcp_socket = -1;
static bool g_connected = false;

OPERATE_RET tcp_audio_init(void)
{
    // Initialize audio player
    return ai_audio_player_init();
}

OPERATE_RET tcp_connect_to_vps(void)
{
    OPERATE_RET rt = OPRT_OK;
    
    // Create TCP socket
    g_tcp_socket = tkl_net_socket_create(PROTOCOL_TCP);
    if (g_tcp_socket < 0) {
        PR_ERR("Failed to create socket");
        return OPRT_COM_ERROR;
    }
    
    // Convert IP string to address
    TUYA_IP_ADDR_T addr = tkl_net_str2addr(VPS_SERVER_IP);
    
    // Connect to VPS server
    rt = tkl_net_connect(g_tcp_socket, addr, VPS_SERVER_PORT);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to connect to VPS: %d", rt);
        tkl_net_close(g_tcp_socket);
        return rt;
    }
    
    g_connected = true;
    PR_NOTICE("Connected to VPS %s:%d", VPS_SERVER_IP, VPS_SERVER_PORT);
    
    return OPRT_OK;
}

static void tcp_audio_receiver_task(void *arg)
{
    TCP_CMD_HEADER_T header;
    uint8_t *recv_buf = tkl_system_malloc(RECV_BUF_SIZE);
    int recv_len;
    
    while (g_connected) {
        // Receive command header
        recv_len = tkl_net_recv(g_tcp_socket, &header, sizeof(header));
        if (recv_len <= 0) {
            PR_ERR("Connection lost");
            break;
        }
        
        switch (header.cmd_type) {
            case TCP_CMD_AUDIO_START:
                PR_NOTICE("Audio start command received");
                ai_audio_player_start(NULL);
                break;
                
            case TCP_CMD_AUDIO_DATA:
                if (header.data_len > 0 && header.data_len <= RECV_BUF_SIZE) {
                    recv_len = tkl_net_recv(g_tcp_socket, recv_buf, header.data_len);
                    if (recv_len > 0) {
                        // Write MP3 data to player
                        // is_eof = 0 for intermediate chunks
                        ai_audio_player_data_write(NULL, recv_buf, recv_len, 0);
                    }
                }
                break;
                
            case TCP_CMD_AUDIO_STOP:
                PR_NOTICE("Audio stop command received");
                // Signal end of stream
                ai_audio_player_data_write(NULL, NULL, 0, 1);
                break;
                
            case TCP_CMD_SET_VOLUME:
                if (header.data_len == 1) {
                    uint8_t volume;
                    tkl_net_recv(g_tcp_socket, &volume, 1);
                    tkl_ao_set_vol(TKL_AUDIO_TYPE_BOARD, 0, NULL, volume);
                }
                break;
                
            case TCP_CMD_HEARTBEAT:
                // Respond to keep-alive
                break;
        }
    }
    
    tkl_system_free(recv_buf);
    tkl_net_close(g_tcp_socket);
    g_connected = false;
}
```

### VPS TCP Server (Python)

```python
#!/usr/bin/env python3
"""
VPS TCP Server for T5AI Audio Streaming
"""

import socket
import struct
import time
from pathlib import Path

# Command types
TCP_CMD_AUDIO_START = 0x01
TCP_CMD_AUDIO_DATA = 0x02
TCP_CMD_AUDIO_STOP = 0x03
TCP_CMD_SET_VOLUME = 0x04
TCP_CMD_HEARTBEAT = 0x05

CHUNK_SIZE = 4096

class AudioTCPServer:
    def __init__(self, host='0.0.0.0', port=5000):
        self.host = host
        self.port = port
        self.server = None
        self.client = None
        
    def start(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind((self.host, self.port))
        self.server.listen(1)
        print(f"TCP Server listening on {self.host}:{self.port}")
        
    def wait_for_device(self):
        print("Waiting for T5AI device connection...")
        self.client, addr = self.server.accept()
        print(f"Device connected from {addr}")
        return addr
        
    def send_command(self, cmd_type, data=b''):
        """Send a command with optional data"""
        header = struct.pack('<BI', cmd_type, len(data))
        self.client.send(header + data)
        
    def play_audio_file(self, file_path):
        """Stream an MP3 file to the device"""
        if not Path(file_path).exists():
            print(f"Error: File not found: {file_path}")
            return False
            
        print(f"Playing: {file_path}")
        
        # Send start command
        self.send_command(TCP_CMD_AUDIO_START)
        time.sleep(0.1)  # Give device time to prepare
        
        # Stream audio data
        with open(file_path, 'rb') as f:
            chunk_count = 0
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                self.send_command(TCP_CMD_AUDIO_DATA, chunk)
                chunk_count += 1
                # Small delay to prevent buffer overflow
                time.sleep(0.01)
                
        print(f"Sent {chunk_count} chunks")
        
        # Send stop command
        self.send_command(TCP_CMD_AUDIO_STOP)
        print("Playback complete")
        return True
        
    def set_volume(self, volume):
        """Set device volume (0-100)"""
        volume = max(0, min(100, volume))
        self.send_command(TCP_CMD_SET_VOLUME, bytes([volume]))
        print(f"Volume set to {volume}")
        
    def close(self):
        if self.client:
            self.client.close()
        if self.server:
            self.server.close()

# Usage example
if __name__ == '__main__':
    server = AudioTCPServer(port=5000)
    server.start()
    
    try:
        server.wait_for_device()
        server.set_volume(70)
        server.play_audio_file('/path/to/audio.mp3')
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        server.close()
```

---

## Method 2: WebSocket Streaming

### Architecture

```
┌─────────────────┐       WebSocket           ┌─────────────────┐
│      VPS        │◄─────────────────────────►│    T5AI DevKit  │
│                 │                            │                 │
│  WS Server      │   JSON commands +          │  WS Client      │
│  wss://         │   Binary audio frames      │  Voice Protocol │
│                 │ ─────────────────────────► │                 │
└─────────────────┘                            └─────────────────┘
```

### TuyaOpen WebSocket Voice Protocol

The TuyaOpen framework includes a WebSocket-based voice protocol that can be leveraged:

```c
#include "tuya_voice_protocol_ws.h"

// Start WebSocket client connection
OPERATE_RET tuya_voice_proto_ws_client_start(TUYA_VOICE_CBS_T *cbs);

// Stop WebSocket client
OPERATE_RET tuya_voice_proto_ws_client_stop(void);

// Send control command
OPERATE_RET tuya_voice_proto_ws_control(char *action, char *text);

// Request TTS audio
OPERATE_RET tuya_voice_proto_ws_get_tts_audio(char *text);

// Interrupt current voice request
OPERATE_RET tuya_voice_proto_ws_interrupt(void);
```

### Custom WebSocket Implementation

For custom WebSocket streaming, you can use the underlying WebSocket module:

```c
#include "websocket_client.h"

// WebSocket message types
#define WS_MSG_TYPE_TEXT    0x01
#define WS_MSG_TYPE_BINARY  0x02

typedef struct {
    char *url;                      // WebSocket URL (ws:// or wss://)
    void (*on_open)(void);          // Connection opened callback
    void (*on_message)(uint8_t *data, uint32_t len, uint8_t type);  // Message received
    void (*on_close)(int code);     // Connection closed callback
    void (*on_error)(int error);    // Error callback
} WS_CLIENT_CONFIG_T;

// Example: Custom WebSocket audio receiver
static void on_ws_message(uint8_t *data, uint32_t len, uint8_t type)
{
    if (type == WS_MSG_TYPE_BINARY) {
        // Binary data = audio stream
        ai_audio_player_data_write(NULL, data, len, 0);
    } else if (type == WS_MSG_TYPE_TEXT) {
        // JSON command
        // Parse and handle commands like:
        // {"cmd": "play", "id": "audio_123"}
        // {"cmd": "stop"}
        // {"cmd": "volume", "level": 80}
    }
}
```

### VPS WebSocket Server (Node.js)

```javascript
// VPS WebSocket Server for T5AI Audio Streaming
const WebSocket = require('ws');
const fs = require('fs');

const wss = new WebSocket.Server({ port: 8080 });

wss.on('connection', (ws) => {
    console.log('T5AI device connected');
    
    ws.on('message', (message) => {
        const data = JSON.parse(message);
        console.log('Received:', data);
        
        // Handle device commands
        if (data.type === 'ready') {
            console.log('Device ready for audio');
        }
    });
    
    ws.on('close', () => {
        console.log('Device disconnected');
    });
});

// Function to stream audio to device
function playAudio(ws, filePath) {
    // Send start command
    ws.send(JSON.stringify({ cmd: 'play', id: 'audio_1' }));
    
    // Read and stream MP3 file
    const stream = fs.createReadStream(filePath, { highWaterMark: 4096 });
    
    stream.on('data', (chunk) => {
        // Send binary audio data
        ws.send(chunk, { binary: true });
    });
    
    stream.on('end', () => {
        // Send stop command
        ws.send(JSON.stringify({ cmd: 'stop', id: 'audio_1' }));
        console.log('Audio stream complete');
    });
}

// Usage: playAudio(clientWs, '/path/to/audio.mp3');
```

---

## Method 3: MQTT Commands

### Architecture

```
┌─────────────────┐                           ┌─────────────────┐
│      VPS        │                           │    T5AI DevKit  │
│                 │                           │                 │
│  MQTT Publisher │──────────────────────────►│  MQTT Subscriber│
│                 │     MQTT Broker           │                 │
│                 │    (mosquitto/etc)        │  Voice Protocol │
└─────────────────┘                           └─────────────────┘
```

### TuyaOpen MQTT Voice Protocol

The TuyaOpen framework includes MQTT-based voice control:

```c
#include "tuya_voice_proto_mqtt.h"

// Report audio playback progress
OPERATE_RET tuya_voice_proto_mqtt_audio_report_progress(int progress);

// Request next audio
OPERATE_RET tuya_voice_proto_mqtt_audio_request_next(void);

// Request previous audio
OPERATE_RET tuya_voice_proto_mqtt_audio_request_prev(void);

// Request current audio info
OPERATE_RET tuya_voice_proto_mqtt_audio_request_current(void);

// Request to play music
OPERATE_RET tuya_voice_proto_mqtt_audio_request_playmusic(void);

// Request TTS playback
OPERATE_RET tuya_voice_proto_mqtt_tts_get(char *content);

// Request bell/alert sound
OPERATE_RET tuya_voice_proto_mqtt_bell_request(void);
```

### MQTT Message Formats

```json
// Play TTS
{
    "type": "playTts",
    "data": {
        "text": "Hello, this is a test",
        "url": "https://example.com/tts.mp3",
        "format": "mp3"
    }
}

// Play Audio from URL
{
    "type": "playAudio",
    "data": {
        "url": "https://example.com/audio.mp3",
        "title": "My Audio",
        "format": "mp3"
    }
}

// Control Commands
{
    "type": "control",
    "action": "pause"  // or "resume", "stop", "next", "prev"
}

// Volume Control
{
    "type": "volume",
    "level": 80
}
```

### VPS MQTT Publisher (Python)

```python
#!/usr/bin/env python3
"""
VPS MQTT Publisher for T5AI Audio Control
"""

import paho.mqtt.client as mqtt
import json
import time

# MQTT Configuration
MQTT_BROKER = "your-mqtt-broker.com"
MQTT_PORT = 1883
MQTT_TOPIC_CMD = "tuya/device/YOUR_DEVICE_ID/cmd"
MQTT_TOPIC_STATUS = "tuya/device/YOUR_DEVICE_ID/status"

class AudioMQTTController:
    def __init__(self):
        self.client = mqtt.Client()
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        
    def on_connect(self, client, userdata, flags, rc):
        print(f"Connected to MQTT broker with result code {rc}")
        client.subscribe(MQTT_TOPIC_STATUS)
        
    def on_message(self, client, userdata, msg):
        print(f"Status: {msg.payload.decode()}")
        
    def connect(self):
        self.client.connect(MQTT_BROKER, MQTT_PORT, 60)
        self.client.loop_start()
        
    def play_tts(self, text):
        """Send TTS playback command"""
        payload = {
            "type": "playTts",
            "data": {
                "text": text,
                "format": "mp3"
            }
        }
        self.client.publish(MQTT_TOPIC_CMD, json.dumps(payload))
        print(f"TTS command sent: {text}")
        
    def play_audio_url(self, url, title="Audio"):
        """Send audio URL playback command"""
        payload = {
            "type": "playAudio",
            "data": {
                "url": url,
                "title": title,
                "format": "mp3"
            }
        }
        self.client.publish(MQTT_TOPIC_CMD, json.dumps(payload))
        print(f"Play audio command sent: {url}")
        
    def set_volume(self, level):
        """Set device volume"""
        payload = {
            "type": "volume",
            "level": level
        }
        self.client.publish(MQTT_TOPIC_CMD, json.dumps(payload))
        print(f"Volume set to: {level}")
        
    def control(self, action):
        """Send control command (pause/resume/stop/next/prev)"""
        payload = {
            "type": "control",
            "action": action
        }
        self.client.publish(MQTT_TOPIC_CMD, json.dumps(payload))
        print(f"Control command sent: {action}")
        
    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()

# Usage example
if __name__ == '__main__':
    controller = AudioMQTTController()
    controller.connect()
    
    time.sleep(1)  # Wait for connection
    
    controller.set_volume(70)
    controller.play_tts("Hello from the VPS server")
    
    time.sleep(5)
    
    controller.play_audio_url("https://example.com/audio.mp3", "Test Audio")
    
    controller.disconnect()
```

---

## VPS Integration Architecture

### Complete System Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              VPS SERVER                                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │  Web App    │  │   REST API  │  │  Audio      │  │  Protocol   │    │
│  │  (Frontend) │──│   Server    │──│  Processor  │──│  Handler    │    │
│  └─────────────┘  └─────────────┘  └─────────────┘  └──────┬──────┘    │
│                                                             │           │
│                    ┌────────────────────────────────────────┼───────┐   │
│                    │              │              │          │       │   │
│                    ▼              ▼              ▼          │       │   │
│              ┌──────────┐  ┌──────────┐  ┌──────────┐       │       │   │
│              │   TCP    │  │WebSocket │  │  MQTT    │       │       │   │
│              │ :5000    │  │ :8080    │  │ Pub/Sub  │       │       │   │
│              └────┬─────┘  └────┬─────┘  └────┬─────┘       │       │   │
└───────────────────┼─────────────┼─────────────┼─────────────┼───────┘   │
                    │             │             │             │           │
                    │     Internet / VPN / Tunnel             │           │
                    │             │             │             │           │
┌───────────────────┼─────────────┼─────────────┼─────────────┼───────────┘
│                   ▼             ▼             ▼             │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                    T5AI DevKit                       │    │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐          │    │
│  │  │TCP Client│  │WS Client │  │MQTT Sub  │          │    │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘          │    │
│  │       │             │             │                 │    │
│  │       ▼             ▼             ▼                 │    │
│  │  ┌──────────────────────────────────────────┐      │    │
│  │  │         Command Dispatcher                │      │    │
│  │  └────────────────────┬─────────────────────┘      │    │
│  │                       │                             │    │
│  │       ┌───────────────┼───────────────┐            │    │
│  │       ▼               ▼               ▼            │    │
│  │  ┌─────────┐    ┌──────────┐    ┌──────────┐      │    │
│  │  │  Audio  │    │  Volume  │    │  Other   │      │    │
│  │  │  Player │    │  Control │    │ Controls │      │    │
│  │  └────┬────┘    └──────────┘    └──────────┘      │    │
│  │       │                                            │    │
│  │       ▼                                            │    │
│  │  ┌──────────────────────────────────────────┐     │    │
│  │  │    ES8311 Audio Codec → Speaker          │     │    │
│  │  └──────────────────────────────────────────┘     │    │
│  └───────────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────────┘
```

### Protocol Comparison

| Feature | TCP | WebSocket | MQTT |
|---------|-----|-----------|------|
| **Latency** | Lowest | Low | Medium |
| **Complexity** | Simple | Medium | Higher |
| **Bidirectional** | Yes | Yes | Pub/Sub |
| **Firewall Friendly** | ⚠️ | ✅ (HTTP upgrade) | ⚠️ |
| **Binary Support** | ✅ Native | ✅ Native | ⚠️ Base64 |
| **Reconnection** | Manual | Auto (with libs) | Auto |
| **Best For** | Raw streaming | Real-time bidirectional | Command & control |

### Recommended Usage

| Use Case | Recommended Protocol |
|----------|---------------------|
| Live audio streaming | TCP or WebSocket |
| TTS playback | MQTT (with URL) or WebSocket |
| Remote control commands | MQTT |
| Low-latency audio | TCP with raw PCM |
| Behind firewalls | WebSocket (wss://) |

---

## Code Examples

### Complete DevKit Application Structure

```
apps/
└── custom_audio_app/
    ├── CMakeLists.txt
    ├── src/
    │   ├── main.c                 # Application entry
    │   ├── tcp_audio_client.c     # TCP streaming client
    │   ├── tcp_audio_client.h
    │   ├── ws_audio_client.c      # WebSocket client
    │   ├── ws_audio_client.h
    │   ├── mqtt_audio_handler.c   # MQTT command handler
    │   ├── mqtt_audio_handler.h
    │   └── audio_manager.c        # Audio playback manager
    └── config/
        └── app_config.h           # Server IPs, ports, etc.
```

### Main Application Entry

```c
// main.c
#include "tal_api.h"
#include "ai_audio_player.h"
#include "tcp_audio_client.h"

void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;
    
    PR_NOTICE("T5AI Audio Streaming App Starting...");
    
    // Initialize audio player
    rt = ai_audio_player_init();
    if (rt != OPRT_OK) {
        PR_ERR("Failed to init audio player: %d", rt);
        return;
    }
    
    // Initialize network
    // ... WiFi/network initialization ...
    
    // Start TCP audio client (connects to VPS)
    rt = tcp_audio_client_start("YOUR_VPS_IP", 5000);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to start TCP client: %d", rt);
    }
    
    PR_NOTICE("Audio streaming app ready");
}
```

---

## Troubleshooting

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| No audio output | Wrong sample rate | Ensure MP3 is 16kHz |
| Choppy audio | Buffer underrun | Increase buffer size or reduce network latency |
| Connection drops | Firewall/NAT | Use WebSocket (wss://) or set up proper port forwarding |
| Volume too low | Codec settings | Adjust `spk_volume` in audio config |

### Debug Logging

```c
// Enable verbose logging in your app
tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

// Check audio player state
PR_DEBUG("Audio player state: %d", sg_player.stat);
PR_DEBUG("Ring buffer used: %d bytes", tuya_ring_buff_used_size_get(sg_player.rb_hdl));
```

---

## References

- TuyaOpen Examples: `examples/multimedia/audio_speaker/`
- AI Audio Components: `apps/tuya.ai/ai_components/ai_audio/`
- Voice Protocols: `apps/tuya.ai/ai_components/tuya_voice/`
- TDL Audio Driver: `src/peripherals/audio_codecs/tdl_audio/`
