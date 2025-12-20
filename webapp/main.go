// DevKit Communication Server in Go
// Provides: TCP (5000), UDP (5001 with G.711 decoding), HTTP+WebSocket (3000)
package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
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

// Audio WebSocket clients (low-latency G.711 push)
type AudioWSClient struct {
	conn *websocket.Conn
	mu   sync.Mutex
}

var (
	audioWSClients   = make(map[*AudioWSClient]bool)
	audioWSClientsMu sync.RWMutex
)

// UDP packet statistics
var (
	udpPacketCount  uint64
	udpLastSeq      uint8
	udpSeqJumps     uint64
	udpBytesDecoded uint64
	udpPingCount    uint64
)

// ==================== G.711 u-law Decoder ====================
// G.711 u-law decoding table (converts 8-bit u-law to 16-bit PCM)
var ulawToLinear = [256]int16{
	-32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956,
	-23932, -22908, -21884, -20860, -19836, -18812, -17788, -16764,
	-15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412,
	-11900, -11388, -10876, -10364, -9852, -9340, -8828, -8316,
	-7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
	-5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
	-3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
	-2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
	-1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
	-1372, -1308, -1244, -1180, -1116, -1052, -988, -924,
	-876, -844, -812, -780, -748, -716, -684, -652,
	-620, -588, -556, -524, -492, -460, -428, -396,
	-372, -356, -340, -324, -308, -292, -276, -260,
	-244, -228, -212, -196, -180, -164, -148, -132,
	-120, -112, -104, -96, -88, -80, -72, -64,
	-56, -48, -40, -32, -24, -16, -8, 0,
	32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
	23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
	15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
	11900, 11388, 10876, 10364, 9852, 9340, 8828, 8316,
	7932, 7676, 7420, 7164, 6908, 6652, 6396, 6140,
	5884, 5628, 5372, 5116, 4860, 4604, 4348, 4092,
	3900, 3772, 3644, 3516, 3388, 3260, 3132, 3004,
	2876, 2748, 2620, 2492, 2364, 2236, 2108, 1980,
	1884, 1820, 1756, 1692, 1628, 1564, 1500, 1436,
	1372, 1308, 1244, 1180, 1116, 1052, 988, 924,
	876, 844, 812, 780, 748, 716, 684, 652,
	620, 588, 556, 524, 492, 460, 428, 396,
	372, 356, 340, 324, 308, 292, 276, 260,
	244, 228, 212, 196, 180, 164, 148, 132,
	120, 112, 104, 96, 88, 80, 72, 64,
	56, 48, 40, 32, 24, 16, 8, 0,
}

// decodeG711ULaw decodes G.711 u-law bytes to 16-bit PCM
func decodeG711ULaw(ulaw []byte) []byte {
	pcm := make([]byte, len(ulaw)*2)
	for i, u := range ulaw {
		sample := ulawToLinear[u]
		// Little-endian 16-bit
		pcm[i*2] = byte(sample & 0xFF)
		pcm[i*2+1] = byte((sample >> 8) & 0xFF)
	}
	return pcm
}

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
	session, exists := sessions[sessionID]
	sessionMutex.RUnlock()
	if !exists {
		return false
	}
	// 24 hour expiry
	if time.Since(session.CreatedAt) > 24*time.Hour {
		sessionMutex.Lock()
		delete(sessions, sessionID)
		sessionMutex.Unlock()
		return false
	}
	return true
}

func getSessionFromCookie(r *http.Request) string {
	cookie, err := r.Cookie("session")
	if err != nil {
		return ""
	}
	sessionID, valid := verifySession(cookie.Value)
	if !valid {
		return ""
	}
	return sessionID
}

// ==================== Broadcast Functions ====================

func broadcastToWeb(data interface{}) {
	msg, err := json.Marshal(data)
	if err != nil {
		return
	}
	wsClientsMu.RLock()
	defer wsClientsMu.RUnlock()
	for client := range wsClients {
		if client.authenticated {
			client.mu.Lock()
			client.conn.WriteMessage(websocket.TextMessage, msg)
			client.mu.Unlock()
		}
	}
}

func broadcastAudioToWeb(data []byte) {
	audioClientsMu.RLock()
	defer audioClientsMu.RUnlock()
	for ch := range audioClients {
		select {
		case ch <- data:
		default:
			// Channel full, skip this client
		}
	}
}

