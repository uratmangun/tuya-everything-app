FROM docker.io/library/golang:1.21-alpine AS builder

WORKDIR /build

# Copy go mod files
COPY go.mod go.sum* ./

# Download dependencies
RUN go mod download

# Copy source
COPY main.go ./

# Build static binary
RUN CGO_ENABLED=0 GOOS=linux go build -a -installsuffix cgo -o server .

# Final stage - minimal image
FROM docker.io/library/alpine:latest

WORKDIR /app

# Copy binary from builder
COPY --from=builder /build/server .

# Copy public files
COPY public ./public

# Expose ports
EXPOSE 3000 5000 5001/udp

# Run
CMD ["./server"]
