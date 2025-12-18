/**
 * VPS RTSP Relay Server
 * 
 * This server receives the RTSP tunnel connection from the T5AI DevKit
 * and makes the RTSP stream available locally on the VPS.
 * 
 * Architecture:
 * [RTSP Camera] <--> [T5AI DevKit] <--> [This Server] <--> [FFmpeg/VLC/etc]
 * 
 * The DevKit connects to this server (port 8554 by default) and forwards
 * all RTSP traffic bidirectionally.
 * 
 * Usage:
 *   node rtsp_relay.js
 * 
 * Then use FFmpeg with INTERLEAVED TCP mode to access the stream:
 * 
 *   # The camera's path and credentials are passed to FFmpeg
 *   # Example for camera: rtsp://admin:Admin1234@192.168.18.34:554/live/ch00_0
 *   
 *   ffplay -rtsp_transport tcp -i "rtsp://admin:Admin1234@localhost:8555/live/ch00_0"
 *   
 *   # Or save to file:
 *   ffmpeg -rtsp_transport tcp -i "rtsp://admin:Admin1234@localhost:8555/live/ch00_0" -c copy output.mp4
 *   
 *   # Re-stream as HLS:
 *   ffmpeg -rtsp_transport tcp -i "rtsp://admin:Admin1234@localhost:8555/live/ch00_0" \
 *          -c:v libx264 -f hls /var/www/html/stream.m3u8
 */

const net = require('net');
const http = require('http');
const { spawn } = require('child_process');

// Configuration
const DEVKIT_PORT = process.env.RTSP_DEVKIT_PORT || 8554;  // Port for DevKit connection
const LOCAL_RTSP_PORT = process.env.RTSP_LOCAL_PORT || 8555;  // RTSP access port
const STATUS_PORT = process.env.RTSP_STATUS_PORT || 8556;  // Status/control API
const PUBLIC_ACCESS = process.env.RTSP_PUBLIC_ACCESS === '1';  // Allow public RTSP access

// Bind address: 0.0.0.0 for public, 127.0.0.1 for local only
const RTSP_BIND_ADDR = PUBLIC_ACCESS ? '0.0.0.0' : '127.0.0.1';

// State
let devkitSocket = null;
let localClients = new Map();  // port -> socket
let totalBytesToLocal = 0;
let totalBytesToCamera = 0;
let lastDataTime = 0;

// ==================== DevKit Connection (from T5AI) ====================
const devkitServer = net.createServer((socket) => {
    const clientInfo = `${socket.remoteAddress}:${socket.remotePort}`;
    console.log(`[DevKit] Connected: ${clientInfo}`);

    // Enable TCP keepalive to prevent connection drops
    socket.setKeepAlive(true, 30000);  // 30 second keepalive
    socket.setNoDelay(true);  // Disable Nagle's algorithm for real-time

    if (devkitSocket) {
        console.log('[DevKit] Replacing existing connection');
        devkitSocket.destroy();
    }

    devkitSocket = socket;
    lastDataTime = Date.now();

    socket.on('data', (data) => {
        totalBytesToLocal += data.length;
        lastDataTime = Date.now();

        // Log data flow periodically
        if (totalBytesToLocal % 10000 < data.length) {
            console.log(`[DevKit] Received ${data.length} bytes (total: ${totalBytesToLocal})`);
        }

        // Forward to all local RTSP clients
        localClients.forEach((client, port) => {
            try {
                client.write(data);
            } catch (err) {
                console.error(`[Local:${port}] Write error: ${err.message}`);
            }
        });
    });

    socket.on('close', () => {
        console.log(`[DevKit] Disconnected: ${clientInfo}`);
        devkitSocket = null;

        // Close all local clients
        localClients.forEach((client, port) => {
            console.log(`[Local:${port}] Closing (DevKit disconnected)`);
            client.destroy();
        });
        localClients.clear();
    });

    socket.on('error', (err) => {
        console.error(`[DevKit] Error: ${err.message}`);
        devkitSocket = null;
    });
});

devkitServer.listen(DEVKIT_PORT, '0.0.0.0', () => {
    console.log(`[DevKit] Server listening on port ${DEVKIT_PORT}`);
    console.log(`[DevKit] DevKit should connect to: YOUR_VPS_IP:${DEVKIT_PORT}`);
});

