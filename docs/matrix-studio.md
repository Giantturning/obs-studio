# OBS Matrix Studio — native fork MVP

This feature is built into the OBS Studio frontend source tree, **not a loadable plugin**.
It is an early prototype, not a verified production-ready broadcast system.

## Scope

- Four independently managed RTMP/RTMPS outputs with a shared OBS program video.
- Each destination selects one of OBS's existing six audio mixers.
- Six-track source routing matrix using OBS's native \`obs_source_set_audio_mixers\`.
- Built-in Qt dock with per-output server, key, track, start/stop, and status.
- Read-only status and writable control HTTP API on **127.0.0.1:4456**.
- Per-user settings stored by QSettings (Giantturning / OBSMatrixStudio).

The RTMP output encoder currently uses **x264 software encoding** for each destination.
Four concurrent encoders can require substantial CPU. Hardware encoders, encoder sharing,
per-output scene/video routing, resilient state management, and enhanced credential
storage are planned improvements, not implemented features.

## Configuration and secrets

The API binds only to loopback. At first launch it generates a long random bearer token.
The token is found in the \`apiToken\` field of the per-user OBSMatrixStudio.ini
configuration file. Do not commit or share the INI file: **stream keys and the API token
are stored there in plaintext**. Use a protected OS user account and appropriate file
permissions. API output responses redact stream keys.

For remote network access, use an authenticated local tunnel. **Do not expose port 4456
directly to the internet.** The HTTP parser is intentionally small and should be replaced
with a more hardened implementation before production or untrusted-network use.

## API

All requests require \`Authorization: Bearer <apiToken>\` and JSON request bodies for writes.

| Method | Path | Behavior |
| --- | --- | --- |
| GET | /api/v1/status | Main stream status, outputs, audio sources |
| GET | /api/v1/outputs | RTMP states and destinations (no keys) |
| GET | /api/v1/matrix | Audio source UUIDs and 6-bit routing masks |
| PATCH | /api/v1/outputs/rtmp-01/config | Set name, server, key, audioMix (1–6), videoBitrate, audioBitrate |
| POST | /api/v1/outputs/rtmp-01/start | Start one output |
| POST | /api/v1/outputs/rtmp-01/stop | Stop one output |
| PUT | /api/v1/matrix/{source-uuid} | Set \`{"mixers": 5}\` to route one source to tracks 1 and 3 |

Example (set token from the local config file, do not paste secrets into tickets):

\`\`\`sh
curl -H "Authorization: Bearer $OBS_MATRIX_TOKEN" \
  http://127.0.0.1:4456/api/v1/status
\`\`\`

\`\`\`sh
curl -X PATCH \
  -H "Authorization: Bearer $OBS_MATRIX_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"server":"rtmps://example.invalid/live","key":"YOUR_KEY","audioMix":2}' \
  http://127.0.0.1:4456/api/v1/outputs/rtmp-01/config
\`\`\`

Note: restarting an RTMP connection is required for URL/key changes on a live output.
Route changes use OBS's live source mixer flags. These mixer flags belong to the OBS
session and can also be affected by the OBS Advanced Audio Properties UI.

## Development and validation

This branch modifies:
- \`frontend/matrix/MatrixController.hpp/.cpp\`
- \`frontend/matrix/MatrixDock.hpp/.cpp\`
- \`frontend/widgets/OBSBasic.cpp\`
- \`frontend/cmake/ui-widgets.cmake\`

Build and test with the repository's existing CMake presets and platform dependencies.
The branch has not yet passed a full OBS build or functional broadcast test.
Test on a non-production installation and an RTMP endpoint under your control.

## Next milestones

1. Verify Windows/Linux/macOS builds and add tests for start/stop, audio routing, and HTTP validation.
2. Harden HTTP parsing, authentication, and key storage (OS credential vault).
3. Improve output lifecycle handling, reconnection events, per-target telemetry, and saved source routing.
4. Support shared/hardware encoding and dynamically configurable output counts.
5. Add independent video buses and per-output scene routing if required.