// Broadcast raw G.711 data to WebSocket audio clients (low latency)
func broadcastG711ToWSClients(data []byte) {
	audioWSClientsMu.RLock()
	defer audioWSClientsMu.RUnlock()
	for client := range audioWSClients {
		client.mu.Lock()
		err := client.conn.WriteMessage(websocket.BinaryMessage, data)
		client.mu.Unlock()
		if err != nil {
			// Mark for removal but don't remove during iteration
			log.Printf("[WS-AUDIO] Write error: %v", err)
		}
	}
}

// ==================== DevKit TCP Communication ====================

func sendToDevKitRaw(data string) bool {
	devkitMutex.RLock()
	conn := devkitConn
	devkitMutex.RUnlock()
	if conn == nil {
		return false
	}
	payload := []byte(data)
	header := make([]byte, 4)
	binary.LittleEndian.PutUint32(header, uint32(len(payload)))
	_, err := conn.Write(append(header, payload...))
	return err == nil
}

func sendToDevKit(data string) bool {
	devkitMutex.RLock()
	conn := devkitConn
	auth := devkitAuthenticated
	devkitMutex.RUnlock()
	if conn == nil || !auth {
		log.Println("[TCP] Cannot send - DevKit not connected/authenticated")
		broadcastToWeb(map[string]interface{}{
			"type":      "error",
			"message":   "DevKit not connected or not authenticated",
			"timestamp": time.Now().Format(time.RFC3339),
		})
		return false
	}
	if sendToDevKitRaw(data) {
		log.Printf("[TCP] Sent to DevKit: %s", data)
		broadcastToWeb(map[string]interface{}{
			"type":      "sent_to_devkit",
			"data":      data,
			"timestamp": time.Now().Format(time.RFC3339),
		})
		return true
	}
	return false
}

// TCP Keepalive pinger - sends ping to DevKit every 30 seconds to prevent connection timeout
func startTCPKeepalive() {
	ticker := time.NewTicker(30 * time.Second)
	for range ticker.C {
		devkitMutex.RLock()
		conn := devkitConn
		auth := devkitAuthenticated
		devkitMutex.RUnlock()

		if conn != nil && auth {
			if sendToDevKitRaw("ping") {
				log.Println("[TCP] Sent keepalive ping to DevKit")
			} else {
				log.Println("[TCP] Failed to send keepalive ping")
			}
		}
	}
}

// ==================== TCP Server ====================

func startTCPServer() {
	listener, err := net.Listen("tcp", "0.0.0.0:"+tcpPort)
	if err != nil {
		log.Fatalf("[TCP] Failed to start: %v", err)
	}
	log.Printf("[TCP] Server listening on port %s", tcpPort)
	log.Printf("[TCP] DevKit should connect to: %s:%s", getLocalIP(), tcpPort)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("[TCP] Accept error: %v", err)
			continue
		}
		go handleTCPConnection(conn)
	}
}

func handleTCPConnection(conn net.Conn) {
	clientInfo := conn.RemoteAddr().String()
	log.Printf("[TCP] DevKit connecting: %s", clientInfo)

	devkitMutex.Lock()
	if devkitConn != nil {
		devkitConn.Close()
	}
	devkitConn = conn
	devkitAuthenticated = false
	devkitMutex.Unlock()

	defer func() {
		conn.Close()
		devkitMutex.Lock()
		if devkitConn == conn {
			devkitConn = nil
			devkitAuthenticated = false
		}
		devkitMutex.Unlock()
		log.Printf("[TCP] DevKit disconnected: %s", clientInfo)
		broadcastToWeb(map[string]interface{}{
			"type":      "devkit_status",
			"connected": false,
			"timestamp": time.Now().Format(time.RFC3339),
		})
	}()

	buffer := make([]byte, 0, 4096)
	readBuf := make([]byte, 1024)

	for {
		n, err := conn.Read(readBuf)
		if err != nil {
			return
		}
		buffer = append(buffer, readBuf[:n]...)

		// Parse messages: [LEN:4bytes][DATA:LEN bytes]
		for len(buffer) >= 4 {
			msgLen := binary.LittleEndian.Uint32(buffer[:4])
			if uint32(len(buffer)) < 4+msgLen {
				break
			}

			rawMessage := buffer[4 : 4+msgLen]
			buffer = buffer[4+msgLen:]

			devkitMutex.RLock()
			auth := devkitAuthenticated
			devkitMutex.RUnlock()

			if !auth {
				message := string(rawMessage)
				if strings.HasPrefix(message, "auth:") {
					token := message[5:]
					if token == authToken {
						devkitMutex.Lock()
						devkitAuthenticated = true
						devkitMutex.Unlock()
						log.Printf("[TCP] DevKit authenticated: %s", clientInfo)
						sendToDevKitRaw("auth:ok")
						broadcastToWeb(map[string]interface{}{
							"type":          "devkit_status",
							"connected":     true,
							"authenticated": true,
							"address":       conn.RemoteAddr().(*net.TCPAddr).IP.String(),
							"timestamp":     time.Now().Format(time.RFC3339),
						})
					} else {
						log.Printf("[TCP] DevKit auth failed: %s", clientInfo)
						sendToDevKitRaw("auth:failed")
						return
					}
				} else {
					log.Printf("[TCP] DevKit not authenticated, ignoring: %s", message)
					sendToDevKitRaw("auth:required")
				}
				continue
			}

			// Check for audio data
			if len(rawMessage) > 6 && string(rawMessage[:6]) == "audio:" {
				audioData := rawMessage[6:]
				log.Printf("[TCP] Audio data received: %d bytes", len(audioData))
				broadcastAudioToWeb(audioData)
				continue
			}

			// Regular text message
			message := string(rawMessage)
			log.Printf("[TCP] Received from DevKit: %s", message)

			if message == "pong" {
				log.Println("[TCP] Keep-alive pong received")
				continue
			}

			broadcastToWeb(map[string]interface{}{
				"type":      "devkit_message",
				"data":      message,
				"timestamp": time.Now().Format(time.RFC3339),
			})
		}
	}
}

