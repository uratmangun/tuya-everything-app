# VPS TTS Tunnel to T5AI DevKit

## Overview

This plan describes how to tunnel TTS (Text-to-Speech) audio from a VPS web application to your T5AI DevKit for playback through the attached speaker GPIO.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              VPS SERVER                                      │
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐         │
│  │   Web App       │    │   TTS Service   │    │  TCP/WS Server  │         │
│  │  (Form + API)   │───►│  (Google/Azure/ │───►│  (Audio Stream) │         │
│  │   Port 3000     │    │   Tuya Cloud)   │    │   Port 5000     │         │
│  └─────────────────┘    └─────────────────┘    └────────┬────────┘         │
└───────────────────────────────────────────────────────────┼─────────────────┘
                                                            │
                                    Internet / TCP Tunnel   │
                                                            │
┌───────────────────────────────────────────────────────────┼─────────────────┐
│                         T5AI DevKit (Local Network)       │                 │
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────▼─────────┐       │
│  │  Speaker GPIO   │◄───│  Audio Player   │◄───│  TCP/WS Client    │       │
│  │  (GPIO39)       │    │  (ai_audio)     │    │  (Connects to VPS)│       │
│  └─────────────────┘    └─────────────────┘    └───────────────────┘       │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Data Flow

1. **User** enters text in web form on VPS
2. **Web App** sends text to TTS service via API
3. **TTS Service** generates MP3 audio
4. **VPS Server** streams MP3 to DevKit via TCP tunnel
5. **DevKit** receives and plays audio through speaker GPIO

---

## Phase 1: VPS Web Application

### 1.1 Project Structure

```
vps-tts-server/
├── package.json
├── server.js           # Express + TCP server
├── public/
│   └── index.html      # Web form UI
├── tts/
│   ├── google-tts.js   # Google Cloud TTS
│   ├── azure-tts.js    # Azure Cognitive Services
│   └── tuya-tts.js     # Tuya Cloud TTS (if available)
└── .env                # API keys
```

