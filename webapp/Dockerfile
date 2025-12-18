FROM node:20-alpine

WORKDIR /app

# Copy package files
COPY package*.json ./

# Install dependencies
RUN npm ci --only=production

# Copy application code
COPY . .

# Expose ports
# 3000 - Web UI (HTTP)
# 3001 - WebSocket
# 5000 - TCP for DevKit
EXPOSE 3000 3001 5000

# Start the server
CMD ["node", "server.js"]