// ==================== UDP Server (G.711 Relay + Decoder) ====================

func startUDPServer() {
	addr, err := net.ResolveUDPAddr("udp", "0.0.0.0:"+udpPort)
	if err != nil {
		log.Fatalf("[UDP] Failed to resolve address: %v", err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatalf("[UDP] Failed to start: %v", err)
	}
	log.Printf("[UDP] G.711 audio server listening on port %s", udpPort)

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

		// Minimum audio packet: 1 byte seq + 1 byte data
		if n < 2 {
			continue
		}

		// Extract sequence number (first byte)
		seq := buf[0]
		g711Data := buf[1:n]

		// Check for sequence discontinuity (jitter/packet loss detection)
		expectedSeq := udpLastSeq + 1
		if udpPacketCount > 0 && seq != expectedSeq {
			udpSeqJumps++
			// Only log significant jumps to avoid spam
			if udpSeqJumps%100 == 1 {
				log.Printf("[UDP] Sequence jump detected: expected %d, got %d (total jumps: %d)",
					expectedSeq, seq, udpSeqJumps)
			}
		}
		udpLastSeq = seq

		udpPacketCount++
		if udpPacketCount%100 == 1 {
			log.Printf("[UDP] G.711 packet #%d from %s: seq=%d, g711_bytes=%d",
				udpPacketCount, remoteAddr, seq, len(g711Data))
		}

		// Make a copy of the raw packet for WebSocket broadcast
		rawPacket := make([]byte, n)
		copy(rawPacket, buf[:n])

		// Broadcast raw G.711 (with seq) to WebSocket audio clients (low latency)
		broadcastG711ToWSClients(rawPacket)

		// Also decode and broadcast to HTTP streaming clients (legacy fallback)
		audioClientsMu.RLock()
		httpClientCount := len(audioClients)
		audioClientsMu.RUnlock()

		if httpClientCount > 0 {
			pcmData := decodeG711ULaw(g711Data)
			udpBytesDecoded += uint64(len(pcmData))
			broadcastAudioToWeb(pcmData)
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

	wsClientsMu.Lock()
	wsClients[client] = true
	wsClientsMu.Unlock()

	defer func() {
		conn.Close()
		wsClientsMu.Lock()
		delete(wsClients, client)
		wsClientsMu.Unlock()
		log.Printf("[WS] Web client disconnected: %s", clientIP)
	}()

	// Setup pong handler
	conn.SetPongHandler(func(string) error {
		client.isAlive = true
		return nil
	})

	// Send auth required
	conn.WriteJSON(map[string]string{"type": "auth_required"})

	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			return
		}

		var message map[string]interface{}
		if err := json.Unmarshal(msg, &message); err != nil {
			log.Printf("[WS] Parse error: %v", err)
			continue
		}

		msgType, _ := message["type"].(string)

		if msgType == "auth" {
			token, _ := message["token"].(string)
			if token == authToken {
				client.authenticated = true
				log.Printf("[WS] Web client authenticated: %s", clientIP)

				devkitMutex.RLock()
				connected := devkitConn != nil && devkitAuthenticated
				devkitMutex.RUnlock()

				conn.WriteJSON(map[string]interface{}{
					"type": "auth_success",
					"devkit": map[string]bool{
						"connected": connected,
					},
					"serverIP": getLocalIP(),
					"tcpPort":  tcpPort,
				})
			} else {
				log.Printf("[WS] Web client auth failed: %s", clientIP)
				conn.WriteJSON(map[string]string{"type": "auth_failed"})
				return
			}
			continue
		}

		if !client.authenticated {
			conn.WriteJSON(map[string]string{"type": "auth_required"})
			continue
		}

		log.Printf("[WS] Received from web: %v", message)

		if msgType == "send_to_devkit" {
			if data, ok := message["data"].(string); ok {
				sendToDevKit(data)
			}
		}
	}
}

