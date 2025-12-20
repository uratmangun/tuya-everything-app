FROM docker.io/library/golang:1.21-alpine AS builder

WORKDIR /build

# Install build dependencies for Opus (CGO required)
RUN apk add --no-cache git opus-dev opusfile-dev gcc musl-dev pkgconfig

# Copy go mod files
COPY go.mod go.sum* ./

# Download dependencies
RUN go mod download

# Copy source
COPY main.go ./

# Build with CGO enabled for Opus support
RUN CGO_ENABLED=1 GOOS=linux go build -a -o server .

# Final stage - minimal image with Opus runtime
FROM docker.io/library/alpine:latest

WORKDIR /app

# Install Opus runtime library
RUN apk add --no-cache opus opusfile

# Copy binary from builder
COPY --from=builder /build/server .

# Copy public files
COPY public ./public

# Expose ports: HTTP, TCP, UDP (for raw PCM audio)
EXPOSE 3000 5000 5001/udp

# Run
CMD ["./server"]
