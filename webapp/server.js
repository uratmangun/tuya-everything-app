/**
 * DevKit Communication Server with Authentication
 * 
 * This server provides:
 * 1. TCP Server (port 5000) - DevKit connects here (with token auth)
 * 2. HTTP + WebSocket Server (port 3000) - Serves web UI and WebSocket on same port
 * 
 * Security:
 * - HTTP Basic Auth for web UI
 * - Token-based auth for WebSocket and TCP
 */

const express = require('express');
const { WebSocketServer } = require('ws');
const net = require('net');
const path = require('path');
const http = require('http');

// Load environment variables
const AUTH_USERNAME = process.env.AUTH_USERNAME || 'admin';
const AUTH_PASSWORD = process.env.AUTH_PASSWORD || 'changeme123';
const AUTH_TOKEN = process.env.AUTH_TOKEN || 'devkit-secret-token';

// Configuration
const HTTP_PORT = process.env.HTTP_PORT || 3000;
const TCP_PORT = process.env.TCP_PORT || 5000;

// Express app for serving static files
const app = express();

// Create HTTP server from Express app
const server = http.createServer(app);

// Store connections
let devkitSocket = null;
let devkitAuthenticated = false;
let webClients = new Map(); // ws -> { authenticated: bool }

// Get local IP address
const os = require('os');
function getLocalIP() {
    const interfaces = os.networkInterfaces();
    for (const name of Object.keys(interfaces)) {
        for (const iface of interfaces[name]) {
            if (iface.family === 'IPv4' && !iface.internal) {
                return iface.address;
            }
        }
    }
    return '127.0.0.1';
}

const LOCAL_IP = getLocalIP();

// ==================== HTTP Basic Auth Middleware ====================
function basicAuth(req, res, next) {
    const authHeader = req.headers.authorization;

    if (!authHeader) {
        res.setHeader('WWW-Authenticate', 'Basic realm="Tuya DevKit Controller"');
        return res.status(401).send('Authentication required');
    }

    const [type, credentials] = authHeader.split(' ');

    if (type !== 'Basic' || !credentials) {
        return res.status(401).send('Invalid authentication');
    }

    const decoded = Buffer.from(credentials, 'base64').toString('utf8');
    const [username, password] = decoded.split(':');

    if (username === AUTH_USERNAME && password === AUTH_PASSWORD) {
        next();
    } else {
        res.setHeader('WWW-Authenticate', 'Basic realm="Tuya DevKit Controller"');
        return res.status(401).send('Invalid credentials');
    }
}

// Apply auth to all routes except health check
app.use((req, res, next) => {
    if (req.path === '/health') {
        return next();
    }
    basicAuth(req, res, next);
});

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ==================== TCP Server for DevKit ====================
const tcpServer = net.createServer((socket) => {
    const clientInfo = `${socket.remoteAddress}:${socket.remotePort}`;
    console.log(`[TCP] DevKit connecting: ${clientInfo}`);

    // Store the DevKit socket (not authenticated yet)
    devkitSocket = socket;
    devkitAuthenticated = false;

    // Buffer for incomplete messages
    let buffer = Buffer.alloc(0);

    socket.on('data', (data) => {
        // Append new data to buffer
        buffer = Buffer.concat([buffer, data]);

        // Try to parse complete messages
        // Protocol: [LEN:4bytes][DATA:LEN bytes]
        while (buffer.length >= 4) {
            const msgLen = buffer.readUInt32LE(0);

            if (buffer.length >= 4 + msgLen) {
                const message = buffer.slice(4, 4 + msgLen).toString('utf8');
                buffer = buffer.slice(4 + msgLen);

                // Check for auth message
                if (!devkitAuthenticated) {
                    if (message.startsWith('auth:')) {
                        const token = message.substring(5);
                        if (token === AUTH_TOKEN) {
                            devkitAuthenticated = true;
                            console.log(`[TCP] DevKit authenticated: ${clientInfo}`);
                            sendToDevKitRaw('auth:ok');

                            broadcastToWeb({
                                type: 'devkit_status',
                                connected: true,
                                authenticated: true,
                                address: socket.remoteAddress,
                                timestamp: new Date().toISOString()
                            });
                        } else {
                            console.log(`[TCP] DevKit auth failed: ${clientInfo}`);
                            sendToDevKitRaw('auth:failed');
                            socket.destroy();
                        }
                    } else {
                        console.log(`[TCP] DevKit not authenticated, ignoring: ${message}`);
                        sendToDevKitRaw('auth:required');
                    }
                    continue;
                }

                console.log(`[TCP] Received from DevKit: ${message}`);

                // Forward to web clients
                broadcastToWeb({
                    type: 'devkit_message',
                    data: message,
                    timestamp: new Date().toISOString()
                });
            } else {
                break; // Wait for more data
            }
        }
    });

    socket.on('close', () => {
        console.log(`[TCP] DevKit disconnected: ${clientInfo}`);
        devkitSocket = null;
        devkitAuthenticated = false;

        broadcastToWeb({
            type: 'devkit_status',
            connected: false,
            timestamp: new Date().toISOString()
        });
    });

    socket.on('error', (err) => {
        console.error(`[TCP] DevKit socket error: ${err.message}`);
        devkitSocket = null;
        devkitAuthenticated = false;
    });
});

