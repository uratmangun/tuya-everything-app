# T5AI DevKit Web Controller

A real-time web-based control system for the T5AI DevKit. This system allows you to send commands to your DevKit from a web browser via TCP connection through a VPS.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ VPS (with Cloudflare Tunnel)                                       │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │ tuya-webapp (podman container)                              │  │
│  │                                                              │  │
│  │  Port 3000: HTTP + WebSocket (via Cloudflare tunnel)        │  │
│  │  Port 5000: TCP Server (for DevKit connection)              │  │
│  └──────────────────────────────┬───────────────────────────────┘  │
│                                 │                                   │
│  ┌──────────────────────────────┴───────────────────────────────┐  │
│  │ cloudflared (Cloudflare Tunnel)                              │  │
│  │ Exposes port 3000 as https://your-domain.example.com        │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
                                   │
                              Internet
                                   │
┌──────────────────────────────────┼──────────────────────────────────┐
│ T5AI DevKit                      │                                  │
│                                  ▼                                  │
│  - Connects to VPS:5000 via TCP                                    │
│  - Sends auth token for secure connection                          │
│  - Receives commands from web UI                                   │
│  - Responds with status, plays audio, etc.                         │
└─────────────────────────────────────────────────────────────────────┘
```

## 📱 Web Bluetooth Configuration

Configure your DevKit locally via Bluetooth when the VPS is unreachable!

🔗 **Configuration Page**: https://ble-config-web.vercel.app

### Features
- **Set TCP Server**: Configure host, port, and auth token
- **Set WiFi Network**: Scan and connect to new WiFi networks  
- **View Status**: Check device connection status
- **Reboot Device**: Remotely restart the DevKit

### How to Use
1. Open https://ble-config-web.vercel.app on Chrome/Edge (Android, Windows, macOS, Linux)
2. Click **Connect to T5AI**
3. Select your DevKit from the Bluetooth device picker
4. Configure WiFi and TCP settings as needed
5. Click **Reboot** to apply changes

> ⚠️ **Note**: Web Bluetooth is not supported on Safari (iPhone). Use Chrome on Android or desktop.

## Components

### 1. Web Application (`/webapp`)

Node.js server that provides:
- **Web UI**: Beautiful dashboard to control the DevKit
- **WebSocket**: Real-time communication with the web browser
- **TCP Server**: Receives connections from the DevKit

### 2. DevKit Firmware (`/apps/tuya_cloud/object_detection`)

Tuya-based firmware for T5AI DevKit that:
- Connects to VPS via TCP
- Authenticates with a secret token
- Handles commands (ping, status, audio play, switch, etc.)
- Reports back to web UI

## Quick Setup

### Prerequisites

- T5AI DevKit with USB connection
- VPS with Docker/Podman installed
- Cloudflare account with a domain
- Python 3.x (for Tuya build tools)

### 1. Configure DevKit Firmware

```bash
cd apps/tuya_cloud/object_detection

# Copy and edit the environment file
cp .env.example .env

# Edit .env with your values:
# - TUYA_PRODUCT_ID: Your Tuya product ID
# - TUYA_OPENSDK_UUID: Your license UUID
# - TUYA_OPENSDK_AUTHKEY: Your license auth key
# - TCP_SERVER_HOST: Your VPS public IP
# - TCP_SERVER_PORT: 5000
# - TCP_AUTH_TOKEN: A secret token (must match webapp)
```

### 2. Build and Flash Firmware

```bash
cd apps/tuya_cloud/object_detection

# Build the firmware
python3 ../../../tos.py build

# Flash to DevKit (select the correct serial port)
python3 ../../../tos.py flash

# Monitor the output
python3 ../../../tos.py monitor -p /dev/ttyACM2
```

### 3. Deploy Web Application to VPS

```bash
# SSH to your VPS
ssh user@your-vps-ip

# Create directory
mkdir -p ~/tuya-webapp

