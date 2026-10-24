# Control API (v3)

The audio service runs a small HTTP server on **port 8097**. It serves the web
control page at `/` and a JSON API under `/api`. The home-screen app and the web
page both drive the service through this API; nothing else talks to the
Bluetooth driver.

Every response carries `"api": 3`. The number is the API schema version; it
changes when the JSON shape or the endpoint set changes, independently of the
release version in `VERSION`.

## Who can reach it

| Client | How it connects | Access |
|---|---|---|
| The console app | Loopback (`127.0.0.1`) | Full, always. It is physically the owner's console. |
| A phone or PC on the LAN | The PS4's LAN IP | **None by default.** Must pair first (below). |

Connections from outside the local network are refused outright, and the `Host`
header must name a local address on port 8097.

## Phone control and pairing

LAN access is off until the owner turns **phone control** on at the console
(the app's Settings, or by creating the flag file
`/data/bta_audio/lan_on`). While it is on, the service shows a **six-digit
pairing code** on the TV and also writes it to `/data/bta_audio/pair_code.txt`.

A LAN client pairs by posting the code and is handed a token:

```
POST /api/pair/<code>
X-BTA-Control: 1
--> 200 {"api":3,"paired":true,"token":"<32 hex chars>"}
```

It then sends that token on every later request:

```
X-BTA-Token: <token>
```

- Tokens live in memory only: they are cleared when phone control is turned off
  and when the service restarts, so a phone re-pairs after a reboot.
- At most 4 LAN clients are paired at once; the oldest is dropped beyond that.
- After 10 wrong codes the service pauses pairing for 30 seconds, then allows it
  again — enough to make guessing a six-digit code hopeless without ever locking
  the owner out for good.
- Turning phone control **on or off is a console-only action** (`/api/lan-on`,
  `/api/lan-off` require loopback), so a paired phone cannot widen access or
  lock the owner out.

A LAN request without a valid token gets `401` and a JSON body
`{"api":3,"paired":false,"lan":<0|1>}`, which tells the page whether to prompt
for a code or ask the owner to enable phone control.

## Endpoints

All `POST` requests require the header `X-BTA-Control: 1` (this blocks silent
cross-site requests). `GET /` and `POST /api/pair/...` are the only paths a LAN
client may reach without a token.

| Method & path | Does |
|---|---|
| `GET /` | The web control page. |
| `GET /api/status` | Full state as JSON (see below). The home-screen app and the web page both read this. |
| `GET /api/timing` | Per-second send statistics for the last ~2 minutes. |
| `POST /api/pair/<code>` | Exchange the TV code for a token. |
| `POST /api/scan` | Start a Bluetooth scan (not while streaming). |
| `POST /api/select/<MAC>` | Choose a listed audio device and connect. |
| `POST /api/connect` | Connect the selected device. |
| `POST /api/stop` | Disconnect and restore TV sound. |
| `POST /api/test` | Play a short test tone. |
| `POST /api/volume/<0-100>` | Set headphone volume. |
| `POST /api/pin/<digits>` | Submit a pairing PIN for a headset that asks for one. |
| `POST /api/tv-on`, `/api/tv-off` | Keep or mute TV sound while streaming. |
| `POST /api/joint-on`, `/api/joint-off` | Joint-stereo SBC on/off for this device. |
| `POST /api/latency/<0-2>` | Latency buffer target: 0 lowest, 1 balanced, 2 most stable. |
| `POST /api/lan-on`, `/api/lan-off` | Turn phone control on/off (console only). |
| `POST /api/quit` | Stop the service and restore TV sound. |

A successful `POST` returns the same JSON as `GET /api/status`.

## Status fields

The important fields:

| Field | Meaning |
|---|---|
| `api` | API schema version (3). |
| `version` | Release version string. |
| `message` | Human-readable status line. |
| `link` | Truthful link state: 0 idle, 1 connecting, 2 playing, 3 lost, 4 failed, 5 searching. |
| `state` | Granular connection phase as a string (`idle`, `discovering`, `connecting`, `pairing`, `configuring`, `streaming`, `recovering`). |
| `streaming`, `wanted`, `scanning`, `pin_pending` | Service flags. |
| `volume`, `meter`, `level` | Headphone volume and output level. |
| `latency_mode` | Latency buffer target: 0 lowest, 1 balanced, 2 most stable. |
| `tv_enabled`, `hdmi_muted` | TV-sound preference and current mute state. |
| `name`, `selected`, `devices[]` | The chosen device and the listed audio devices. |
| `lan` | 1 if phone control is on. |
| `pair_code` | The current six-digit code. **Only present for console-local callers**, never in a LAN response. |

Remaining fields (`drops`, `underruns`, `patch_rc`, `bt_state`, `relinks`,
`resyncs`, timing counters, …) are diagnostics.

## Status codes

`200` ok · `400` bad request · `401` pairing required · `403` missing control
header, non-local host, or wrong/disabled pairing · `409` wrong state (e.g.
scanning while streaming) · `500` could not save · `503` could not start.
