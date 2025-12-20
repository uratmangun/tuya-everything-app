// DevKit Communication Server with WebRTC/Opus Audio
// Provides: TCP (5000), UDP (5001 for raw PCM), HTTP+WebSocket (3000), WebRTC Audio
package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/hraban/opus"
	"github.com/pion/webrtc/v3"
	"github.com/pion/webrtc/v3/pkg/media"
)

// Config from environment
var (
	authUsername  = getEnv("AUTH_USERNAME", "admin")
	authPassword  = getEnv("AUTH_PASSWORD", "changeme123")
	authToken     = getEnv("AUTH_TOKEN", "devkit-secret-token")
	sessionSecret = getEnv("SESSION_SECRET", generateRandomHex(32))
	httpPort      = getEnv("HTTP_PORT", "3000")
	tcpPort       = getEnv("TCP_PORT", "5000")
	udpPort       = getEnv("UDP_PORT", "5001")
)

// Audio configuration
const (
	sampleRate       = 16000
	channels         = 1
	frameSizeMs      = 20
	frameSizeSamples = sampleRate * frameSizeMs / 1000 // 320 samples
	frameSizeBytes   = frameSizeSamples * 2            // 640 bytes (16-bit)
)

// Session store
type Session struct {
	Username  string
	CreatedAt time.Time
}

var (
	sessions     = make(map[string]*Session)
	sessionMutex sync.RWMutex
)

// DevKit connection
var (
	devkitConn          net.Conn
	devkitAuthenticated bool
	devkitMutex         sync.RWMutex
)

// WebSocket clients
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

// Audio stream clients (HTTP chunked - legacy fallback)
var (
	audioClients   = make(map[chan []byte]bool)
	audioClientsMu sync.RWMutex
)

// WebRTC Audio Track (global, shared by all peer connections)
var (
	audioTrack      *webrtc.TrackLocalStaticSample
	audioTrackMutex sync.RWMutex
	opusEncoder     *opus.Encoder
	opusMutex       sync.Mutex
)

// WebRTC Peer Connections
var (
	peerConnections   = make(map[*webrtc.PeerConnection]bool)
	peerConnectionsMu sync.RWMutex
)

// Jitter Buffer - decouples UDP reception from WebRTC sending
var (
	jitterBuffer    = make(chan []int16, 50) // Buffer up to 50 frames (~1 second)
	jitterDropCount uint64
)

// UDP packet statistics
var (
	udpPacketCount  uint64
	udpLastSeq      uint8
	udpSeqJumps     uint64
	udpBytesDecoded uint64
	udpPingCount    uint64
)

func getEnv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func generateRandomHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func getLocalIP() string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return "127.0.0.1"
	}
	for _, addr := range addrs {
		if ipnet, ok := addr.(*net.IPNet); ok && !ipnet.IP.IsLoopback() {
			if ipnet.IP.To4() != nil {
				return ipnet.IP.String()
			}
		}
	}
	return "127.0.0.1"
}

// ==================== Session Management ====================

func signSession(sessionID string) string {
	h := hmac.New(sha256.New, []byte(sessionSecret))
	h.Write([]byte(sessionID))
	sig := hex.EncodeToString(h.Sum(nil))
	return sessionID + "." + sig
}

func verifySession(signed string) (string, bool) {
	parts := strings.SplitN(signed, ".", 2)
	if len(parts) != 2 {
		return "", false
	}
	sessionID, sig := parts[0], parts[1]
	h := hmac.New(sha256.New, []byte(sessionSecret))
	h.Write([]byte(sessionID))
	expected := hex.EncodeToString(h.Sum(nil))
	if !hmac.Equal([]byte(sig), []byte(expected)) {
		return "", false
	}
	return sessionID, true
}

func validateSession(sessionID string) bool {
	if sessionID == "" {
		return false
	}
	sessionMutex.RLock()
	sess, exists := sessions[sessionID]
	sessionMutex.RUnlock()
	if !exists {
		return false
	}
	// Sessions expire after 24 hours
	if time.Since(sess.CreatedAt) > 24*time.Hour {
		sessionMutex.Lock()
		delete(sessions, sessionID)
		sessionMutex.Unlock()
		return false
	}
	return true
}

// ==================== Opus Encoder ====================

