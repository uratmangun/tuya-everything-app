/**
 * DevKit Communication Server with Authentication
 * 
 * This server provides:
 * 1. TCP Server (port 5000) - DevKit connects here (with token auth)
 * 2. HTTP + WebSocket Server (port 3000) - Serves web UI and WebSocket on same port
 * 
 * Security:
 * - Session-based auth with login page for web UI
 * - Token-based auth for WebSocket and TCP
 */

const express = require('express');
const { WebSocketServer } = require('ws');
const net = require('net');
const path = require('path');
const http = require('http');
const crypto = require('crypto');

// Load environment variables
const AUTH_USERNAME = process.env.AUTH_USERNAME || 'admin';
const AUTH_PASSWORD = process.env.AUTH_PASSWORD || 'changeme123';
const AUTH_TOKEN = process.env.AUTH_TOKEN || 'devkit-secret-token';
const SESSION_SECRET = process.env.SESSION_SECRET || crypto.randomBytes(32).toString('hex');

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
let keepAliveInterval = null; // Keep-alive ping interval
const KEEPALIVE_INTERVAL_MS = 60000; // 60 seconds

// Simple session store (in-memory)
const sessions = new Map();

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

// ==================== Cookie-Based Session Auth ====================
const cookieParser = require('cookie-parser');
app.use(cookieParser(SESSION_SECRET));
app.use(express.json());

// Generate session ID
function generateSessionId() {
    return crypto.randomBytes(32).toString('hex');
}

// Validate session
function validateSession(sessionId) {
    if (!sessionId) return false;
    const session = sessions.get(sessionId);
    if (!session) return false;
    // Check expiry (24 hours)
    if (Date.now() - session.createdAt > 24 * 60 * 60 * 1000) {
        sessions.delete(sessionId);
        return false;
    }
    return true;
}

// Auth middleware - protect all routes except login page and API
function authMiddleware(req, res, next) {
    // Public paths that don't require auth
    const publicPaths = ['/login.html', '/api/login', '/api/check-auth', '/health'];

    if (publicPaths.some(p => req.path === p || req.path.startsWith(p))) {
        return next();
    }

    // Check session cookie
    const sessionId = req.signedCookies.session;

    if (validateSession(sessionId)) {
        return next();
    }

    // Not authenticated - redirect to login or return 401 for API
    if (req.path.startsWith('/api/')) {
        return res.status(401).json({ error: 'Not authenticated' });
    }

    // Redirect to login with return URL
    const returnUrl = encodeURIComponent(req.originalUrl);
    return res.redirect(`/login.html?redirect=${returnUrl}`);
}

app.use(authMiddleware);
app.use(express.static(path.join(__dirname, 'public')));

// ==================== Auth API Endpoints ====================
app.post('/api/login', (req, res) => {
    const { username, password } = req.body;

    if (username === AUTH_USERNAME && password === AUTH_PASSWORD) {
        const sessionId = generateSessionId();
        sessions.set(sessionId, {
            username,
            createdAt: Date.now()
        });

        res.cookie('session', sessionId, {
            signed: true,
            httpOnly: true,
            secure: false, // Set to true if using HTTPS
            sameSite: 'lax',
            maxAge: 24 * 60 * 60 * 1000 // 24 hours
        });

        console.log(`[AUTH] User logged in: ${username}`);
        return res.json({ success: true });
    }

    console.log(`[AUTH] Failed login attempt for: ${username}`);
    return res.status(401).json({ success: false, error: 'Invalid username or password' });
});

app.get('/api/check-auth', (req, res) => {
    const sessionId = req.signedCookies.session;
    if (validateSession(sessionId)) {
        const session = sessions.get(sessionId);
        return res.json({ authenticated: true, username: session.username });
    }
    return res.status(401).json({ authenticated: false });
});

app.post('/api/logout', (req, res) => {
    const sessionId = req.signedCookies.session;
    if (sessionId) {
        sessions.delete(sessionId);
    }
    res.clearCookie('session');
    console.log(`[AUTH] User logged out`);
    return res.json({ success: true });
});

// Redirect /ble to /ble/index.html
app.get('/ble', (req, res) => {
    res.redirect('/ble/index.html');
});

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

            if (buffer.length < 4 + msgLen) {
                break; // Wait for more data
            }

            // Check for audio data (binary with 'audio:' prefix)
            // Format: "audio:" (6 bytes) + binary PCM data
            const rawMessage = buffer.slice(4, 4 + msgLen);
            buffer = buffer.slice(4 + msgLen);

            // Check for auth message (only ASCII auth: prefix matters)
            if (!devkitAuthenticated) {
                const message = rawMessage.toString('utf8');
                if (message.startsWith('auth:')) {
                    const token = message.substring(5);
                    if (token === AUTH_TOKEN) {
                        devkitAuthenticated = true;
                        console.log(`[TCP] DevKit authenticated: ${clientInfo}`);
                        sendToDevKitRaw('auth:ok');

                        // Start keep-alive ping
                        startKeepAlive();

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

            // Check if this is audio data (binary with 'audio:' prefix)
            const audioPrefix = Buffer.from('audio:');
            if (rawMessage.length > 6 && rawMessage.slice(0, 6).equals(audioPrefix)) {
                // Extract audio payload (after the 'audio:' prefix)
                const audioData = rawMessage.slice(6);

                // Forward audio data to web clients as binary
                broadcastAudioToWeb(audioData);
                continue;
            }

            // Regular text message
            const message = rawMessage.toString('utf8');
            console.log(`[TCP] Received from DevKit: ${message}`);

            // Handle pong responses silently (don't broadcast to web clients)
            if (message === 'pong') {
                console.log(`[TCP] Keep-alive pong received`);
                continue;
            }

            // Forward to web clients as JSON
            broadcastToWeb({
                type: 'devkit_message',
                data: message,
                timestamp: new Date().toISOString()
            });
        }
    });

    socket.on('close', () => {
        console.log(`[TCP] DevKit disconnected: ${clientInfo}`);
        stopKeepAlive();
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
        stopKeepAlive();
        devkitSocket = null;
        devkitAuthenticated = false;
    });
});

// ==================== Keep-Alive Functions ====================

function startKeepAlive() {
    // Stop any existing interval first
    stopKeepAlive();

    console.log(`[TCP] Starting keep-alive ping (every ${KEEPALIVE_INTERVAL_MS / 1000}s)`);

    keepAliveInterval = setInterval(() => {
        if (devkitSocket && devkitAuthenticated) {
            console.log(`[TCP] Sending keep-alive ping`);
            sendToDevKitRaw('ping');
        } else {
            // DevKit disconnected, stop the interval
            stopKeepAlive();
        }
    }, KEEPALIVE_INTERVAL_MS);
}

function stopKeepAlive() {
    if (keepAliveInterval) {
        console.log(`[TCP] Stopping keep-alive ping`);
        clearInterval(keepAliveInterval);
        keepAliveInterval = null;
    }
}

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

/**
 * Broadcast binary audio data to all authenticated web clients
 * Audio format: PCM 16-bit, 8000Hz, mono
 */
function broadcastAudioToWeb(audioData) {
    // Create a message header to identify this as audio data
    const header = Buffer.from(JSON.stringify({
        type: 'audio_data',
        sampleRate: 8000,
        channels: 1,
        bitsPerSample: 16,
        length: audioData.length
    }) + '\n');

    webClients.forEach((state, client) => {
        if (client.readyState === 1 && state.authenticated) {
            // Send as binary data with JSON header
            // First send the header, then the binary audio data
            client.send(header);
            client.send(audioData);
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