// WebSocket ping goroutine
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
			if !client.isAlive {
				log.Println("[WS] Client not responding to ping, terminating")
				client.conn.Close()
				continue
			}
			client.isAlive = false
			client.mu.Lock()
			client.conn.WriteMessage(websocket.PingMessage, nil)
			client.mu.Unlock()
		}
	}
}

// ==================== WebSocket Audio Handler (Low Latency) ====================

func handleAudioWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[WS-AUDIO] Upgrade error: %v", err)
		return
	}

	clientIP := r.RemoteAddr
	log.Printf("[WS-AUDIO] Client connected: %s", clientIP)

	client := &AudioWSClient{
		conn: conn,
	}

	audioWSClientsMu.Lock()
	audioWSClients[client] = true
	count := len(audioWSClients)
	audioWSClientsMu.Unlock()

	log.Printf("[WS-AUDIO] Total clients: %d", count)

	defer func() {
		conn.Close()
		audioWSClientsMu.Lock()
		delete(audioWSClients, client)
		count := len(audioWSClients)
		audioWSClientsMu.Unlock()
		log.Printf("[WS-AUDIO] Client disconnected: %s (remaining: %d)", clientIP, count)
	}()

	// Keep connection open until client disconnects
	// Client only receives data, doesn't send anything meaningful
	for {
		_, _, err := conn.ReadMessage()
		if err != nil {
			break
		}
	}
}

// ==================== HTTP Handlers ====================

func authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Allow WebSocket upgrades without session auth (they have token auth)
		if r.Header.Get("Upgrade") == "websocket" {
			next.ServeHTTP(w, r)
			return
		}

		// Public paths
		publicPaths := []string{"/login.html", "/api/login", "/api/check-auth", "/health"}
		for _, p := range publicPaths {
			if r.URL.Path == p || strings.HasPrefix(r.URL.Path, p) {
				next.ServeHTTP(w, r)
				return
			}
		}

		sessionID := getSessionFromCookie(r)
		if validateSession(sessionID) {
			next.ServeHTTP(w, r)
			return
		}

		if strings.HasPrefix(r.URL.Path, "/api/") {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusUnauthorized)
			json.NewEncoder(w).Encode(map[string]string{"error": "Not authenticated"})
			return
		}

		http.Redirect(w, r, "/login.html?redirect="+r.URL.Path, http.StatusFound)
	})
}

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

	w.Header().Set("Content-Type", "application/json")

	if creds.Username == authUsername && creds.Password == authPassword {
		sessionID := generateRandomHex(32)
		sessionMutex.Lock()
		sessions[sessionID] = &Session{
			Username:  creds.Username,
			CreatedAt: time.Now(),
		}
		sessionMutex.Unlock()

		http.SetCookie(w, &http.Cookie{
			Name:     "session",
			Value:    signSession(sessionID),
			Path:     "/",
			HttpOnly: true,
			MaxAge:   86400, // 24 hours
			SameSite: http.SameSiteLaxMode,
		})

		log.Printf("[AUTH] User logged in: %s", creds.Username)
		json.NewEncoder(w).Encode(map[string]bool{"success": true})
		return
	}

	log.Printf("[AUTH] Failed login attempt for: %s", creds.Username)
	w.WriteHeader(http.StatusUnauthorized)
	json.NewEncoder(w).Encode(map[string]interface{}{
		"success": false,
		"error":   "Invalid username or password",
	})
}

func handleCheckAuth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	sessionID := getSessionFromCookie(r)
	if validateSession(sessionID) {
		sessionMutex.RLock()
		session := sessions[sessionID]
		sessionMutex.RUnlock()
		json.NewEncoder(w).Encode(map[string]interface{}{
			"authenticated": true,
			"username":      session.Username,
		})
		return
	}
	w.WriteHeader(http.StatusUnauthorized)
	json.NewEncoder(w).Encode(map[string]bool{"authenticated": false})
}