tcpServer.listen(TCP_PORT, '0.0.0.0', () => {
    console.log(`[TCP] Server listening on port ${TCP_PORT}`);
    console.log(`[TCP] DevKit should connect to: ${LOCAL_IP}:${TCP_PORT}`);
});

// ==================== WebSocket Server (on same port as HTTP) ====================
const wss = new WebSocketServer({ server });

wss.on('connection', (ws, req) => {
    const clientIP = req.socket.remoteAddress;
    console.log(`[WS] Web client connecting: ${clientIP}`);

    // Store client (not authenticated yet)
    webClients.set(ws, { authenticated: false });

    // Request authentication
    ws.send(JSON.stringify({ type: 'auth_required' }));

    ws.on('message', (data) => {
        try {
            const message = JSON.parse(data.toString());
            const clientState = webClients.get(ws);

            // Handle authentication
            if (message.type === 'auth') {
                if (message.token === AUTH_TOKEN) {
                    clientState.authenticated = true;
                    console.log(`[WS] Web client authenticated: ${clientIP}`);

                    ws.send(JSON.stringify({
                        type: 'auth_success',
                        devkit: {
                            connected: devkitSocket !== null && devkitAuthenticated
                        },
                        serverIP: LOCAL_IP,
                        tcpPort: TCP_PORT
                    }));
                } else {
                    console.log(`[WS] Web client auth failed: ${clientIP}`);
                    ws.send(JSON.stringify({ type: 'auth_failed' }));
                    ws.close();
                }
                return;
            }

            // Require authentication for all other messages
            if (!clientState.authenticated) {
                ws.send(JSON.stringify({ type: 'auth_required' }));
                return;
            }

            console.log(`[WS] Received from web:`, message);

            if (message.type === 'send_to_devkit') {
                sendToDevKit(message.data);
            }
        } catch (err) {
            console.error('[WS] Parse error:', err.message);
        }
    });

    ws.on('close', () => {
        console.log(`[WS] Web client disconnected: ${clientIP}`);
        webClients.delete(ws);
    });

    ws.on('error', (err) => {
        console.error(`[WS] Client error: ${err.message}`);
        webClients.delete(ws);
    });
});

console.log(`[WS] Server attached to HTTP server on port ${HTTP_PORT}`);

// ==================== Helper Functions ====================

function broadcastToWeb(data) {
    const message = JSON.stringify(data);
    webClients.forEach((state, client) => {
        if (client.readyState === 1 && state.authenticated) { // OPEN and authenticated
            client.send(message);
        }
    });
}

function sendToDevKitRaw(data) {
    if (!devkitSocket) return false;

    try {
        const payload = Buffer.from(data, 'utf8');
        const header = Buffer.alloc(4);
        header.writeUInt32LE(payload.length, 0);
        devkitSocket.write(Buffer.concat([header, payload]));
        return true;
    } catch (err) {
        console.error('[TCP] Send error:', err.message);
        return false;
    }
}

function sendToDevKit(data) {
    if (!devkitSocket || !devkitAuthenticated) {
        console.log('[TCP] Cannot send - DevKit not connected/authenticated');
        broadcastToWeb({
            type: 'error',
            message: 'DevKit not connected or not authenticated',
            timestamp: new Date().toISOString()
        });
        return false;
    }

    if (sendToDevKitRaw(data)) {
        console.log(`[TCP] Sent to DevKit: ${data}`);
        broadcastToWeb({
            type: 'sent_to_devkit',
            data: data,
            timestamp: new Date().toISOString()
        });
        return true;
    }
    return false;
}

// ==================== HTTP API ====================

app.get('/health', (req, res) => {
    res.json({ status: 'ok', timestamp: new Date().toISOString() });
});

app.get('/api/status', (req, res) => {
    res.json({
        devkit: {
            connected: devkitSocket !== null,
            authenticated: devkitAuthenticated,
            address: devkitSocket ? devkitSocket.remoteAddress : null
        },
        server: {
            ip: LOCAL_IP,
            httpPort: HTTP_PORT,
            tcpPort: TCP_PORT
        },
        webClients: webClients.size
    });
});

app.post('/api/send', (req, res) => {
    const { message } = req.body;

    if (!message) {
        return res.status(400).json({ success: false, error: 'No message provided' });
    }

    const sent = sendToDevKit(message);
    res.json({ success: sent, message: sent ? 'Message sent' : 'DevKit not connected' });
});

// Get auth token (for WebSocket auth from authenticated HTTP session)
app.get('/api/token', (req, res) => {
    res.json({ token: AUTH_TOKEN });
});

// Start HTTP + WebSocket server
server.listen(HTTP_PORT, '0.0.0.0', () => {
    console.log('');
    console.log('='.repeat(60));
    console.log('  DevKit Communication Server Started (WITH AUTH)');
    console.log('='.repeat(60));
    console.log(`  Web UI + WS: http://${LOCAL_IP}:${HTTP_PORT}`);
    console.log(`  TCP Server:  ${LOCAL_IP}:${TCP_PORT} (for DevKit)`);
    console.log('');
    console.log(`  Auth Username: ${AUTH_USERNAME}`);
    console.log(`  Auth Password: ${'*'.repeat(AUTH_PASSWORD.length)}`);
    console.log(`  Auth Token:    ${AUTH_TOKEN.substring(0, 8)}...`);
    console.log('='.repeat(60));
    console.log('');
});
