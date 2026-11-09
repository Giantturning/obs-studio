# OBS Matrix Studio — Windows x64 experimental fork

Native OBS Studio fork (not a plugin), based on libobs and the Qt frontend.

## What's implemented in this branch

- 8 RTMP(S) outputs by default; create/delete destinations dynamically (up to 64 configurations).
- 16 native OBS audio buses instead of the stock 6. The source routing matrix supports 16-bit routing masks (0–65535).
- Per-output Program video or separate OBS scene via OBS view rendering. Each output may use its own selected H.264 encoder, bitrate, audio bus, RTMP URL/key and backup server.
- Dynamic video encoder discovery: choose a listed H.264 encoder, including hardware encoders (NVENC, QSV or AMF) if OBS exposes them at runtime. Hardware combinations are not yet tested.
- Encoder sharing for matching Program-only outputs and matching bitrate/encoder ID (experimental).
- Backup RTMP URL/key and one automatic backup attempt after a primary failure or when native reconnect has stopped. No seamless switch or automatic failback yet.
- Named output groups with start/stop.
- Named presets for destination and source mixer routing. For safety, stream keys are not duplicated into presets.
- Per-output outgoing bitrate, reconnect status, frame drops, connection time, and live/backup state.
- 16 native audio peak meters and silence alarms for active outputs (threshold -55 dB, more than 10 seconds).
- Browser dashboard: http://127.0.0.1:4456/dashboard
- Local REST API on 127.0.0.1:4456 and read-only WebSocket event stream on 127.0.0.1:4457/events.
- API audit log for REST modifications, without request bodies or credentials.
- Windows DPAPI protects stream keys, backup keys, and the API bearer token in user settings. The Qt Matrix Control dock has Copy token / Show token buttons.

IMPORTANT: This is a development branch. All changes require a successful Windows x64 GitHub Actions build plus end-to-end live RTMP and audio/video routing tests. A successful build alone is not proof of production stability.

## REST API

All requests except GET /dashboard need the bearer Authorization header.
For POST/PATCH/PUT sends use JSON.

| Method | Endpoint | Function |
| --- | --- | --- |
| GET | /api/v1/status | All outputs, audio routing, meter levels, scenes, encoder choices |
| GET | /api/v1/health | Counts of streaming, backup, reconnecting, silent outputs |
| GET | /api/v1/outputs | Current destinations; secret keys omitted |
| POST | /api/v1/outputs | Create an RTMP destination |
| DELETE | /api/v1/outputs/{id} | Remove a stopped destination |
| PATCH | /api/v1/outputs/{id}/config | Configure a stopped output |
| POST | /api/v1/outputs/{id}/start | Start destination |
| POST | /api/v1/outputs/{id}/stop | Stop destination |
| GET | /api/v1/scenes | Selectable OBS scenes |
| GET | /api/v1/encoders | Available H.264 encoder IDs |
| GET | /api/v1/matrix | Audio sources and mixer bitmask |
| PUT | /api/v1/matrix/{source-uuid} | Set 16-bit mixer mask |
| GET | /api/v1/groups | Groups |
| POST | /api/v1/groups/{group}/start | Start group |
| POST | /api/v1/groups/{group}/stop | Stop group |
| GET | /api/v1/presets | List presets |
| POST | /api/v1/presets/save | Save a preset by name |
| POST | /api/v1/presets/load | Load preset while streams stopped |
| DELETE | /api/v1/presets/{name} | Delete preset |

PATCH /api/v1/outputs/rtmp-01/config example:

    {
      "name": "Deutsch",
      "server": "rtmps://server.example/live",
      "key": "PRIVATE_KEY",
      "audioMix": 8,
      "videoSourceUuid": "",
      "videoEncoderId": "obs_x264",
      "group": "language",
      "videoBitrate": 3500,
      "audioBitrate": 160,
      "backupServer": "rtmps://backup.example/live",
      "backupKey": "BACKUP_KEY",
      "failoverEnabled": true
    }

Routing mask examples: bus 1 = 1, bus 8 = 128, all 16 buses = 65535.
Video scene UUIDs come from GET /api/v1/scenes; empty string selects Program.
Configuration of a running output requires stop and reconnect.

WebSocket status: connect to ws://127.0.0.1:4457/events using the requested
subprotocol matrix.YOUR_API_TOKEN. It publishes one full status JSON about every second.
The local browser dashboard handles this automatically.

## Security and production limitations

The API accepts loopback connections only. Never expose either port directly
to the public internet. For remote use, put it behind a trusted tunnel. The
current HTTP/WebSocket servers are prototypes, not audited production transports.
Windows DPAPI binds encrypted secrets to the OS user; keep secure Windows
account access. The token is only shown/copyable from the local Matrix dock.

The initial 64-output ceiling is a configuration limit, NOT a guarantee that
a Windows PC can encode and upload 64 separate streams. Validate available
NVENC/AMF/QSV sessions, CPU, GPU and network bandwidth for each configuration.

The 16 buses are global audio mixers, not 16 fully independent audio
capture/processing engines. Per-scene audio isolation, separate per-output
audio effects/monitoring devices, perceptual loudness, multi-host failover,
automatic failback and unconditional zero-downtime output switching are still
unimplemented. Encoder sharing for nonidentical scene views is also not supported.

Preset load does not yet provide atomic rollback across all outputs.
The WebSocket is currently status-only; modification commands use REST.
The audit logger stores paths and timestamps, not secrets.

## Windows-only build & test

GitHub Actions workflow:
.github/workflows/matrix-studio-ci.yaml

Download successful artifacts at:
https://github.com/Giantturning/obs-studio/actions

Acceptance tests:
1. Build Windows x64 in GitHub Actions; ZIP artifact exists.
2. Start from extracted Windows build and check the Matrix Control dock.
3. Route distinct test tones to buses 1, 8 and 16, check isolation and silence flags.
4. Connect two RTMP outputs to independent scenes; confirm images differ.
5. Test primary/backup RTMP failure, output-group start/stop and preset persistence.
6. Validate WebSocket auth, REST API auth, DPAPI secrets, status and audit log.
7. Confirm CPU/GPU/network capacity for eight simultaneous live streams.
8. Test Windows shutdown while outputs are active; no crashes or handle leaks.
