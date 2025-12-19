/**
 * Tuya BLE Encryption Protocol Implementation
 * 
 * This module implements the Tuya BLE communication protocol including:
 * - MD5-based key derivation
 * - AES-128-CBC encryption/decryption
 * - Packet framing and CRC16
 * 
 * Based on reverse-engineering from TuyaOpen source code.
 */

// ===== MD5 Implementation =====
// Using a compact MD5 implementation
const MD5 = (function () {
    function md5cycle(x, k) {
        var a = x[0], b = x[1], c = x[2], d = x[3];
        a = ff(a, b, c, d, k[0], 7, -680876936);
        d = ff(d, a, b, c, k[1], 12, -389564586);
        c = ff(c, d, a, b, k[2], 17, 606105819);
        b = ff(b, c, d, a, k[3], 22, -1044525330);
        a = ff(a, b, c, d, k[4], 7, -176418897);
        d = ff(d, a, b, c, k[5], 12, 1200080426);
        c = ff(c, d, a, b, k[6], 17, -1473231341);
        b = ff(b, c, d, a, k[7], 22, -45705983);
        a = ff(a, b, c, d, k[8], 7, 1770035416);
        d = ff(d, a, b, c, k[9], 12, -1958414417);
        c = ff(c, d, a, b, k[10], 17, -42063);
        b = ff(b, c, d, a, k[11], 22, -1990404162);
        a = ff(a, b, c, d, k[12], 7, 1804603682);
        d = ff(d, a, b, c, k[13], 12, -40341101);
        c = ff(c, d, a, b, k[14], 17, -1502002290);
        b = ff(b, c, d, a, k[15], 22, 1236535329);
        a = gg(a, b, c, d, k[1], 5, -165796510);
        d = gg(d, a, b, c, k[6], 9, -1069501632);
        c = gg(c, d, a, b, k[11], 14, 643717713);
        b = gg(b, c, d, a, k[0], 20, -373897302);
        a = gg(a, b, c, d, k[5], 5, -701558691);
        d = gg(d, a, b, c, k[10], 9, 38016083);
        c = gg(c, d, a, b, k[15], 14, -660478335);
        b = gg(b, c, d, a, k[4], 20, -405537848);
        a = gg(a, b, c, d, k[9], 5, 568446438);
        d = gg(d, a, b, c, k[14], 9, -1019803690);
        c = gg(c, d, a, b, k[3], 14, -187363961);
        b = gg(b, c, d, a, k[8], 20, 1163531501);
        a = gg(a, b, c, d, k[13], 5, -1444681467);
        d = gg(d, a, b, c, k[2], 9, -51403784);
        c = gg(c, d, a, b, k[7], 14, 1735328473);
        b = gg(b, c, d, a, k[12], 20, -1926607734);
        a = hh(a, b, c, d, k[5], 4, -378558);
        d = hh(d, a, b, c, k[8], 11, -2022574463);
        c = hh(c, d, a, b, k[11], 16, 1839030562);
        b = hh(b, c, d, a, k[14], 23, -35309556);
        a = hh(a, b, c, d, k[1], 4, -1530992060);
        d = hh(d, a, b, c, k[4], 11, 1272893353);
        c = hh(c, d, a, b, k[7], 16, -155497632);
        b = hh(b, c, d, a, k[10], 23, -1094730640);
        a = hh(a, b, c, d, k[13], 4, 681279174);
        d = hh(d, a, b, c, k[0], 11, -358537222);
        c = hh(c, d, a, b, k[3], 16, -722521979);
        b = hh(b, c, d, a, k[6], 23, 76029189);
        a = hh(a, b, c, d, k[9], 4, -640364487);
        d = hh(d, a, b, c, k[12], 11, -421815835);
        c = hh(c, d, a, b, k[15], 16, 530742520);
        b = hh(b, c, d, a, k[2], 23, -995338651);
        a = ii(a, b, c, d, k[0], 6, -198630844);
        d = ii(d, a, b, c, k[7], 10, 1126891415);
        c = ii(c, d, a, b, k[14], 15, -1416354905);
        b = ii(b, c, d, a, k[5], 21, -57434055);
        a = ii(a, b, c, d, k[12], 6, 1700485571);
        d = ii(d, a, b, c, k[3], 10, -1894986606);
        c = ii(c, d, a, b, k[10], 15, -1051523);
        b = ii(b, c, d, a, k[1], 21, -2054922799);
        a = ii(a, b, c, d, k[8], 6, 1873313359);
        d = ii(d, a, b, c, k[15], 10, -30611744);
        c = ii(c, d, a, b, k[6], 15, -1560198380);
        b = ii(b, c, d, a, k[13], 21, 1309151649);
        a = ii(a, b, c, d, k[4], 6, -145523070);
        d = ii(d, a, b, c, k[11], 10, -1120210379);
        c = ii(c, d, a, b, k[2], 15, 718787259);
        b = ii(b, c, d, a, k[9], 21, -343485551);
        x[0] = add32(a, x[0]);
        x[1] = add32(b, x[1]);
        x[2] = add32(c, x[2]);
        x[3] = add32(d, x[3]);
    }

    function cmn(q, a, b, x, s, t) {
        a = add32(add32(a, q), add32(x, t));
        return add32((a << s) | (a >>> (32 - s)), b);
    }

    function ff(a, b, c, d, x, s, t) {
        return cmn((b & c) | ((~b) & d), a, b, x, s, t);
    }

    function gg(a, b, c, d, x, s, t) {
        return cmn((b & d) | (c & (~d)), a, b, x, s, t);
    }

    function hh(a, b, c, d, x, s, t) {
        return cmn(b ^ c ^ d, a, b, x, s, t);
    }

    function ii(a, b, c, d, x, s, t) {
        return cmn(c ^ (b | (~d)), a, b, x, s, t);
    }

    function md51(s) {
        var n = s.length,
            state = [1732584193, -271733879, -1732584194, 271733878], i;
        for (i = 64; i <= s.length; i += 64) {
            md5cycle(state, md5blk(s.substring(i - 64, i)));
        }
        s = s.substring(i - 64);
        var tail = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        for (i = 0; i < s.length; i++)
            tail[i >> 2] |= s.charCodeAt(i) << ((i % 4) << 3);
        tail[i >> 2] |= 0x80 << ((i % 4) << 3);
        if (i > 55) {
            md5cycle(state, tail);
            for (i = 0; i < 16; i++) tail[i] = 0;
        }
        tail[14] = n * 8;
        md5cycle(state, tail);
        return state;
    }

    function md5blk(s) {
        var md5blks = [], i;
        for (i = 0; i < 64; i += 4) {
            md5blks[i >> 2] = s.charCodeAt(i) + (s.charCodeAt(i + 1) << 8) +
                (s.charCodeAt(i + 2) << 16) + (s.charCodeAt(i + 3) << 24);
        }
        return md5blks;
    }

    var hex_chr = '0123456789abcdef'.split('');

    function rhex(n) {
        var s = '', j = 0;
        for (; j < 4; j++)
            s += hex_chr[(n >> (j * 8 + 4)) & 0x0F] + hex_chr[(n >> (j * 8)) & 0x0F];
        return s;
    }

    function hex(x) {
        for (var i = 0; i < x.length; i++)
            x[i] = rhex(x[i]);
        return x.join('');
    }

    function add32(a, b) {
        return (a + b) & 0xFFFFFFFF;
    }

    // MD5 for Uint8Array
    function md5Bytes(bytes) {
        var str = '';
        for (var i = 0; i < bytes.length; i++) {
            str += String.fromCharCode(bytes[i]);
        }
        var state = md51(str);
        var result = new Uint8Array(16);
        for (var i = 0; i < 4; i++) {
            result[i * 4] = state[i] & 0xFF;
            result[i * 4 + 1] = (state[i] >> 8) & 0xFF;
            result[i * 4 + 2] = (state[i] >> 16) & 0xFF;
            result[i * 4 + 3] = (state[i] >> 24) & 0xFF;
        }
        return result;
    }

    return { hash: md5Bytes };
})();

