# WebSocket Communication - Implementation Guide

## Overview

WebSocket provides real-time bidirectional communication between the browser and server for:
- Sending commands to the DevKit (mic on/off, audio play, etc.)
- Receiving DevKit status updates
- Receiving DevKit responses

**Note**: Audio streaming uses WebRTC (not WebSocket) for optimal latency.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│ Browser                                                                         │
│                                                                                 │
│  ┌─────────────────┐                                                            │
│  │ WebSocket       │ ◀──▶ wss://bell.uratmangun.ovh (via Cloudflare Tunnel)    │
│  │ Client          │                                                            │
│  └────────┬────────┘                                                            │
│           │                                                                     │
│           │  JSON Messages                                                      │
│           │  { type: "send_to_devkit", data: "mic on" }                        │
│           │                                                                     │
└───────────│─────────────────────────────────────────────────────────────────────┘
            │
            │ WSS (WebSocket Secure via Cloudflare)
            │
┌───────────│─────────────────────────────────────────────────────────────────────┐
│ Go Server │                                                                     │
│           ▼                                                                     │
│  ┌─────────────────┐         ┌─────────────────┐         ┌─────────────────┐   │
│  │ WebSocket       │────────▶│ Command Router  │────────▶│ TCP Client      │   │
│  │ Handler         │         │                 │         │ (to DevKit)     │   │
│  └─────────────────┘         └─────────────────┘         └────────┬────────┘   │
│           ▲                                                       │            │
│           │                                                       │            │
│           │  Broadcast                                            │ TCP        │
│           │                                                       │            │
│  ┌────────┴────────┐                                              │            │
│  │ broadcastToWeb  │◀─────────────────────────────────────────────┘            │
│  │ (status updates)│                                                           │
│  └─────────────────┘                                                           │
└─────────────────────────────────────────────────────────────────────────────────┘
            │
            │ TCP (Length-prefixed messages)
            │
┌───────────│─────────────────────────────────────────────────────────────────────┐
│ DevKit    ▼                                                                     │
│  ┌─────────────────┐                                                            │
│  │ TCP Client      │ Receives commands, sends responses                         │
│  └─────────────────┘                                                            │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## Files

| File | Purpose |
|------|---------|
| `webapp/main.go` | WebSocket server handler |
| `webapp/public/index.html` | WebSocket client |

---

## Message Types

### Client → Server

| Type | Data | Description |
|------|------|-------------|
| `ping` | - | Keepalive ping (every 30s) |
| `send_to_devkit` | `{ data: "command" }` | Send command to DevKit |
| `auth` | `{ token: "..." }` | Authenticate (legacy) |

### Server → Client

| Type | Data | Description |
|------|------|-------------|
| `pong` | - | Response to ping |
| `auth_success` | `{ devkit: {...}, serverIP, tcpPort }` | Authentication successful |
| `auth_required` | - | Authentication needed |
| `devkit_status` | `{ connected: bool, address: string }` | DevKit connection status |
| `devkit_message` | `{ data: "response" }` | Response from DevKit |
| `error` | `{ error: "message" }` | Error message |

---

## Server Implementation (main.go)

### WSClient Structure (line 68-73)

```go
type WSClient struct {
    conn          *websocket.Conn
    authenticated bool
    isAlive       bool
    mu            sync.Mutex
}

var (
    wsClients   = make(map[*WSClient]bool)
    wsClientsMu sync.RWMutex
    upgrader    = websocket.Upgrader{
        CheckOrigin: func(r *http.Request) bool { return true },
    }
)
```

### WebSocket Handler (line 517-611)