# Copy webapp files (from your local machine)
scp -r webapp/* user@your-vps-ip:~/tuya-webapp/

# Create environment file
cat > ~/tuya-webapp/.env.production << EOF
AUTH_USERNAME=admin
AUTH_PASSWORD=your-secure-password
AUTH_TOKEN=your-secret-token
HTTP_PORT=3000
TCP_PORT=5000
EOF

# Build Docker image
cd ~/tuya-webapp
podman build -t tuya-webapp:latest .

# Run the container on the Cloudflare tunnel network
podman run -d \
  --name tuya-webapp \
  --network tunnel-net \
  --env-file .env.production \
  -p 3000:3000 \
  -p 5000:5000 \
  --restart unless-stopped \
  tuya-webapp:latest
```

### 4. Configure Cloudflare Tunnel

1. Go to **Cloudflare Zero Trust Dashboard**
2. Navigate to **Networks** → **Tunnels**
3. Select your tunnel
4. Add a **Public Hostname**:
   - **Subdomain**: `devkit` (or your choice)
   - **Domain**: Your domain (e.g., `example.com`)
   - **Type**: HTTP
   - **URL**: `tuya-webapp:3000`

5. Access your web UI at `https://devkit.example.com`

## Security

### Authentication Layers

1. **HTTP Basic Auth**: Protects the web UI
   - Username and password required to access the page

2. **WebSocket Token Auth**: Protects WebSocket commands
   - Token obtained after HTTP authentication
   - Required for all WebSocket messages

3. **TCP Token Auth**: Protects DevKit connection
   - DevKit sends `auth:<token>` on connect
   - Invalid tokens are rejected

### Important Security Notes

⚠️ **Never commit `.env` files with real credentials**

- Use strong, unique passwords
- Change default tokens before deployment
- Keep `.env.production` secure on your VPS
- The `.env.example` files contain only placeholders

## Available Commands

From the web UI, you can send these commands to the DevKit:

| Command | Description |
|---------|-------------|
| `ping` | DevKit responds with `pong` |
| `status` | Returns JSON with detection state, volume, heap size |
| `audio play` | Plays the alert audio on speaker |
| `audio stop` | Stops audio playback |
| `switch on` | Turns detection on (updates Tuya app) |
| `switch off` | Turns detection off |
| `mem` | Returns free heap memory |
| `reset` | Factory resets the device |

## File Structure

```
├── webapp/
│   ├── server.js           # Node.js server (HTTP + WS + TCP)
│   ├── package.json        # Dependencies
│   ├── Dockerfile          # Container build
│   ├── docker-compose.yml  # Optional compose file
│   ├── .env.example        # Example environment variables
│   ├── .gitignore          # Ignore node_modules and .env
│   └── public/
│       └── index.html      # Web UI
│
├── apps/tuya_cloud/object_detection/
│   ├── src/
│   │   ├── tuya_main.c     # Main application
│   │   ├── tcp_client.c    # TCP client for web app
│   │   └── tcp_client.h    # TCP client header
│   ├── .env.example        # Example environment variables
│   ├── .env                # Your configuration (gitignored)
│   └── CMakeLists.txt      # Build configuration
```

## Troubleshooting

### DevKit can't connect to VPS

1. Check VPS firewall allows port 5000
2. Verify `TCP_SERVER_HOST` in `.env` is correct
3. Ensure DevKit has WiFi connection
4. Check Tuya app pairing is complete

### WebSocket connection fails

1. Verify Cloudflare tunnel is running
2. Check the container is on `tunnel-net` network
3. Ensure HTTP Basic Auth credentials are correct

### Authentication fails

1. Verify `TCP_AUTH_TOKEN` matches in both:
   - `apps/tuya_cloud/object_detection/.env`
   - `webapp/.env.production`

### View container logs

```bash
podman logs tuya-webapp
```

## Development

### Run webapp locally

```bash
cd webapp
npm install
cp .env.example .env
# Edit .env with your values
node server.js
```

### Rebuild firmware after changes

```bash
cd apps/tuya_cloud/object_detection
python3 ../../../tos.py build
python3 ../../../tos.py flash
```

## License

This project uses the TuyaOpen SDK. See the main repository for license details.