// ===== CRC16 Implementation =====
// Tuya uses CRC-16 with polynomial 0xA001 and initial value 0xFFFF
function crc16(data) {
    const poly = [0, 0xA001];  // 0x8005 reversed = 0xA001
    let crc = 0xFFFF;

    for (let j = 0; j < data.length; j++) {
        let ds = data[j];
        for (let i = 0; i < 8; i++) {
            crc = (crc >>> 1) ^ poly[(crc ^ ds) & 1];
            ds = ds >>> 1;
        }
    }

    return crc;
}

// ===== AES-128-CBC Implementation using Web Crypto API =====
class TuyaAES {
    static async encrypt(data, key, iv) {
        const cryptoKey = await crypto.subtle.importKey(
            'raw', key, { name: 'AES-CBC' }, false, ['encrypt']
        );
        const encrypted = await crypto.subtle.encrypt(
            { name: 'AES-CBC', iv: iv }, cryptoKey, data
        );
        return new Uint8Array(encrypted);
    }

    static async decrypt(data, key, iv) {
        const cryptoKey = await crypto.subtle.importKey(
            'raw', key, { name: 'AES-CBC' }, false, ['decrypt']
        );
        const decrypted = await crypto.subtle.decrypt(
            { name: 'AES-CBC', iv: iv }, cryptoKey, data
        );
        return new Uint8Array(decrypted);
    }
}

