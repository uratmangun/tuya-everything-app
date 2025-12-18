/**
 * VPS RTP Receiver
 * 
 * Receives RTP packets from DevKit over TCP and either:
 * - Saves to a file
 * - Pipes to FFmpeg for decoding/display
 * - Re-streams via UDP locally
 * 
 * Protocol: Each RTP packet is prefixed with 2-byte big-endian length
 * 
 * Usage:
 *   node rtp_receiver.js              # Save to file
 *   node rtp_receiver.js | ffplay -   # Pipe to FFplay (experimental)
 *   node rtp_receiver.js --udp        # Re-emit as UDP (for ffplay udp://localhost:5006)
 * 
 * Then play with:
 *   ffplay -protocol_whitelist file,rtp,udp -i rtp://127.0.0.1:5006
 */

const net = require('net');
const dgram = require('dgram');
const fs = require('fs');

const RTP_LISTEN_PORT = process.env.RTP_LISTEN_PORT || 8557;
const UDP_RESTREAM_PORT = process.env.UDP_RESTREAM_PORT || 5006;
const useUdp = process.argv.includes('--udp');

let totalPackets = 0;
let totalBytes = 0;
let devkitSocket = null;
let buffer = Buffer.alloc(0);

// UDP socket for re-streaming
let udpSocket = null;
if (useUdp) {
    udpSocket = dgram.createSocket('udp4');
    console.log(`[RTP] Will re-stream RTP via UDP to localhost:${UDP_RESTREAM_PORT}`);
}

// File output for raw RTP
let fileStream = null;
if (!useUdp && !process.stdout.isTTY) {
    // Piping mode - write to stdout
    console.error('[RTP] Piping RTP data to stdout');
} else if (!useUdp) {
    const filename = `rtp_capture_${Date.now()}.rtp`;
    fileStream = fs.createWriteStream(filename);
    console.log(`[RTP] Saving RTP data to ${filename}`);
}

// Server for DevKit connection
const server = net.createServer((socket) => {
    console.log(`[DevKit] Connected: ${socket.remoteAddress}:${socket.remotePort}`);

    if (devkitSocket) {
        console.log('[DevKit] Replacing existing connection');
        devkitSocket.destroy();
    }

    devkitSocket = socket;
    socket.setKeepAlive(true, 30000);
    buffer = Buffer.alloc(0);

    socket.on('data', (data) => {
        buffer = Buffer.concat([buffer, data]);

        // Process complete packets (2-byte length prefix)
        while (buffer.length >= 2) {
            const packetLen = (buffer[0] << 8) | buffer[1];

            if (buffer.length < packetLen + 2) {
                break;  // Wait for more data
            }

            // Extract RTP packet
            const rtpPacket = buffer.slice(2, 2 + packetLen);
            buffer = buffer.slice(2 + packetLen);

            totalPackets++;
            totalBytes += packetLen;

            // Log periodically
            if (totalPackets % 100 === 0) {
                console.error(`[RTP] Received ${totalPackets} packets, ${(totalBytes / 1024).toFixed(1)} KB`);
            }

            // Handle RTP packet
            if (useUdp && udpSocket) {
                // Re-emit via UDP
                udpSocket.send(rtpPacket, UDP_RESTREAM_PORT, '127.0.0.1');
            } else if (fileStream) {
                // Write to file with RTP packet format
                fileStream.write(rtpPacket);
            } else {
                // Write to stdout for piping
                process.stdout.write(rtpPacket);
            }
        }
    });

    socket.on('close', () => {
        console.log('[DevKit] Disconnected');
        devkitSocket = null;
    });

    socket.on('error', (err) => {
        console.error(`[DevKit] Error: ${err.message}`);
    });
});

server.listen(RTP_LISTEN_PORT, '0.0.0.0', () => {
    console.log('============================================');
    console.log('  RTP Receiver for DevKit UDP Camera Tunnel');
    console.log('============================================');
    console.log(`[RTP] Listening on port ${RTP_LISTEN_PORT}`);
    console.log('');
    if (useUdp) {
        console.log('Usage: Play with FFplay');
        console.log(`  ffplay -protocol_whitelist udp,rtp -i "rtp://127.0.0.1:${UDP_RESTREAM_PORT}"`);
    } else {
        console.log('Usage:');
        console.log('  Start tunnel on DevKit, RTP data will be saved to file');
    }
    console.log('============================================');
});

// Clean exit
process.on('SIGINT', () => {
    console.log(`\n[RTP] Shutting down. Received ${totalPackets} packets, ${totalBytes} bytes`);
    if (fileStream) fileStream.end();
    if (udpSocket) udpSocket.close();
    server.close();
    process.exit(0);
});