### 1.2 Web Form (public/index.html)

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>TTS to DevKit</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Inter', system-ui, sans-serif;
            background: linear-gradient(135deg, #0f172a 0%, #1e293b 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            color: #f8fafc;
        }
        .container {
            background: rgba(30, 41, 59, 0.8);
            backdrop-filter: blur(10px);
            border: 1px solid rgba(148, 163, 184, 0.1);
            border-radius: 16px;
            padding: 2rem;
            width: 100%;
            max-width: 500px;
            box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5);
        }
        h1 {
            font-size: 1.5rem;
            margin-bottom: 1.5rem;
            text-align: center;
            background: linear-gradient(90deg, #06b6d4, #22d3ee);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .status {
            padding: 0.75rem;
            border-radius: 8px;
            margin-bottom: 1rem;
            font-size: 0.875rem;
            display: flex;
            align-items: center;
            gap: 0.5rem;
        }
        .status.connected { background: rgba(34, 197, 94, 0.2); color: #4ade80; }
        .status.disconnected { background: rgba(239, 68, 68, 0.2); color: #f87171; }
        .status-dot {
            width: 8px; height: 8px;
            border-radius: 50%;
            animation: pulse 2s infinite;
        }
        .connected .status-dot { background: #4ade80; }
        .disconnected .status-dot { background: #f87171; }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        textarea {
            width: 100%;
            padding: 1rem;
            border: 1px solid rgba(148, 163, 184, 0.2);
            border-radius: 8px;
            background: rgba(15, 23, 42, 0.5);
            color: #f8fafc;
            font-size: 1rem;
            resize: vertical;
            min-height: 120px;
            margin-bottom: 1rem;
        }
        textarea:focus {
            outline: none;
            border-color: #06b6d4;
            box-shadow: 0 0 0 3px rgba(6, 182, 212, 0.2);
        }
        button {
            width: 100%;
            padding: 1rem;
            border: none;
            border-radius: 8px;
            background: linear-gradient(90deg, #0891b2, #06b6d4);
            color: white;
            font-size: 1rem;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        button:hover:not(:disabled) {
            transform: translateY(-2px);
            box-shadow: 0 10px 20px -10px rgba(6, 182, 212, 0.5);
        }
        button:disabled {
            opacity: 0.6;
            cursor: not-allowed;
        }
        .log {
            margin-top: 1rem;
            padding: 1rem;
            background: rgba(15, 23, 42, 0.5);
            border-radius: 8px;
            font-family: monospace;
            font-size: 0.75rem;
            max-height: 150px;
            overflow-y: auto;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🔊 TTS to DevKit</h1>
        <div id="status" class="status disconnected">
            <span class="status-dot"></span>
            <span>DevKit: Disconnected</span>
        </div>
        <textarea id="text" placeholder="Enter text to speak..."></textarea>
        <button id="send" onclick="sendTTS()">Send to DevKit</button>
        <div id="log" class="log"></div>
    </div>

    <script>
        const statusEl = document.getElementById('status');
        const logEl = document.getElementById('log');
        const sendBtn = document.getElementById('send');
        
        function log(msg) {
            const time = new Date().toLocaleTimeString();
            logEl.innerHTML = `[${time}] ${msg}<br>` + logEl.innerHTML;
        }

        async function checkStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                if (data.connected) {
                    statusEl.className = 'status connected';
                    statusEl.innerHTML = '<span class="status-dot"></span><span>DevKit: Connected</span>';
                } else {
                    statusEl.className = 'status disconnected';
                    statusEl.innerHTML = '<span class="status-dot"></span><span>DevKit: Disconnected</span>';
                }
            } catch (e) {
                log('Status check failed: ' + e.message);
            }
        }

        async function sendTTS() {
            const text = document.getElementById('text').value.trim();
            if (!text) return;
            
            sendBtn.disabled = true;
            sendBtn.textContent = 'Sending...';
            log('Sending: ' + text.substring(0, 50) + '...');
            
            try {
                const res = await fetch('/api/tts', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ text })
                });
                const data = await res.json();
                if (data.success) {
                    log('✓ Audio sent to DevKit');
                } else {
                    log('✗ Error: ' + data.error);
                }
            } catch (e) {
                log('✗ Failed: ' + e.message);
            } finally {
                sendBtn.disabled = false;
                sendBtn.textContent = 'Send to DevKit';
            }
        }

        setInterval(checkStatus, 3000);
        checkStatus();
    </script>
</body>
</html>
```

### 1.3 Server (server.js)

```javascript
const express = require('express');
const net = require('net');
const path = require('path');
require('dotenv').config();

// Choose TTS provider
const tts = require('./tts/google-tts'); // or azure-tts, tuya-tts

const app = express();
const PORT = process.env.PORT || 3000;
const TCP_PORT = process.env.TCP_PORT || 5000;

app.use(express.json());
app.use(express.static('public'));

// Track connected DevKit
let devkitSocket = null;

// TCP Server for DevKit connection
const tcpServer = net.createServer((socket) => {
    console.log('DevKit connected:', socket.remoteAddress);
    devkitSocket = socket;
    
    socket.on('close', () => {
        console.log('DevKit disconnected');
        devkitSocket = null;
    });
    
    socket.on('error', (err) => {
        console.error('Socket error:', err.message);
        devkitSocket = null;
    });
});

tcpServer.listen(TCP_PORT, '0.0.0.0', () => {
    console.log(`TCP Server listening on port ${TCP_PORT}`);
});

// API Endpoints
app.get('/api/status', (req, res) => {
    res.json({ connected: devkitSocket !== null });
});

app.post('/api/tts', async (req, res) => {
    const { text } = req.body;
    
    if (!text) {
        return res.status(400).json({ success: false, error: 'No text provided' });
    }
    
    if (!devkitSocket) {
        return res.status(503).json({ success: false, error: 'DevKit not connected' });
    }
    
    try {
        // Generate TTS audio (MP3)
        console.log('Generating TTS for:', text);
        const audioBuffer = await tts.synthesize(text);
        console.log('Audio generated:', audioBuffer.length, 'bytes');
        
        // Send to DevKit using protocol
        // Header: [CMD:1byte][LENGTH:4bytes][DATA:Nbytes]
        const CMD_AUDIO_START = 0x01;
        const CMD_AUDIO_DATA = 0x02;
        const CMD_AUDIO_STOP = 0x03;
        
        // Send start command
        const startHeader = Buffer.alloc(5);
        startHeader.writeUInt8(CMD_AUDIO_START, 0);
        startHeader.writeUInt32LE(0, 1);
        devkitSocket.write(startHeader);
        
        // Send audio data in chunks
        const CHUNK_SIZE = 4096;
        for (let i = 0; i < audioBuffer.length; i += CHUNK_SIZE) {
            const chunk = audioBuffer.slice(i, Math.min(i + CHUNK_SIZE, audioBuffer.length));
            const dataHeader = Buffer.alloc(5);
            dataHeader.writeUInt8(CMD_AUDIO_DATA, 0);
            dataHeader.writeUInt32LE(chunk.length, 1);
            devkitSocket.write(Buffer.concat([dataHeader, chunk]));
            
            // Small delay to prevent buffer overflow
            await new Promise(r => setTimeout(r, 10));
        }
        
        // Send stop command
        const stopHeader = Buffer.alloc(5);
        stopHeader.writeUInt8(CMD_AUDIO_STOP, 0);
        stopHeader.writeUInt32LE(0, 1);
        devkitSocket.write(stopHeader);
        
        res.json({ success: true, audioSize: audioBuffer.length });
    } catch (err) {
        console.error('TTS error:', err);
        res.status(500).json({ success: false, error: err.message });
    }
});

app.listen(PORT, () => {
    console.log(`Web server running on http://localhost:${PORT}`);
});
```

### 1.4 TTS Providers

#### Google Cloud TTS (tts/google-tts.js)

```javascript
const textToSpeech = require('@google-cloud/text-to-speech');

const client = new textToSpeech.TextToSpeechClient();

async function synthesize(text) {
    const [response] = await client.synthesizeSpeech({
        input: { text },
        voice: { languageCode: 'en-US', ssmlGender: 'NEUTRAL' },
        audioConfig: { 
            audioEncoding: 'MP3',
            sampleRateHertz: 16000  // Match T5AI hardware
        },
    });
    return response.audioContent;
}

module.exports = { synthesize };
```

#### gTTS (Free Alternative) - tts/gtts.js

```javascript
const gtts = require('gtts');
const { Readable } = require('stream');

async function synthesize(text) {
    return new Promise((resolve, reject) => {
        const speech = new gtts(text, 'en');
        const chunks = [];
        
        speech.stream()
            .on('data', chunk => chunks.push(chunk))
            .on('end', () => resolve(Buffer.concat(chunks)))
            .on('error', reject);
    });
}

module.exports = { synthesize };
```

---

## Phase 2: DevKit Firmware (TTS TCP Client)

### 2.1 File Structure

Extend your existing `object_detection` app or create a new app:

```
apps/tuya_cloud/object_detection/src/
├── tuya_main.c           # Existing - add TCP client init
├── tts_tcp_client.c      # NEW - TCP client for VPS
├── tts_tcp_client.h      # NEW - Header file
└── ...
```

### 2.2 TTS TCP Client Header (tts_tcp_client.h)

```c
#ifndef __TTS_TCP_CLIENT_H__
#define __TTS_TCP_CLIENT_H__

#include "tuya_cloud_types.h"

/* VPS Server Configuration - set via .env or hardcode */
#define TTS_SERVER_HOST     "YOUR_VPS_IP"
#define TTS_SERVER_PORT     5000

/* Protocol commands */
#define TCP_CMD_AUDIO_START     0x01
#define TCP_CMD_AUDIO_DATA      0x02
#define TCP_CMD_AUDIO_STOP      0x03
#define TCP_CMD_SET_VOLUME      0x04
#define TCP_CMD_HEARTBEAT       0x05

/* Buffer sizes */
#define TTS_RECV_BUF_SIZE       4096

/**
 * @brief Initialize TTS TCP client
 * @return OPRT_OK on success
 */
OPERATE_RET tts_tcp_client_init(void);

/**
 * @brief Start TCP client task (connects to VPS)
 * @return OPRT_OK on success
 */
OPERATE_RET tts_tcp_client_start(void);

/**
 * @brief Stop TCP client
 */
void tts_tcp_client_stop(void);

/**
 * @brief Check if connected to VPS
 * @return true if connected
 */
bool tts_tcp_client_is_connected(void);

#endif /* __TTS_TCP_CLIENT_H__ */
```

### 2.3 TTS TCP Client Implementation (tts_tcp_client.c)

```c
#include "tts_tcp_client.h"
#include "tal_api.h"
#include "tkl_net.h"
#include "ai_audio_player.h"
#include "ai_audio.h"

/* Command header structure */
typedef struct {
    uint8_t cmd_type;
    uint32_t data_len;
} __attribute__((packed)) TCP_CMD_HEADER_T;

/* Client context */
typedef struct {
    int socket_fd;
    bool connected;
    bool running;
    THREAD_HANDLE thread;
    uint8_t recv_buf[TTS_RECV_BUF_SIZE];
} tts_client_ctx_t;

static tts_client_ctx_t g_ctx = {0};

/**
 * @brief Connect to VPS server
 */
static OPERATE_RET connect_to_vps(void)
{
    OPERATE_RET rt = OPRT_OK;
    
    /* Create TCP socket */
    g_ctx.socket_fd = tkl_net_socket_create(PROTOCOL_TCP);
    if (g_ctx.socket_fd < 0) {
        PR_ERR("Failed to create socket");
        return OPRT_SOCK_ERR;
    }
    
    /* Convert IP string to address */
    TUYA_IP_ADDR_T addr = tkl_net_str2addr(TTS_SERVER_HOST);
    
    /* Connect to VPS */
    rt = tkl_net_connect(g_ctx.socket_fd, addr, TTS_SERVER_PORT);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to connect to VPS %s:%d", TTS_SERVER_HOST, TTS_SERVER_PORT);
        tkl_net_close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return rt;
    }
    
    g_ctx.connected = true;
    PR_NOTICE("Connected to TTS VPS %s:%d", TTS_SERVER_HOST, TTS_SERVER_PORT);
    
    return OPRT_OK;
}

/**
 * @brief Handle incoming audio stream
 */
static void handle_audio_data(uint8_t *data, uint32_t len, bool is_eof)
{
    if (len > 0) {
        ai_audio_player_data_write("tts_stream", data, len, is_eof ? 1 : 0);
    }
}

/**
 * @brief TCP receiver task
 */
static void tts_tcp_receiver_task(void *arg)
{
    TCP_CMD_HEADER_T header;
    int recv_len;
    bool audio_started = false;
    
    while (g_ctx.running) {
        /* Reconnect if disconnected */
        if (!g_ctx.connected) {
            PR_INFO("Attempting to reconnect to VPS...");
            if (connect_to_vps() != OPRT_OK) {
                tal_system_sleep(5000);  /* Wait 5s before retry */
                continue;
            }
        }
        
        /* Receive command header (5 bytes) */
        recv_len = tkl_net_recv(g_ctx.socket_fd, &header, sizeof(header));
        if (recv_len <= 0) {
            PR_WARN("Connection lost, will reconnect...");
            tkl_net_close(g_ctx.socket_fd);
            g_ctx.connected = false;
            continue;
        }
        
        PR_DEBUG("Received cmd: 0x%02X, len: %u", header.cmd_type, header.data_len);
        
        switch (header.cmd_type) {
            case TCP_CMD_AUDIO_START:
                PR_INFO("TTS Audio stream starting...");
                ai_audio_player_start("tts_stream");
                audio_started = true;
                break;
                
            case TCP_CMD_AUDIO_DATA:
                if (header.data_len > 0 && header.data_len <= TTS_RECV_BUF_SIZE) {
                    recv_len = tkl_net_recv(g_ctx.socket_fd, g_ctx.recv_buf, header.data_len);
                    if (recv_len > 0 && audio_started) {
                        handle_audio_data(g_ctx.recv_buf, recv_len, false);
                    }
                }
                break;
                
            case TCP_CMD_AUDIO_STOP:
                PR_INFO("TTS Audio stream complete");
                if (audio_started) {
                    /* Signal end of stream */
                    ai_audio_player_data_write("tts_stream", NULL, 0, 1);
                    audio_started = false;
                }
                break;
                
            case TCP_CMD_SET_VOLUME:
                if (header.data_len == 1) {
                    uint8_t volume;
                    tkl_net_recv(g_ctx.socket_fd, &volume, 1);
                    ai_audio_set_volume(volume);
                    PR_INFO("Volume set to: %d", volume);
                }
                break;
                
            case TCP_CMD_HEARTBEAT:
                /* Respond to keep-alive if needed */
                break;
                
            default:
                PR_WARN("Unknown command: 0x%02X", header.cmd_type);
                break;
        }
    }
    
    if (g_ctx.socket_fd >= 0) {
        tkl_net_close(g_ctx.socket_fd);
    }
    g_ctx.connected = false;
}

OPERATE_RET tts_tcp_client_init(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.socket_fd = -1;
    return OPRT_OK;
}

OPERATE_RET tts_tcp_client_start(void)
{
    if (g_ctx.running) {
        PR_WARN("TTS client already running");
        return OPRT_OK;
    }
    
    g_ctx.running = true;
    
    THREAD_CFG_T cfg = {
        .stackDepth = 4096,
        .priority = 4,
        .thrdname = "tts_tcp_client"
    };
    
    OPERATE_RET rt = tal_thread_create_and_start(
        &g_ctx.thread, NULL, NULL,
        tts_tcp_receiver_task, NULL, &cfg
    );
    
    if (rt != OPRT_OK) {
        PR_ERR("Failed to create TTS client thread: %d", rt);
        g_ctx.running = false;
        return rt;
    }
    
    PR_INFO("TTS TCP client started");
    return OPRT_OK;
}

void tts_tcp_client_stop(void)
{
    g_ctx.running = false;
    
    if (g_ctx.socket_fd >= 0) {
        tkl_net_close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
    }
    
    if (g_ctx.thread) {
        tal_thread_delete(g_ctx.thread);
        g_ctx.thread = NULL;
    }
    
    g_ctx.connected = false;
    PR_INFO("TTS TCP client stopped");
}

bool tts_tcp_client_is_connected(void)
{
    return g_ctx.connected;
}
```

### 2.4 Integration with tuya_main.c

Add to your existing `tuya_main.c`:

```c
/* Add include at top */
#include "tts_tcp_client.h"

/* In user_main(), after network initialization and MQTT connection: */
void user_main(void)
{
    // ... existing initialization code ...
    
    /* Start TTS TCP client after network is ready */
    PR_INFO("Initializing TTS TCP client...");
    tts_tcp_client_init();
    
    /* Start the client (will auto-reconnect) */
    tts_tcp_client_start();
    
    // ... rest of existing code ...
}
```

---

## Phase 3: Deployment

### 3.1 VPS Setup

```bash
# On VPS
cd ~
mkdir vps-tts-server && cd vps-tts-server

# Create files as shown above
# ...

# Install dependencies
npm init -y
npm install express dotenv gtts
# OR for Google Cloud: npm install @google-cloud/text-to-speech

# Create .env
echo "PORT=3000" >> .env
echo "TCP_PORT=5000" >> .env
# Add TTS API keys as needed

# Open firewall
sudo ufw allow 3000/tcp
sudo ufw allow 5000/tcp

# Run with PM2 (production)
npm install -g pm2
pm2 start server.js --name tts-server
pm2 save
```

### 3.2 DevKit Configuration

Update `TTS_SERVER_HOST` in `tts_tcp_client.h` with your VPS IP, then:

```bash
cd /home/uratmangun/CascadeProjects/TuyaOpen

# Build
tos build apps/tuya_cloud/object_detection

# Flash
tos flash apps/tuya_cloud/object_detection -p /dev/ttyUSB0

# Monitor
tos monitor -p /dev/ttyUSB0
```

---

## TTS Service Options

| Service | Cost | Quality | Latency | Setup |
|---------|------|---------|---------|-------|
| **gTTS** | Free | Good | Medium | `npm install gtts` |
| **Google Cloud** | $4/1M chars | Excellent | Low | API key required |
| **Azure** | $4/1M chars | Excellent | Low | API key required |
| **Amazon Polly** | $4/1M chars | Excellent | Low | API key required |
| **Tuya Cloud** | Check pricing | Good | Medium | Already integrated |

### Recommendation

Start with **gTTS** (free) for testing, then upgrade to Google Cloud or Azure for production quality.

---

## Security Considerations

1. **TLS/SSL**: Consider adding TLS encryption for production
2. **Authentication**: Add a simple token handshake
3. **Rate Limiting**: Limit API requests to prevent abuse
4. **Input Sanitization**: Validate text input length/content

---

## Next Steps

1. [ ] Set up VPS with Node.js web app
2. [ ] Choose and configure TTS provider
3. [ ] Add `tts_tcp_client.c/h` to DevKit firmware
4. [ ] Update `tuya_main.c` to start TTS client
5. [ ] Build and flash firmware
6. [ ] Test end-to-end

---

*Document created: 2025-12-18*
*Status: Planning Phase*