func initOpusEncoder() error {
	var err error
	opusEncoder, err = opus.NewEncoder(sampleRate, channels, opus.AppVoIP)
	if err != nil {
		return fmt.Errorf("failed to create Opus encoder: %v", err)
	}
	// Set bitrate to 24kbps for good quality
	opusEncoder.SetBitrate(24000)
	log.Println("[OPUS] Encoder initialized: 16kHz, mono, 24kbps")
	return nil
}

func encodePCMToOpus(pcmData []int16) ([]byte, error) {
	opusMutex.Lock()
	defer opusMutex.Unlock()

	if opusEncoder == nil {
		return nil, fmt.Errorf("opus encoder not initialized")
	}

	opusBuffer := make([]byte, 1024)
	n, err := opusEncoder.Encode(pcmData, opusBuffer)
	if err != nil {
		return nil, err
	}
	return opusBuffer[:n], nil
}

// ==================== WebRTC ====================

func initWebRTCTrack() error {
	var err error
	audioTrack, err = webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus},
		"audio", "doorbell-audio",
	)
	if err != nil {
		return fmt.Errorf("failed to create WebRTC track: %v", err)
	}
	log.Println("[WEBRTC] Audio track initialized (Opus)")
	return nil
}

func writeAudioToTrack(opusData []byte) {
	audioTrackMutex.RLock()
	track := audioTrack
	audioTrackMutex.RUnlock()

	if track == nil {
		return
	}

	// Write sample with correct duration for jitter buffer
	err := track.WriteSample(media.Sample{
		Data:     opusData,
		Duration: time.Millisecond * time.Duration(frameSizeMs),
	})
	if err != nil {
		log.Printf("[WEBRTC] Write sample error: %v", err)
	}
}

// ==================== TCP Server ====================

func startTCPServer() {
	listener, err := net.Listen("tcp", "0.0.0.0:"+tcpPort)
	if err != nil {
		log.Fatalf("[TCP] Failed to start: %v", err)
	}
	log.Printf("[TCP] Server listening on port %s", tcpPort)
	localIP := getLocalIP()
	log.Printf("[TCP] DevKit should connect to: %s:%s", localIP, tcpPort)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("[TCP] Accept error: %v", err)
			continue
		}
		log.Printf("[TCP] DevKit connecting: %s", conn.RemoteAddr())
		go handleDevKitConnection(conn)
	}
}

func handleDevKitConnection(conn net.Conn) {
	defer conn.Close()

	// Enable TCP keepalive at OS level
	if tcpConn, ok := conn.(*net.TCPConn); ok {
		tcpConn.SetKeepAlive(true)
		tcpConn.SetKeepAlivePeriod(30 * time.Second)
	}

	// Helper to send length-prefixed message
	sendLengthPrefixed := func(msg string) {
		msgBytes := []byte(msg)
		lenBuf := make([]byte, 4)
		binary.LittleEndian.PutUint32(lenBuf, uint32(len(msgBytes)))
		conn.Write(append(lenBuf, msgBytes...))
	}

	// Helper to read length-prefixed message
	readLengthPrefixed := func() (string, error) {
		// Read 4-byte length header
		header := make([]byte, 4)
		_, err := io.ReadFull(conn, header)
		if err != nil {
			return "", err
		}
		msgLen := binary.LittleEndian.Uint32(header)
		if msgLen > 4096 {
			return "", fmt.Errorf("message too large: %d", msgLen)
		}
		// Read message body
		body := make([]byte, msgLen)
		_, err = io.ReadFull(conn, body)
		if err != nil {
			return "", err
		}
		return string(body), nil
	}

	// First message should be authentication (length-prefixed)
	conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	message, err := readLengthPrefixed()
	if err != nil {
		log.Printf("[TCP] Auth read failed: %v", err)
		return
	}

	// Parse and validate token
	message = strings.TrimSpace(message)
	if !strings.HasPrefix(message, "auth:") {
		log.Printf("[TCP] Invalid auth format from %s", conn.RemoteAddr())
		sendLengthPrefixed("error:auth_required")
		return
	}

	token := strings.TrimPrefix(message, "auth:")
	if token != authToken {
		log.Printf("[TCP] Invalid token from %s", conn.RemoteAddr())
		sendLengthPrefixed("error:invalid_token")
		return
	}

	// Auth successful
	sendLengthPrefixed("auth:ok")
	conn.SetReadDeadline(time.Time{})
	log.Printf("[TCP] DevKit authenticated: %s", conn.RemoteAddr())

	devkitMutex.Lock()
	if devkitConn != nil {
		devkitConn.Close()
	}
	devkitConn = conn
	devkitAuthenticated = true
	devkitMutex.Unlock()

	broadcastToWeb(map[string]interface{}{
		"type":      "devkit_status",
		"connected": true,
		"address":   conn.RemoteAddr().String(),
	})

	defer func() {
		devkitMutex.Lock()
		if devkitConn == conn {
			devkitConn = nil
			devkitAuthenticated = false
		}
		devkitMutex.Unlock()

		broadcastToWeb(map[string]interface{}{
			"type":      "devkit_status",
			"connected": false,
		})
	}()

	// Read messages from DevKit (length-prefixed)
	for {
		message, err := readLengthPrefixed()
		if err != nil {
			if err != io.EOF {
				log.Printf("[TCP] Read error: %v", err)
			}
			break
		}

		message = strings.TrimSpace(message)
		if message == "" {
			continue
		}

		log.Printf("[TCP] Received from DevKit: %s", message)

		if message == "pong" {
			log.Printf("[TCP] Keep-alive pong received")
			continue
		}

		broadcastToWeb(map[string]interface{}{
			"type":      "devkit_message",
			"data":      message,
			"timestamp": time.Now().Format(time.RFC3339),
		})
	}
}

