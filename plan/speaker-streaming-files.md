# Speaker Streaming - File List

This document lists all files involved in the two-way audio (speaker streaming) feature.

## DevKit Firmware Files

| File | Purpose |
|------|---------|
| `apps/tuya_cloud/object_detection/src/speaker_streaming.c` | Main speaker streaming module - UDP receiver, jitter buffer, playback task, NAT hole punching |
| `apps/tuya_cloud/object_detection/src/speaker_streaming.h` | Header file - API declarations for speaker_streaming_init/stop/is_active/get_stats |
| `apps/tuya_cloud/object_detection/src/tuya_main.c` | Main app - calls `speaker_streaming_init()` during boot, handles "audio play" command from web |
| `apps/tuya_cloud/object_detection/src/cli_cmd.c` | CLI commands - includes "audio play" test command |
| `apps/tuya_cloud/object_detection/src/ai_audio_player.c` | Audio player module - plays audio through speaker using `ai_audio_player_start/stop/data_write` |
| `apps/tuya_cloud/object_detection/src/ai_audio_player.h` | Audio player API header |
| `apps/tuya_cloud/object_detection/src/ai_audio.c` | Low-level audio abstraction - `tdl_audio_play()` for raw PCM output |
| `apps/tuya_cloud/object_detection/src/ai_audio.h` | Audio abstraction header - defines `AUDIO_CODEC_NAME` |

## Web Application Files

| File | Purpose |
|------|---------|
| `webapp/main.go` | Go web server - WebRTC audio handling, Opus decode (48kHz), resample to 16kHz, UDP speaker sender, NAT address tracking |
| `webapp/public/index.html` | Web UI - "Start Talking" / "Stop" buttons, "PLAY AUDIO" button, JavaScript for WebRTC audio capture |
| `webapp/Dockerfile` | Docker config - exposes UDP port 5002 for speaker audio |
| `webapp/go.mod` | Go dependencies - includes `hraban/opus` for Opus codec |

## Web UI Commands (index.html)

| Button | Command Sent | DevKit Handler |
|--------|-------------|----------------|
| 🎙️ Start Talking | WebRTC audio stream | `speaker_streaming.c` via UDP |
| 🔊 PLAY AUDIO | `audio play` via TCP | `tuya_main.c` → `ai_audio_player_start()` |


## Key Functions by File

### speaker_streaming.c

| Function | Description |
|----------|-------------|
| `speaker_streaming_init()` | Initialize UDP socket, jitter buffer, start RX/playback/keepalive threads |
| `speaker_streaming_stop()` | Stop all threads, close socket, print stats |
| `speaker_rx_task()` | UDP receiver thread - writes packets to jitter buffer |
| `speaker_playback_task()` | Playback thread - reads from buffer, calls `tdl_audio_play()` |
| `speaker_keepalive_task()` | NAT hole punching - sends 0xFE ping every 5s to VPS |
| `jitter_buffer_write()` | Write data to ring buffer (producer) |
| `jitter_buffer_read()` | Read data from ring buffer (consumer) |

### main.go

| Function/Handler | Description |
|------------------|-------------|
| `startSpeakerUDPServer()` | Listens on UDP 5002 for DevKit pings, tracks NAT address |
| `sendSpeakerAudio()` | Sends PCM bytes to DevKit's NAT-mapped address |
| `peerConnection.OnTrack()` | Receives browser audio via WebRTC, decodes Opus at 48kHz, resamples to 16kHz, sends 640-byte UDP packets |

### index.html

| Function | Description |
|----------|-------------|
| `startTalking()` | Captures browser mic, connects WebRTC, sends audio |
| `stopTalking()` | Stops mic capture, closes WebRTC connection |
| `connectTalkWebRTC()` | Creates WebRTC peer connection with audio track |

## Audio Pipeline

```
Browser Mic (48kHz) 
    │
    ▼ WebRTC/Opus
VPS Server (main.go)
    │ Decode 48kHz → Resample to 16kHz → 640-byte packets
    ▼ UDP to port 5002
DevKit NAT Router
    │ NAT hole punched by DevKit pings
    ▼
DevKit (speaker_streaming.c)
    │ Jitter buffer (32KB, 100ms prefill)
    ▼
Speaker (tdl_audio_play)
```

## Configuration Constants

### DevKit (speaker_streaming.c)

| Constant | Value | Description |
|----------|-------|-------------|
| `SPEAKER_UDP_PORT` | 5002 | UDP port for speaker audio |
| `JITTER_BUFFER_SIZE` | 32000 | 1 second of audio buffer |
| `PREFILL_THRESHOLD` | 3200 | 100ms of audio before playback starts |
| `PLAYBACK_CHUNK_SIZE` | 640 | 20ms playback chunks |
| `NAT_KEEPALIVE_MS` | 5000 | Send ping every 5 seconds |
| `SPEAKER_PING_MARKER` | 0xFE | Ping packet identifier |

### Server (main.go)

| Constant | Value | Description |
|----------|-------|-------------|
| `speakerUDPPort` | 5002 | UDP port to listen for pings |
| `browserSampleRate` | 48000 | Browser's native Opus rate |
| `devkitSampleRate` | 16000 | DevKit's required rate |
| `resampleRatio` | 3 | 48000/16000 decimation factor |
| `targetPacketBytes` | 640 | 20ms of 16kHz audio |
