FROM node:20-alpine

WORKDIR /app

# Install FFmpeg for audio streaming
RUN apk add --no-cache ffmpeg

# Copy package files
COPY package*.json ./

# Install dependencies
RUN npm install --omit=dev

# Copy application code
COPY . .

# Expose ports
# 3000 - Web UI (HTTP + WebSocket)
# 5000 - TCP for DevKit control
# 5001 - UDP for DevKit audio
EXPOSE 3000 5000 5001/udp

# Start the server
CMD ["node", "server.js"]
