# Control API (v4)

The audio service runs a small HTTP server on **port 8097**. It serves the web
control page at `/` and a JSON API under `/api`. The home-screen app and the web
page both drive the service through this API; nothing else talks to the
Bluetooth driver.

Every response carries `"api": 4`. The number is the API schema version; it
changes when the JSON shape or the endpoint set changes, independently of the
release version in `VERSION`. Clients require `api >= 3`, so v4 is compatible
with them.

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
The TV notice also names the console's address (`http://<PS4_IP>:8097`) when the
service can tell it, and the app shows that address as a QR code next to the code.

A LAN client pairs by posting the code and is handed a token:

```
POST /api/pair/<code>
X-BTA-Control: 1
--> 200 {"api":4,"paired":true,"token":"<32 hex chars>"}
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
`{"api":4,"paired":false,"lan":<0|1>}`, which tells the page whether to prompt
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
| `POST /api/scan` | Add Device: stop reconnecting, disconnect the device (or cancel its connection attempt), then search once the radio is free. Again while a search runs or waits: nothing; after it: one more search. |
| `POST /api/scan-done` | Leave Add Device: stop a search and reconnect the device Add Device paused. |
| `POST /api/select/<MAC>` | Choose a device and connect (ends Add Device). `<MAC>` is twelve hex digits, no separators. Any well-formed address is accepted, including one `devices[]` does not hold and one whose class of device does not advertise audio: it is taken at its word and dialled. Only a malformed address is a `400`. |
| `POST /api/connect` | Connect the selected device. |
| `POST /api/stop` | Disconnect and restore TV sound. |
| `POST /api/test` | Play a short test tone. Needs a streaming device (`409` otherwise); the link state is not re-read. |
| `POST /api/volume/<0-100>` | Set headphone volume. |
| `POST /api/pin/<digits>` | Submit a pairing PIN for a headset that asks for one. |
| `POST /api/tv-on`, `/api/tv-off` | Keep or mute TV sound while streaming. |
| `POST /api/joint-on`, `/api/joint-off` | Joint-stereo SBC on/off for this device. |
| `POST /api/latency/<0-2>` | Set the latency mode: 0 lowest, 1 balanced, 2 most stable. |
| `POST /api/lan-on`, `/api/lan-off` | Turn phone control on/off (console only). |
| `POST /api/quit` | Stop the service, disconnect the device and restore TV sound. |

A successful `POST` returns the same JSON as `GET /api/status`.

## Status fields

The important fields:

| Field | Meaning |
|---|---|
| `api` | API schema version (4). |
| `version` | Release version string. |
| `message` | Human-readable status line. |
| `link` | Truthful link state: 0 idle, 1 connecting, 2 playing, 3 lost, 4 failed, 5 searching. |
| `state` | Connection state-machine name: `idle`, `discovering`, `connecting`, `pairing`, `configuring`, `streaming`, `recovering`. |
| `attempts`, `retry_ms` | Connection attempt count and milliseconds until the next retry. |
| `streaming`, `wanted`, `scanning`, `pin_pending` | Service flags. `scanning` is also 1 while a requested search waits for the radio. |
| `adding` | 1 while Add Device pauses reconnecting (see below). |
| `latency_mode` | Buffering mode: 0 lowest, 1 balanced, 2 most stable. |
| `drops`, `underruns` | Packets dropped and buffer underruns while streaming (choppy-audio signals). |
| `volume`, `meter`, `level` | Headphone volume and output level. |
| `tv_enabled`, `hdmi_muted` | TV-sound preference and current mute state. |
| `name`, `selected`, `devices[]` | The chosen device and the listed devices. |
| `kernel` | One line describing what the service resolved in this kernel, e.g. `kernel: base ok, 6/6 signatures, fw 0x13520001, patch+devlist+widen`. This is the line to quote in a bug report; see [FIRMWARE.md](FIRMWARE.md). |
| `fw_version` | Firmware as `sceKernelGetSystemSwVersion` reports it, a number: `324141057` (`0x13520001`) on 13.52. 0 if unknown. |
| `kernel_caps` | Bitmask of what is available: 1 accept patch, 2 device list, 4 three-slot widening. |
| `kernel_sigs` | How many of the six code signatures resolved uniquely. |
| `lan` | 1 if phone control is on. |
| `ip` | The console's LAN address, e.g. `"192.168.1.97"`, while phone control is on. Absent when it is off or the address isn't known. |
| `pair_code` | The current six-digit code. **Only present for console-local callers**, never in a LAN response. |

Each entry in `devices[]` has `address`, `name`, `audio`, `known`, `found` and
`rssi`:

- `audio` is 1 when the device advertises audio playback, from its service bits or
  from its class of device alone. It is a **label, never a filter**: every device the
  service knows of is listed, because one with an odd class of device is exactly the
  one a user needs to see in order to try it.
- `known` is 1 if the device has played sound through the service before; the
  home-screen app lists only these on Home.
- `found` is 1 if it answered a search since Add Device was last opened (the app's Add
  Device lists only these), and `rssi` is its signal strength then, in dBm, or 0 when
  not found.
- `name` is empty when the service could not read the driver's device cache — on a
  firmware without the device-list capability the list comes from scan events, which
  carry no name. Clients then show the address.

Remaining fields (`patch_rc`, `packet_rc`, `connect_rc`, `bt_state`, `relinks`,
`resyncs`, timing counters, …) are diagnostics.

## Searching and connecting share the radio

A search started while the service is still trying to connect to a device that
doesn't answer leaves the PS4's Bluetooth unable to connect anything, the
controller included, until the console is fully powered off. So a search and a
connection attempt never run at the same time:

- `POST /api/scan` stops reconnecting. A playing device is disconnected (TV sound
  comes back) and the search starts once the driver reports the link down. A
  connection attempt is cancelled, and the search starts once the driver confirms
  it ended, or, without that confirmation, once the attempt's 30 s window is over.
  Until then `scanning` is 1.
- A device chosen during a search, or reconnected after it, is dialled once the
  search has stopped and the radio rested about 2 s; `link` shows 1 meanwhile.
- Add Device ends with `POST /api/scan-done`, `/api/select`, `/api/connect` or
  `/api/stop`; also when the console app stops asking for status for 10 s (it
  was closed, or a game took the screen), and, for a search started from a
  phone, a minute after that search ended. Ending it any way but choosing,
  connecting or disconnecting reconnects the device it paused.

`service.log` records each step: `add: ...` when Add Device starts and ends,
`radio: dial cancelled after N ms, the driver confirmed it`, `radio: waited N ms
for the dial to end (no confirmation)` or `radio: link down after N ms`, then
`scan: started`.

## Status codes

`200` ok · `400` bad request · `401` pairing required · `403` missing control
header, non-local host, or wrong/disabled pairing · `409` wrong state (e.g. a
test tone with no device connected) · `500` could not save · `503` could not start.