// ===== Tuya BLE Crypto Class =====
class TuyaBLECrypto {
    constructor(uuid, authKey) {
        // Store keys as Uint8Array
        this.uuid = this.stringToBytes(uuid.replace('uuid', ''));  // Remove 'uuid' prefix
        this.authKey = this.stringToBytes(authKey);
        this.serviceRand = new Uint8Array(16);
        this.pairRand = new Uint8Array(6);
        this.key11 = null;  // Cached key for mode 11
        this.sendSN = 0;
        this.recvSN = 0;
    }

    stringToBytes(str) {
        const bytes = new Uint8Array(str.length);
        for (let i = 0; i < str.length; i++) {
            bytes[i] = str.charCodeAt(i);
        }
        return bytes;
    }

    bytesToHex(bytes) {
        return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('');
    }

    hexToBytes(hex) {
        const bytes = new Uint8Array(hex.length / 2);
        for (let i = 0; i < hex.length; i += 2) {
            bytes[i / 2] = parseInt(hex.substr(i, 2), 16);
        }
        return bytes;
    }

    // Compress UUID (20 chars -> 16 bytes)
    compressUUID(uuid) {
        if (uuid.length < 20) {
            // Already 16 or less, just return as bytes
            const result = new Uint8Array(16);
            for (let i = 0; i < Math.min(uuid.length, 16); i++) {
                result[i] = uuid.charCodeAt(i);
            }
            return result;
        }

        const out = new Uint8Array(16);
        const temp = new Uint8Array(4);

        for (let i = 0; i < 5; i++) {
            for (let j = i * 4; j < (i * 4 + 4); j++) {
                const ch = uuid.charCodeAt(j);
                let val;
                if (ch >= 0x30 && ch <= 0x39) {
                    val = ch - 0x30;
                } else if (ch >= 0x41 && ch <= 0x5A) {
                    val = ch - 0x41 + 36;
                } else if (ch >= 0x61 && ch <= 0x7A) {
                    val = ch - 0x61 + 10;
                } else {
                    val = 0;
                }
                temp[j - i * 4] = val;
            }
            out[i * 3] = ((temp[0] & 0x3F) << 2) | ((temp[1] >> 4) & 0x03);
            out[i * 3 + 1] = ((temp[1] & 0x0F) << 4) | ((temp[2] >> 2) & 0x0F);
            out[i * 3 + 2] = ((temp[2] & 0x03) << 6) | (temp[3] & 0x3F);
        }
        out[15] = 0xFF;
        return out;
    }