```go
func handleWebSocket(w http.ResponseWriter, r *http.Request) {
    // Upgrade HTTP to WebSocket
    conn, err := upgrader.Upgrade(w, r, nil)
    if err != nil {
        return
    }

    client := &WSClient{
        conn:          conn,
        authenticated: false,
        isAlive:       true,
    }

    // Check session cookie for authentication
    if cookie, err := r.Cookie("session"); err == nil {
        if sessionID, valid := verifySession(cookie.Value); valid {
            client.authenticated = true
        }
    }

    // Add to clients map
    wsClientsMu.Lock()
    wsClients[client] = true
    wsClientsMu.Unlock()

    defer func() {
        wsClientsMu.Lock()
        delete(wsClients, client)
        wsClientsMu.Unlock()
        conn.Close()
    }()

    // Send initial status
    if client.authenticated {
        sendWSMessage(client, map[string]interface{}{
            "type": "auth_success",
            "devkit": map[string]interface{}{
                "connected": devkitAuthenticated,
            },
            "serverIP": getLocalIP(),
            "tcpPort":  tcpPort,
        })
    }

    // Message loop
    for {
        _, msg, err := conn.ReadMessage()
        if err != nil {
            break
        }

        var data map[string]interface{}
        json.Unmarshal(msg, &data)

        msgType, _ := data["type"].(string)

        switch msgType {
        case "ping":
            sendWSMessage(client, map[string]interface{}{
                "type": "pong",
            })

        case "send_to_devkit":
            if cmd, ok := data["data"].(string); ok {
                if err := sendToDevKit(cmd); err != nil {
                    sendWSMessage(client, map[string]interface{}{
                        "type":  "error",
                        "error": err.Error(),
                    })
                }
            }
        }
    }
}
```

### Broadcast to All Clients (line 619-626)

```go
func broadcastToWeb(data map[string]interface{}) {
    wsClientsMu.RLock()
    defer wsClientsMu.RUnlock()

    for client := range wsClients {
        sendWSMessage(client, data)
    }
}
```

### Usage: DevKit Status Update

When DevKit connects/disconnects, broadcast to all web clients:

```go
// In TCP handler when DevKit connects
broadcastToWeb(map[string]interface{}{
    "type":      "devkit_status",
    "connected": true,
    "address":   conn.RemoteAddr().String(),
})

// When DevKit sends a response
broadcastToWeb(map[string]interface{}{
    "type":      "devkit_message",
    "data":      message,
    "timestamp": time.Now().Format(time.RFC3339),
})
```

---

## Client Implementation (index.html)

### Connection Setup (line 584-630)

```javascript
const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${wsProtocol}//${window.location.host}`;

function connect() {
    ws = new WebSocket(wsUrl);
    let wsPingInterval = null;

    ws.onopen = () => {
        log('info', 'WebSocket connected');
        // Keepalive ping every 30 seconds (prevents Cloudflare timeout)
        wsPingInterval = setInterval(() => {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ type: 'ping' }));
            }
        }, 30000);
    };

    ws.onmessage = (event) => {
        const data = JSON.parse(event.data);
        handleMessage(data);
    };

    ws.onclose = () => {
        if (wsPingInterval) clearInterval(wsPingInterval);
        // Auto-reconnect after 2 seconds
        setTimeout(() => connect(), 2000);
    };
}
```

### Message Handler (line 635-695)

```javascript
function handleMessage(data) {
    switch (data.type) {
        case 'auth_success':
            updateWsStatus(true);
            if (data.devkit) {
                updateDevKitStatus(data.devkit.connected);
            }
            break;

        case 'devkit_status':
            updateDevKitStatus(data.connected, data.address);
            break;

        case 'devkit_message':
            log('received', data.data);
            break;

        case 'pong':
            // Keepalive response (ignore)
            break;

        case 'error':
            log('error', data.error);
            break;
    }
}
```

### Sending Commands (line 697-715)

```javascript
function sendQuickCommand(cmd) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        log('error', 'WebSocket not connected');
        return;
    }

    ws.send(JSON.stringify({
        type: 'send_to_devkit',
        data: cmd
    }));
}

// Usage examples:
sendQuickCommand('mic on');
sendQuickCommand('mic off');
sendQuickCommand('audio play');
sendQuickCommand('status');
```

---

## Message Flow Diagrams

### Command Flow (Browser → DevKit)

```
Browser                    Server                     DevKit
   │                         │                          │
   │  WS: send_to_devkit     │                          │
   │  { data: "mic on" }     │                          │
   │ ───────────────────────▶│                          │
   │                         │                          │
   │                         │  TCP: [len][mic on]      │
   │                         │ ────────────────────────▶│
   │                         │                          │
   │                         │  TCP: [len][ok:mic_on]   │
   │                         │ ◀────────────────────────│
   │                         │                          │
   │  WS: devkit_message     │                          │
   │  { data: "ok:mic_on" }  │                          │
   │ ◀───────────────────────│                          │
   │                         │                          │