func sendToDevKit(message string) error {
	devkitMutex.RLock()
	conn := devkitConn
	authenticated := devkitAuthenticated
	devkitMutex.RUnlock()

	if conn == nil || !authenticated {
		return fmt.Errorf("devkit not connected")
	}

	// Frame message with length prefix (4 bytes little-endian)
	msgBytes := []byte(message)
	lenBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(lenBuf, uint32(len(msgBytes)))

	conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	_, err := conn.Write(append(lenBuf, msgBytes...))
	conn.SetWriteDeadline(time.Time{})

	if err != nil {
		log.Printf("[TCP] Send error: %v", err)
		return err
	}
	log.Printf("[TCP] Sent to DevKit: %s", message)
	return nil
}

// ==================== UDP Server (Raw PCM to Opus/WebRTC) ====================

func startUDPServer() {
	addr, err := net.ResolveUDPAddr("udp", "0.0.0.0:"+udpPort)
	if err != nil {
		log.Fatalf("[UDP] Failed to resolve address: %v", err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatalf("[UDP] Failed to start: %v", err)
	}
	log.Printf("[UDP] Raw PCM audio server listening on port %s (jitter buffer enabled)", udpPort)

	// Set larger buffer for better performance
	conn.SetReadBuffer(1024 * 1024) // 1MB buffer

	buf := make([]byte, 2048)

	for {
		n, remoteAddr, err := conn.ReadFromUDP(buf)
		if err != nil {
			log.Printf("[UDP] Read error: %v", err)
			continue
		}

		// Handle UDP keepalive ping (1-byte 0xFF)
		if n == 1 && buf[0] == 0xFF {
			udpPingCount++
			if udpPingCount%10 == 1 {
				log.Printf("[UDP] Keepalive ping #%d from %s", udpPingCount, remoteAddr)
			}
			continue
		}

		// Minimum audio packet: 1 byte seq + some PCM data
		if n < 2 {
			continue
		}

		// Extract sequence number (first byte)
		seq := buf[0]
		pcmData := buf[1:n]

		// Check for sequence discontinuity
		expectedSeq := udpLastSeq + 1
		if udpPacketCount > 0 && seq != expectedSeq {
			udpSeqJumps++
			if udpSeqJumps%100 == 1 {
				log.Printf("[UDP] Sequence jump: expected %d, got %d (total: %d)",
					expectedSeq, seq, udpSeqJumps)
			}
		}
		udpLastSeq = seq

		udpPacketCount++
		if udpPacketCount%500 == 1 {
			log.Printf("[UDP] PCM packet #%d from %s: seq=%d, pcm_bytes=%d, jitter_drops=%d",
				udpPacketCount, remoteAddr, seq, len(pcmData), jitterDropCount)
		}

		// Convert bytes (Little Endian) to int16 samples
		sampleCount := len(pcmData) / 2
		if sampleCount > frameSizeSamples {
			sampleCount = frameSizeSamples
		}

		// Copy to new slice for jitter buffer
		pcmFrame := make([]int16, sampleCount)
		for i := 0; i < sampleCount; i++ {
			pcmFrame[i] = int16(pcmData[i*2]) | int16(pcmData[i*2+1])<<8
		}

		// Non-blocking send to jitter buffer
		select {
		case jitterBuffer <- pcmFrame:
			// Queued successfully
		default:
			// Buffer full - drop packet to maintain real-time
			jitterDropCount++
			if jitterDropCount%100 == 1 {
				log.Printf("[JITTER] Buffer full, dropped %d packets", jitterDropCount)
			}
		}
	}
}

// Pacer - reads from jitter buffer at steady 20ms intervals
func startPacer() {
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()

	log.Println("[PACER] Started - feeding WebRTC at 20ms intervals")

	for range ticker.C {
		select {
		case pcmData := <-jitterBuffer:
			// Encode to Opus
			opusData, err := encodePCMToOpus(pcmData)
			if err != nil {
				continue
			}
			// Write to WebRTC track
			writeAudioToTrack(opusData)

		default:
			// Buffer empty - let browser's PLC handle it
		}
	}
}

// ==================== WebSocket Handler ====================

func handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[WS] Upgrade error: %v", err)
		return
	}

	clientIP := r.RemoteAddr
	log.Printf("[WS] Web client connecting: %s", clientIP)

	client := &WSClient{
		conn:          conn,
		authenticated: false,
		isAlive:       true,
	}

	// Check for session cookie
	if cookie, err := r.Cookie("session"); err == nil {
		if sessionID, valid := verifySession(cookie.Value); valid && validateSession(sessionID) {
			client.authenticated = true
			log.Printf("[WS] Web client authenticated: %s", clientIP)
		}
	}

	wsClientsMu.Lock()
	wsClients[client] = true
	wsClientsMu.Unlock()

	defer func() {
		wsClientsMu.Lock()
		delete(wsClients, client)
		wsClientsMu.Unlock()
		conn.Close()
		log.Printf("[WS] Web client disconnected: %s", clientIP)
	}()

	// Send auth status and current DevKit status
	devkitMutex.RLock()
	devkitConnected := devkitAuthenticated
	devkitMutex.RUnlock()

	if client.authenticated {
		sendWSMessage(client, map[string]interface{}{
			"type": "auth_success",
			"devkit": map[string]interface{}{
				"connected": devkitConnected,
			},
			"serverIP": getLocalIP(),
			"tcpPort":  tcpPort,
		})
	} else {
		sendWSMessage(client, map[string]interface{}{
			"type": "auth_required",
		})
	}

	sendWSMessage(client, map[string]interface{}{
		"type":      "devkit_status",
		"connected": devkitConnected,
	})

	// Read messages from web client
	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}

		var data map[string]interface{}
		if err := json.Unmarshal(msg, &data); err != nil {
			continue
		}

		log.Printf("[WS] Received from web: %v", data)

		msgType, _ := data["type"].(string)

		switch msgType {
		case "ping":
			// Respond to keepalive ping
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

func sendWSMessage(client *WSClient, data map[string]interface{}) {
	client.mu.Lock()
	defer client.mu.Unlock()
	client.conn.WriteJSON(data)
}

func broadcastToWeb(data map[string]interface{}) {
	wsClientsMu.RLock()
	defer wsClientsMu.RUnlock()

	for client := range wsClients {
		sendWSMessage(client, data)
	}
}

func broadcastAudioToWeb(pcm []byte) {
	audioClientsMu.RLock()
	defer audioClientsMu.RUnlock()

	for ch := range audioClients {
		select {
		case ch <- pcm:
		default:
			// Channel full, skip
		}
	}
}

// ==================== WebRTC Signaling ====================

func handleWebRTCOffer(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var offer webrtc.SessionDescription
	if err := json.NewDecoder(r.Body).Decode(&offer); err != nil {
		http.Error(w, "Invalid offer: "+err.Error(), http.StatusBadRequest)
		return
	}

	// Create peer connection with STUN + self-hosted TURN server
	config := webrtc.Configuration{
		ICEServers: []webrtc.ICEServer{
			// Google's free STUN (works for ~80% of connections)
			{URLs: []string{"stun:stun.l.google.com:19302"}},
			{URLs: []string{"stun:stun1.l.google.com:19302"}},
			// Self-hosted TURN server on VPS (relay for remaining ~20%)
			{
				URLs:       []string{"turn:13.212.218.43:3478"},
				Username:   "turnuser",
				Credential: "TuyaT5DevKit2024",
			},
			{
				URLs:       []string{"turn:13.212.218.43:3478?transport=tcp"},
				Username:   "turnuser",
				Credential: "TuyaT5DevKit2024",
			},
		},
	}

	peerConnection, err := webrtc.NewPeerConnection(config)
	if err != nil {
		http.Error(w, "Failed to create peer connection: "+err.Error(), http.StatusInternalServerError)
		return
	}

	// Track peer connections
	peerConnectionsMu.Lock()
	peerConnections[peerConnection] = true
	log.Printf("[WEBRTC] New peer connection, total: %d", len(peerConnections))
	peerConnectionsMu.Unlock()

	// Handle connection state changes
	peerConnection.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		log.Printf("[WEBRTC] Connection state: %s", state.String())
		if state == webrtc.PeerConnectionStateFailed ||
			state == webrtc.PeerConnectionStateClosed ||
			state == webrtc.PeerConnectionStateDisconnected {
			peerConnectionsMu.Lock()
			delete(peerConnections, peerConnection)
			log.Printf("[WEBRTC] Peer removed, remaining: %d", len(peerConnections))
			peerConnectionsMu.Unlock()
		}
	})

	// Log ICE connection state
	peerConnection.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		log.Printf("[WEBRTC] ICE connection state: %s", state.String())
	})

	// Log ICE candidates for debugging
	peerConnection.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c != nil {
			log.Printf("[WEBRTC] ICE candidate: %s", c.String())
		}
	})

	// Log ICE gathering state
	peerConnection.OnICEGatheringStateChange(func(state webrtc.ICEGathererState) {
		log.Printf("[WEBRTC] ICE gathering state: %s", state.String())
	})

	// Add the audio track
	audioTrackMutex.RLock()
	track := audioTrack
	audioTrackMutex.RUnlock()

	if track != nil {
		_, err = peerConnection.AddTrack(track)
		if err != nil {
			http.Error(w, "Failed to add track: "+err.Error(), http.StatusInternalServerError)
			peerConnection.Close()
			return
		}
	}

	// Set remote description (offer from client)
	if err := peerConnection.SetRemoteDescription(offer); err != nil {
		http.Error(w, "Failed to set remote description: "+err.Error(), http.StatusBadRequest)
		peerConnection.Close()
		return
	}

	// Create answer
	answer, err := peerConnection.CreateAnswer(nil)
	if err != nil {
		http.Error(w, "Failed to create answer: "+err.Error(), http.StatusInternalServerError)
		peerConnection.Close()
		return
	}

	// Gather ICE candidates
	gatherComplete := webrtc.GatheringCompletePromise(peerConnection)

	// Set local description
	if err := peerConnection.SetLocalDescription(answer); err != nil {
		http.Error(w, "Failed to set local description: "+err.Error(), http.StatusInternalServerError)
		peerConnection.Close()
		return
	}

	// Wait for ICE gathering to complete
	select {
	case <-gatherComplete:
	case <-time.After(10 * time.Second):
		log.Printf("[WEBRTC] ICE gathering timeout")
	}

	// Send answer with ICE candidates
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(peerConnection.LocalDescription())
	log.Printf("[WEBRTC] Answer sent to client")
}