func handleLogout(w http.ResponseWriter, r *http.Request) {
	sessionID := getSessionFromCookie(r)
	if sessionID != "" {
		sessionMutex.Lock()
		delete(sessions, sessionID)
		sessionMutex.Unlock()
	}
	http.SetCookie(w, &http.Cookie{
		Name:   "session",
		Value:  "",
		Path:   "/",
		MaxAge: -1,
	})
	log.Println("[AUTH] User logged out")
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]bool{"success": true})
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":    "ok",
		"timestamp": time.Now().Format(time.RFC3339),
	})
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	devkitMutex.RLock()
	connected := devkitConn != nil
	auth := devkitAuthenticated
	var addr string
	if devkitConn != nil {
		addr = devkitConn.RemoteAddr().(*net.TCPAddr).IP.String()
	}
	devkitMutex.RUnlock()

	wsClientsMu.RLock()
	wsCount := len(wsClients)
	wsClientsMu.RUnlock()

	audioClientsMu.RLock()
	audioCount := len(audioClients)
	audioClientsMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"devkit": map[string]interface{}{
			"connected":     connected,
			"authenticated": auth,
			"address":       addr,
		},
		"server": map[string]interface{}{
			"ip":       getLocalIP(),
			"httpPort": httpPort,
			"tcpPort":  tcpPort,
			"udpPort":  udpPort,
		},
		"webClients":   wsCount,
		"audioClients": audioCount,
		"udpStats": map[string]interface{}{
			"packets":      udpPacketCount,
			"seqJumps":     udpSeqJumps,
			"bytesDecoded": udpBytesDecoded,
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
		Message string `json:"message"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request", http.StatusBadRequest)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	if req.Message == "" {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]interface{}{
			"success": false,
			"error":   "No message provided",
		})
		return
	}

	sent := sendToDevKit(req.Message)
	json.NewEncoder(w).Encode(map[string]interface{}{
		"success": sent,
		"message": map[bool]string{true: "Message sent", false: "DevKit not connected"}[sent],
	})
}

func handleAudioStream(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "no-cache, no-store")
	w.Header().Set("Connection", "keep-alive")
	// Updated format info: 16kHz sample rate from G.711 decoded audio
	w.Header().Set("X-Audio-Format", "pcm-s16le-16000hz-mono")

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "Streaming not supported", http.StatusInternalServerError)
		return
	}

	// Create buffered channel for this client
	ch := make(chan []byte, 200) // Larger buffer for G.711 streams
	audioClientsMu.Lock()
	audioClients[ch] = true
	audioClientsMu.Unlock()
	log.Printf("[AUDIO] Client connected, total: %d", len(audioClients))

	defer func() {
		audioClientsMu.Lock()
		delete(audioClients, ch)
		audioClientsMu.Unlock()
		close(ch)
		log.Printf("[AUDIO] Client disconnected, total: %d", len(audioClients))
	}()

	// Use context for cancellation
	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case data, ok := <-ch:
			if !ok {
				return
			}
			if _, err := w.Write(data); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}

// ==================== Main ====================

func main() {
	localIP := getLocalIP()

	// Start TCP server
	go startTCPServer()

	// Start UDP server (with G.711 decoding)
	go startUDPServer()

	// Start WebSocket pinger
	go startWSPinger()

	// Start TCP keepalive pinger (prevents connection timeout)
	go startTCPKeepalive()

	// Setup HTTP routes
	mux := http.NewServeMux()

	// Static files directory
	publicDir := filepath.Join(".", "public")
	if _, err := os.Stat(publicDir); os.IsNotExist(err) {
		publicDir = filepath.Join("/app", "public")
	}
	fs := http.FileServer(http.Dir(publicDir))

	// Root handler - WebSocket upgrade or static files
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		// Check if this is a WebSocket upgrade request
		if r.Header.Get("Upgrade") == "websocket" {
			handleWebSocket(w, r)
			return
		}
		// Otherwise serve static files
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
	mux.HandleFunc("/audio-stream", handleAudioStream) // HTTP streaming (legacy)
	mux.HandleFunc("/ws-audio", handleAudioWebSocket)  // WebSocket audio (low latency)

	// Redirect /ble to /ble/index.html
	mux.HandleFunc("/ble", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/ble/index.html", http.StatusFound)
	})

	// Apply auth middleware
	handler := authMiddleware(mux)

	fmt.Println()
	fmt.Println(strings.Repeat("=", 60))
	fmt.Println("  DevKit Communication Server (G.711 Audio)")
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  Web UI + WS: http://%s:%s\n", localIP, httpPort)
	fmt.Printf("  TCP Server:  %s:%s (for DevKit commands)\n", localIP, tcpPort)
	fmt.Printf("  UDP Server:  %s:%s (for G.711 audio)\n", localIP, udpPort)
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