```

### Status Broadcast (DevKit → All Browsers)

```
DevKit                     Server                    Browser A    Browser B
   │                         │                          │            │
   │  TCP: [connected]       │                          │            │
   │ ───────────────────────▶│                          │            │
   │                         │                          │            │
   │                         │  WS: devkit_status       │            │
   │                         │  { connected: true }     │            │
   │                         │ ────────────────────────▶│            │
   │                         │ ─────────────────────────────────────▶│
   │                         │                          │            │
```

### Keepalive (Cloudflare Tunnel)

```
Browser                    Cloudflare                 Server
   │                         │                          │
   │  WS: { type: "ping" }   │                          │
   │ ───────────────────────▶│ ────────────────────────▶│
   │                         │                          │
   │                         │  WS: { type: "pong" }    │
   │ ◀───────────────────────│ ◀────────────────────────│
   │                         │                          │
   │      (every 30 seconds to prevent idle timeout)    │
   │                         │                          │
```

---

## Cloudflare Tunnel Considerations

### Idle Timeout
Cloudflare Tunnel closes WebSocket connections after ~100 seconds of inactivity.

**Solution**: Client sends `ping` every 30 seconds.

```javascript
// Client-side keepalive
wsPingInterval = setInterval(() => {
    ws.send(JSON.stringify({ type: 'ping' }));
}, 30000);
```

### Auto-Reconnect
Handle disconnections gracefully:

```javascript
ws.onclose = () => {
    clearInterval(wsPingInterval);
    setTimeout(() => connect(), 2000);  // Reconnect after 2s
};
```

---

## Authentication Flow

```
Browser                    Server
   │                         │
   │  HTTP: GET /            │
   │  Cookie: session=xxx    │
   │ ───────────────────────▶│
   │                         │
   │  HTTP: 200 OK           │
   │  (index.html)           │
   │ ◀───────────────────────│
   │                         │
   │  WS: Upgrade            │
   │  Cookie: session=xxx    │
   │ ───────────────────────▶│
   │                         │
   │  (Server validates      │
   │   session cookie)       │
   │                         │
   │  WS: auth_success       │
   │  { devkit: {...} }      │
   │ ◀───────────────────────│
   │                         │
```

The WebSocket inherits authentication from the HTTP session cookie, so no separate WebSocket authentication is needed.

---

## Available Commands

Commands sent via `send_to_devkit`:

| Command | Description | Response |
|---------|-------------|----------|
| `ping` | Check DevKit alive | `pong` |
| `status` | Get device status | JSON status |
| `mic on` | Start mic streaming | `ok:mic_on` |
| `mic off` | Stop mic streaming | `ok:mic_off` |
| `audio play` | Play alert sound | `ok:audio_playing` |
| `audio stop` | Stop audio | `ok:audio_stopped` |
| `switch on` | Enable detection | `ok:switch_on` |
| `switch off` | Disable detection | `ok:switch_off` |
| `mem` | Get free memory | Memory info |
| `reset` | Factory reset | Device resets |

---

## Error Handling

### Connection Errors

```javascript
ws.onerror = (err) => {
    log('error', 'WebSocket error');
};

ws.onclose = () => {
    updateWsStatus(false);
    // Auto-reconnect
    setTimeout(() => connect(), 2000);
};
```

### Command Errors

```javascript
// Server sends error if DevKit not connected
case 'error':
    log('error', data.error);
    // e.g., "devkit not connected"
    break;
```

---

## Summary

| Aspect | Implementation |
|--------|----------------|
| Protocol | WebSocket (WSS via Cloudflare) |
| Authentication | HTTP session cookie |
| Message Format | JSON |
| Keepalive | Client ping every 30s |
| Reconnect | Auto after 2s delay |
| Purpose | Commands & status (not audio) |