    // Generate encryption key based on mode
    generateKey(mode) {
        const ENCRYPTION_MODE_NONE = 0;
        const ENCRYPTION_MODE_KEY_11 = 11;
        const ENCRYPTION_MODE_KEY_12 = 12;

        if (mode === ENCRYPTION_MODE_NONE) {
            return null;
        }

        let keyInput;

        switch (mode) {
            case ENCRYPTION_MODE_KEY_11:
                // key = MD5(authKey + uuid + serviceRand)
                keyInput = new Uint8Array(32 + 16 + 16);  // authKey(32) + uuid(16) + rand(16)
                keyInput.set(this.authKey, 0);
                keyInput.set(this.uuid.slice(0, 16), 32);
                keyInput.set(this.serviceRand, 48);
                this.key11 = MD5.hash(keyInput);
                return this.key11;

            case ENCRYPTION_MODE_KEY_12:
                // key = MD5(key11 + pairRand)
                if (!this.key11) {
                    console.error('Key11 not initialized');
                    return null;
                }
                keyInput = new Uint8Array(16 + 6);  // key11(16) + pairRand(6)
                keyInput.set(this.key11, 0);
                keyInput.set(this.pairRand, 16);
                return MD5.hash(keyInput);

            default:
                console.error('Unsupported encryption mode:', mode);
                return null;
        }
    }

    // Add PKCS7 padding
    addPKCS7Padding(data) {
        const blockSize = 16;
        const padLen = blockSize - (data.length % blockSize);
        const padded = new Uint8Array(data.length + padLen);
        padded.set(data);
        for (let i = data.length; i < padded.length; i++) {
            padded[i] = padLen;
        }
        return padded;
    }

    // Remove PKCS7 padding
    removePKCS7Padding(data) {
        const padLen = data[data.length - 1];
        if (padLen > 16 || padLen === 0) return data;
        return data.slice(0, data.length - padLen);
    }

    // Build a BLE packet
    buildPacket(cmdType, data, ackSN = 0) {
        // Packet format: SN(4) + ACK_SN(4) + CMD(2) + LEN(2) + DATA(N) + CRC16(2)
        const sn = this.sendSN++;
        const dataLen = data ? data.length : 0;
        const packet = new Uint8Array(14 + dataLen);  // 4+4+2+2+N+2

        // SN (4 bytes, big-endian)
        packet[0] = (sn >> 24) & 0xFF;
        packet[1] = (sn >> 16) & 0xFF;
        packet[2] = (sn >> 8) & 0xFF;
        packet[3] = sn & 0xFF;

        // ACK_SN (4 bytes)
        packet[4] = (ackSN >> 24) & 0xFF;
        packet[5] = (ackSN >> 16) & 0xFF;
        packet[6] = (ackSN >> 8) & 0xFF;
        packet[7] = ackSN & 0xFF;

        // CMD (2 bytes)
        packet[8] = (cmdType >> 8) & 0xFF;
        packet[9] = cmdType & 0xFF;

        // LEN (2 bytes)
        packet[10] = (dataLen >> 8) & 0xFF;
        packet[11] = dataLen & 0xFF;

        // DATA
        if (data && dataLen > 0) {
            packet.set(data, 12);
        }

        // CRC16 (calculate over SN to DATA)
        const crc = crc16(packet.slice(0, 12 + dataLen));
        packet[12 + dataLen] = (crc >> 8) & 0xFF;
        packet[12 + dataLen + 1] = crc & 0xFF;

        return packet;
    }

    // Encrypt a packet (or just add mode header for mode 0)
    async encryptPacket(packet, mode = 11) {
        if (mode === 0) {
            // Mode 0: no encryption, but still prepend mode byte
            const result = new Uint8Array(1 + packet.length);
            result[0] = 0;
            result.set(packet, 1);
            return result;
        }

        const key = this.generateKey(mode);
        if (!key) {
            throw new Error('Failed to generate encryption key');
        }

        // Generate random IV
        const iv = new Uint8Array(16);
        crypto.getRandomValues(iv);

        // Pad the data
        const paddedData = this.addPKCS7Padding(packet);

        // Encrypt
        const encrypted = await TuyaAES.encrypt(paddedData, key, iv);

        // Build encrypted packet: mode(1) + IV(16) + encrypted_data
        const result = new Uint8Array(1 + 16 + encrypted.length);
        result[0] = mode;
        result.set(iv, 1);
        result.set(encrypted, 17);

        return result;
    }