// ==================== TCP Keepalive ====================

func startTCPKeepalive() {
	ticker := time.NewTicker(30 * time.Second)
	for range ticker.C {
		devkitMutex.RLock()
		conn := devkitConn
		authenticated := devkitAuthenticated
		devkitMutex.RUnlock()

		if conn != nil && authenticated {
			if err := sendToDevKit("ping"); err != nil {
				log.Printf("[TCP] Keepalive failed: %v", err)
			} else {
				log.Printf("[TCP] Sent keepalive ping to DevKit")
			}
		}
	}
}

// ==================== WebSocket Pinger ====================

func startWSPinger() {
	ticker := time.NewTicker(30 * time.Second)
	for range ticker.C {
		wsClientsMu.RLock()
		clients := make([]*WSClient, 0, len(wsClients))
		for c := range wsClients {
			clients = append(clients, c)
		}
		wsClientsMu.RUnlock()

		for _, client := range clients {
			client.mu.Lock()
			if !client.isAlive {
				client.conn.Close()
				client.mu.Unlock()
				continue
			}
			client.isAlive = false
			client.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			client.conn.WriteMessage(websocket.PingMessage, nil)
			client.conn.SetWriteDeadline(time.Time{})
			client.mu.Unlock()
		}
	}
}

// ==================== HTTP Handlers ====================

func handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var creds struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}

	if err := json.NewDecoder(r.Body).Decode(&creds); err != nil {
		http.Error(w, "Invalid request", http.StatusBadRequest)
		return
	}

	if creds.Username != authUsername || creds.Password != authPassword {
		log.Printf("[AUTH] Failed login for: %s", creds.Username)
		http.Error(w, "Invalid credentials", http.StatusUnauthorized)
		return
	}

	// Create session
	sessionID := generateRandomHex(32)
	sessionMutex.Lock()
	sessions[sessionID] = &Session{
		Username:  creds.Username,
		CreatedAt: time.Now(),
	}
	sessionMutex.Unlock()

	signedSession := signSession(sessionID)
	http.SetCookie(w, &http.Cookie{
		Name:     "session",
		Value:    signedSession,
		Path:     "/",
		HttpOnly: true,
		Secure:   r.TLS != nil,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   86400, // 24 hours
	})

	log.Printf("[AUTH] User logged in: %s", creds.Username)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]bool{"success": true})
}

func handleCheckAuth(w http.ResponseWriter, r *http.Request) {
	cookie, err := r.Cookie("session")
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		json.NewEncoder(w).Encode(map[string]bool{"authenticated": false})
		return
	}

	sessionID, valid := verifySession(cookie.Value)
	authenticated := valid && validateSession(sessionID)

	w.Header().Set("Content-Type", "application/json")
	if !authenticated {
		w.WriteHeader(http.StatusUnauthorized)
	}
	json.NewEncoder(w).Encode(map[string]bool{"authenticated": authenticated})
}

