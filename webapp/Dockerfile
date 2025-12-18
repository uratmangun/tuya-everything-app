FROM node:20-alpine

WORKDIR /app

# Copy package files
COPY package*.json ./

# Install dependencies
RUN npm install --omit=dev

# Copy application code
COPY . .

# Expose ports
# 3000 - Web UI (HTTP + WebSocket)
# 5000 - TCP for DevKit control
EXPOSE 3000 5000

# Start the server
CMD ["node", "server.js"]