    // Decrypt a packet
    async decryptPacket(encryptedPacket) {
        if (encryptedPacket.length < 17) {
            throw new Error('Packet too short');
        }

        const mode = encryptedPacket[0];

        if (mode === 0) {
            return encryptedPacket.slice(1);
        }

        // Extract IV and update serviceRand for key generation
        const iv = encryptedPacket.slice(1, 17);
        if (mode === 11 || mode === 16) {
            this.serviceRand = iv;
        }

        const key = this.generateKey(mode);
        if (!key) {
            throw new Error('Failed to generate decryption key');
        }

        const encryptedData = encryptedPacket.slice(17);
        const decrypted = await TuyaAES.decrypt(encryptedData, key, iv);

        return this.removePKCS7Padding(decrypted);
    }

    // Parse a decrypted packet
    parsePacket(data) {
        if (data.length < 14) {
            throw new Error('Packet too short');
        }

        const sn = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
        const ackSN = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
        const cmdType = (data[8] << 8) | data[9];
        const dataLen = (data[10] << 8) | data[11];

        const payload = data.slice(12, 12 + dataLen);

        // Verify CRC
        const expectedCRC = (data[12 + dataLen] << 8) | data[12 + dataLen + 1];
        const actualCRC = crc16(data.slice(0, 12 + dataLen));

        if (expectedCRC !== actualCRC) {
            console.warn('CRC mismatch:', expectedCRC, '!=', actualCRC);
        }

        return {
            sn,
            ackSN,
            cmdType,
            payload
        };
    }

    // Encode for BLE transmission (framing)
    encodeForTransmission(data) {
        // Simple framing: subpkg_num(varint) + [total_len(varint) + version_seq] + data
        // For single packet transmission
        const version = 4;  // Protocol version 4.x
        const seq = this.sendSN & 0x0F;

        // Encode total length as varint
        let lenBytes = [];
        let len = data.length;
        do {
            let byte = len & 0x7F;
            len >>= 7;
            if (len > 0) byte |= 0x80;
            lenBytes.push(byte);
        } while (len > 0);

        // Build frame: 0x00 (subpkg 0) + len_varint + version_seq + data
        const frame = new Uint8Array(1 + lenBytes.length + 1 + data.length);
        frame[0] = 0x00;  // First subpackage
        for (let i = 0; i < lenBytes.length; i++) {
            frame[1 + i] = lenBytes[i];
        }
        frame[1 + lenBytes.length] = (version << 4) | seq;
        frame.set(data, 2 + lenBytes.length);

        return frame;
    }

    // Set pair random (used after pairing)
    setPairRandom(random) {
        this.pairRand = random.slice(0, 6);
    }

    // Create a device info query command
    createDeviceInfoQuery() {
        return this.buildPacket(0x0000, null);  // FRM_QRY_DEV_INFO_REQ
    }

    // Create a transparent data command (for custom config)
    // channelType: BLE_CHANNLE_NETCFG = 1 for WiFi config
    createTransparentDataCmd(channelType, jsonData) {
        // Format for non-subpacket:
        // [0]: flags_low (0x00)
        // [1]: flags_high (0x00 = not subpacket)
        // [2]: channel_type_high
        // [3]: channel_type_low
        // [4+]: JSON data string
        const jsonStr = JSON.stringify(jsonData);
        const jsonBytes = this.stringToBytes(jsonStr);
        const data = new Uint8Array(4 + jsonBytes.length);
        data[0] = 0x00;  // flags low
        data[1] = 0x00;  // flags high (not subpacket)
        data[2] = (channelType >> 8) & 0xFF;  // channel type high
        data[3] = channelType & 0xFF;         // channel type low
        data.set(jsonBytes, 4);

        return this.buildPacket(0x801B, data);  // FRM_DOWNLINK_TRANSPARENT_REQ
    }
}

// Export for use in main script
window.TuyaBLECrypto = TuyaBLECrypto;