// ==================== Local RTSP Access ====================
const localServer = net.createServer((socket) => {
    const clientPort = socket.remotePort;
    console.log(`[Local:${clientPort}] Connected from ${socket.remoteAddress}`);

    if (!devkitSocket) {
        console.log(`[Local:${clientPort}] No DevKit connected, closing`);
        socket.end();
        return;
    }

    // Enable TCP keepalive
    socket.setKeepAlive(true, 30000);
    socket.setNoDelay(true);

    localClients.set(clientPort, socket);
    console.log(`[Local:${clientPort}] Active clients: ${localClients.size}`);

    socket.on('data', (data) => {
        if (devkitSocket) {
            totalBytesToCamera += data.length;
            console.log(`[Local:${clientPort}] Sending ${data.length} bytes to camera (total: ${totalBytesToCamera})`);
            try {
                devkitSocket.write(data);
            } catch (err) {
                console.error(`[Local:${clientPort}] Error sending to DevKit: ${err.message}`);
            }
        } else {
            console.warn(`[Local:${clientPort}] DevKit disconnected, cannot send data`);
        }
    });

    socket.on('close', () => {
        console.log(`[Local:${clientPort}] Disconnected`);
        localClients.delete(clientPort);
    });

    socket.on('error', (err) => {
        console.error(`[Local:${clientPort}] Error: ${err.message}`);
        localClients.delete(clientPort);
    });
});

localServer.listen(LOCAL_RTSP_PORT, RTSP_BIND_ADDR, () => {
    if (PUBLIC_ACCESS) {
        console.log(`[RTSP] PUBLIC access enabled on 0.0.0.0:${LOCAL_RTSP_PORT}`);
        console.log(`[RTSP] Other devices can connect to: rtsp://YOUR_VPS_IP:${LOCAL_RTSP_PORT}/...`);
    } else {
        console.log(`[RTSP] Local-only access on 127.0.0.1:${LOCAL_RTSP_PORT}`);
        console.log(`[RTSP] Set RTSP_PUBLIC_ACCESS=1 to enable remote access`);
    }
});

// ==================== Status/Control API ====================
const statusServer = http.createServer((req, res) => {
    res.setHeader('Content-Type', 'application/json');
    res.setHeader('Access-Control-Allow-Origin', '*');

    if (req.url === '/status') {
        res.end(JSON.stringify({
            devkit: {
                connected: devkitSocket !== null,
                address: devkitSocket ? devkitSocket.remoteAddress : null
            },
            localClients: localClients.size,
            stats: {
                bytesToLocal: totalBytesToLocal,
                bytesToCamera: totalBytesToCamera
            },
            ports: {
                devkit: DEVKIT_PORT,
                localRtsp: LOCAL_RTSP_PORT,
                status: STATUS_PORT
            }
        }, null, 2));
    } else if (req.url === '/help') {
        res.end(JSON.stringify({
            usage: {
                'RTSP_DEVKIT_PORT': 'Port for DevKit to connect (default: 8554)',
                'RTSP_LOCAL_PORT': 'Local RTSP access port (default: 8555)',
                'RTSP_STATUS_PORT': 'Status API port (default: 8556)'
            },
            endpoints: {
                '/status': 'Get server status',
                '/help': 'This help message'
            },
            ffmpeg_examples: [
                `# Direct TCP (after RTSP handshake completes)`,
                `ffplay tcp://localhost:${LOCAL_RTSP_PORT}`,
                ``,
                `# Save to file`,
                `ffmpeg -i tcp://localhost:${LOCAL_RTSP_PORT} -c copy output.mp4`,
                ``,
                `# Re-stream as HLS`,
                `ffmpeg -i tcp://localhost:${LOCAL_RTSP_PORT} -c:v libx264 -f hls /var/www/html/stream.m3u8`
            ]
        }, null, 2));
    } else {
        res.statusCode = 404;
        res.end(JSON.stringify({ error: 'Not found. Try /status or /help' }));
    }
});

statusServer.listen(STATUS_PORT, '127.0.0.1', () => {
    console.log(`[Status] API on http://localhost:${STATUS_PORT}/status`);
});

// ==================== Startup Banner ====================
console.log('='.repeat(60));
console.log('  RTSP Relay Server for T5AI DevKit Tunnel');
console.log('='.repeat(60));
console.log('');
console.log('Configuration:');
console.log(`  DevKit port:       0.0.0.0:${DEVKIT_PORT}`);
console.log(`  RTSP access:       ${RTSP_BIND_ADDR}:${LOCAL_RTSP_PORT}`);
console.log(`  Status API:        http://127.0.0.1:${STATUS_PORT}/status`);
console.log(`  Public access:     ${PUBLIC_ACCESS ? 'ENABLED' : 'DISABLED (set RTSP_PUBLIC_ACCESS=1)'}`);
console.log('');
console.log('Usage:');
console.log('  1. Start the DevKit RTSP tunnel (send "rtsp start" via web app)');
if (PUBLIC_ACCESS) {
    console.log(`  2. From ANY device: ffplay -rtsp_transport tcp "rtsp://admin:pass@YOUR_VPS_IP:${LOCAL_RTSP_PORT}/live/ch00_0"`);
} else {
    console.log(`  2. From VPS only: ffplay -rtsp_transport tcp "rtsp://admin:pass@localhost:${LOCAL_RTSP_PORT}/live/ch00_0"`);
}
console.log('='.repeat(60));