func handleLogout(w http.ResponseWriter, r *http.Request) {
	if cookie, err := r.Cookie("session"); err == nil {
		if sessionID, valid := verifySession(cookie.Value); valid {
			sessionMutex.Lock()
			delete(sessions, sessionID)
			sessionMutex.Unlock()
		}
	}

	http.SetCookie(w, &http.Cookie{
		Name:     "session",
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		MaxAge:   -1,
	})

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]bool{"success": true})
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	devkitMutex.RLock()
	connected := devkitAuthenticated
	var addr string
	if devkitConn != nil {
		addr = devkitConn.RemoteAddr().String()
	}
	devkitMutex.RUnlock()

	peerConnectionsMu.RLock()
	webrtcClients := len(peerConnections)
	peerConnectionsMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"devkit": map[string]interface{}{
			"connected": connected,
			"address":   addr,
		},
		"audio": map[string]interface{}{
			"packets":        udpPacketCount,
			"sequenceJumps":  udpSeqJumps,
			"keepalivePings": udpPingCount,
		},
		"webrtc": map[string]interface{}{
			"clients": webrtcClients,
		},
	})
}

func handleToken(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"token": authToken})
}

func handleSend(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Command string `json:"command"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request", http.StatusBadRequest)
		return
	}

	if err := sendToDevKit(req.Command); err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusServiceUnavailable)
		json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]bool{"success": true})
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func handleAudioStream(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "audio/x-wav")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Transfer-Encoding", "chunked")

	ch := make(chan []byte, 100)

	audioClientsMu.Lock()
	audioClients[ch] = true
	log.Printf("[AUDIO] HTTP client connected, total: %d", len(audioClients))
	audioClientsMu.Unlock()

	defer func() {
		audioClientsMu.Lock()
		delete(audioClients, ch)
		log.Printf("[AUDIO] HTTP client disconnected, remaining: %d", len(audioClients))
		audioClientsMu.Unlock()
		close(ch)
	}()

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "Streaming unsupported", http.StatusInternalServerError)
		return
	}

	for data := range ch {
		if _, err := w.Write(data); err != nil {
			break
		}
		flusher.Flush()
	}
}

// ==================== Auth Middleware ====================

func authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.Path

		// Public endpoints
		publicPaths := []string{
			"/login.html",
			"/api/login",
			"/api/check-auth",
			"/health",
			"/webrtc-offer", // Allow WebRTC signaling
		}

		for _, p := range publicPaths {
			if path == p {
				next.ServeHTTP(w, r)
				return
			}
		}

		// Allow static assets
		if strings.HasPrefix(path, "/assets/") ||
			strings.HasSuffix(path, ".css") ||
			strings.HasSuffix(path, ".js") ||
			strings.HasSuffix(path, ".ico") {
			next.ServeHTTP(w, r)
			return
		}

		// Allow WebSocket upgrades
		if r.Header.Get("Upgrade") == "websocket" {
			next.ServeHTTP(w, r)
			return
		}

		// Check session cookie
		cookie, err := r.Cookie("session")
		if err != nil {
			if !strings.HasPrefix(path, "/api/") {
				http.Redirect(w, r, "/login.html", http.StatusFound)
				return
			}
			http.Error(w, "Unauthorized", http.StatusUnauthorized)
			return
		}

		sessionID, valid := verifySession(cookie.Value)
		if !valid || !validateSession(sessionID) {
			if !strings.HasPrefix(path, "/api/") {
				http.Redirect(w, r, "/login.html", http.StatusFound)
				return
			}
			http.Error(w, "Session expired", http.StatusUnauthorized)
			return
		}

		next.ServeHTTP(w, r)
	})
}

// ==================== Main ====================

func main() {
	localIP := getLocalIP()

	// Initialize Opus encoder
	if err := initOpusEncoder(); err != nil {
		log.Fatalf("Failed to init Opus: %v", err)
	}

	// Initialize WebRTC track
	if err := initWebRTCTrack(); err != nil {
		log.Fatalf("Failed to init WebRTC: %v", err)
	}

	// Start TCP server
	go startTCPServer()

	// Start UDP server (Raw PCM -> Jitter Buffer)
	go startUDPServer()

	// Start Pacer (Jitter Buffer -> Opus -> WebRTC at steady 20ms)
	go startPacer()

	// Start WebSocket pinger
	go startWSPinger()

	// Start TCP keepalive pinger
	go startTCPKeepalive()

	// Setup HTTP routes
	mux := http.NewServeMux()

	// Static files directory
	publicDir := filepath.Join(".", "public")
	if _, err := os.Stat(publicDir); os.IsNotExist(err) {
		publicDir = filepath.Join("/app", "public")
	}
	fs := http.FileServer(http.Dir(publicDir))

	// Root handler
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Upgrade") == "websocket" {
			handleWebSocket(w, r)
			return
		}
		fs.ServeHTTP(w, r)
	})

	// API routes
	mux.HandleFunc("/api/login", handleLogin)
	mux.HandleFunc("/api/check-auth", handleCheckAuth)
	mux.HandleFunc("/api/logout", handleLogout)
	mux.HandleFunc("/api/status", handleStatus)
	mux.HandleFunc("/api/token", handleToken)
	mux.HandleFunc("/api/send", handleSend)
	mux.HandleFunc("/health", handleHealth)
	mux.HandleFunc("/audio-stream", handleAudioStream)
	mux.HandleFunc("/webrtc-offer", handleWebRTCOffer)

	// Redirect /ble to /ble/index.html
	mux.HandleFunc("/ble", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/ble/index.html", http.StatusFound)
	})

	// Apply auth middleware
	handler := authMiddleware(mux)

	fmt.Println()
	fmt.Println(strings.Repeat("=", 60))
	fmt.Println("  DevKit Communication Server (WebRTC/Opus Audio)")
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  Web UI + WS: http://%s:%s\n", localIP, httpPort)
	fmt.Printf("  TCP Server:  %s:%s (for DevKit commands)\n", localIP, tcpPort)
	fmt.Printf("  UDP Server:  %s:%s (for raw PCM audio)\n", localIP, udpPort)
	fmt.Println()
	fmt.Printf("  Auth Username: %s\n", authUsername)
	fmt.Printf("  Auth Password: %s\n", strings.Repeat("*", len(authPassword)))
	fmt.Printf("  Auth Token:    %s...\n", authToken[:8])
	fmt.Println(strings.Repeat("=", 60))
	fmt.Println()

	log.Printf("[HTTP] Server starting on port %s", httpPort)
	if err := http.ListenAndServe("0.0.0.0:"+httpPort, handler); err != nil {
		log.Fatalf("[HTTP] Server failed: %v", err)
	}
}
