# PS4 Bluetooth Audio (experimental)

Homebrew for a **jailbroken PS4 on firmware 13.520.001**. It sends game audio from the
console's own Bluetooth radio straight to Bluetooth Classic headphones, with no PC in the
audio path. TV sound can be muted or kept alongside.

Tested and confirmed audible on QCY AilyBuds Pro and Baseus Bass BS1 NC. Other headphones may or may
not work. Output only: no microphone or chat input.

> **Status: experimental, one console, one firmware.** Use it at your own risk and only on a
> console you own. Other firmware versions are unsupported: the payload checks kernel
> instruction fingerprints before patching and refuses to run if they don't match.

## What's new in 2.8.0

2.8.0 is a ground-up rework of the 2.7.5 prototype: the same audio path, rebuilt for
stability, security and ease of use, with a reproducible Linux build and host tests.

- **Reliable, self-healing connections.** The link runs through named states (discovering,
  pairing, connecting, configuring, streaming, recovering), each with its own timeout.
  Reconnects back off 2, 4, 8 then 16 seconds and then stop and ask you to power-cycle the
  headphones. A headset that connects but stays silent auto-switches SBC format and, if that
  fails, re-establishes the link — the step that used to need a manual reconnect. After a
  rest-mode wake the service re-opens the driver and reconnects on its own.
- **Remembered devices.** The service keeps a profile per headphone — the SBC format that
  last produced sound and when it last connected — and remembers several. On startup it
  reconnects to whichever remembered device is turned on, in the format that worked last.
- **Bounded latency with modes.** Audio is paced from a clock, one ~4 ms packet at a time,
  holding a shallow buffer (12–20 ms) instead of letting up to ~128 ms pile up. Pick Lowest,
  Balanced or Most stable on the web page or the app.
- **Secured control API with phone control.** The console app always has full access over
  loopback; any phone or PC on the LAN can read and change **nothing** until you turn phone
  control on at the console and enter the six-digit code shown on the TV. See
  [`docs/API.md`](docs/API.md).
- **Redesigned 1080p TV app** that looks and works like the PS4's own Settings screens: one
  list, an Options menu for the connected device, and confirm/cancel buttons that follow
  the console's enter-button setting. It talks to the service
  only through the JSON control API.
- **Rebuilt phone companion page** — mobile-first, accessible, one contextual primary action
  — served from the console on port 8097.
- **Joint stereo by default** (the format headphones agree to at connect), with a per-device
  toggle for the few models that need dual channel.
- **Reproducible Linux build and CI.** The payload and PKG build on Linux in a pinned Docker
  image, the same way GitHub Actions builds every push; host unit tests and UI scene renders
  run in CI, and tagged builds publish a GitHub Release with `SHA256SUMS`. No binaries are
  kept in the repository.

## Phone control and security

The control server runs on **port 8097**. The home-screen app reaches it over the console's
own loopback address and always has full access. Any phone or PC on your network is a LAN
client and, **by default, can read nothing and change nothing**.

To control audio from your phone, turn **phone control** on at the console (or create the
file `/data/bta_audio/lan_on` over FTP). The service then shows a **six-digit pairing code**
on the TV (also written to `/data/bta_audio/pair_code.txt`), and while phone control is on
the app shows a **QR code** for `http://<PS4_IP>:8097`, the address and the code on the left
of its screen. Scan the QR code or open the address on your phone and enter the code once;
the phone is remembered until the service restarts or you turn phone control off.
Connections from outside your local network are always refused.

The full JSON API — endpoints, the pairing flow and the status fields — is documented in
[`docs/API.md`](docs/API.md).

## Known issues

- **A silent headphone usually recovers on its own** now: the service tries the other SBC
  format and then re-establishes the link. If it still plays nothing after that, turn the
  headphones off and on, or press Disconnect and connect again.
- **Brief glitches during game loads** are possible on some headphone models.
- **Controller drops.** An early build polled the Bluetooth driver while streaming and
  starved the wireless controller's link. The current worker does no driver polling while
  streaming, which fixed it on the test console. Test with your own controller.
- A full reboot plus AutoRun cycle and rest-mode wake have had limited testing.
- Longer numeric PINs (via the browser controls) are untested.
- The patch lives only in RAM and disappears at reboot. Nothing persistent in system
  firmware is modified.

## What's here

| Path | Contents |
|---|---|
| `app/` | Payload source: launcher, worker (A2DP bridge and web UI), device cache |
| `native/` | Home-screen app source (OpenOrbis), asset generator and PC preview harness |
| `probe/bt_accept_patch.c` | In-RAM Bluetooth acceptance patch |
| `tools/` | Install and deploy scripts |
| `Makefile`, `docker/`, `scripts/` | Linux build, pinned Docker build image, helper scripts |
| `VERSION` | Release version used for file names and the PKG's `param.sfo` |

Prebuilt files are not kept in the repository. Download `Bluetooth-Audio-<version>.pkg`,
`BTA-Audio-<version>.bin` and `SHA256SUMS` from the
[Releases page](https://github.com/sse-97/ps4-bt-audio/releases), or build them yourself (see
below). Every push also uploads them as a build artifact on the Actions tab.

## Install

You need a jailbroken PS4 on firmware 13.520.001 running GoldHEN, with its FTP server
enabled (port 2121) so you can copy files. Use either option below, or both.

### Option A: home-screen app (PKG)

`Bluetooth-Audio-2.8.0.pkg` from the Releases page installs a **Bluetooth Audio** app (title ID `BTAD00001`) on the
home screen. It bundles the audio payload: when you open the app and the service isn't
running, it sends the payload to GoldHEN's BinLoader (port 9090) for you. For the service to
also start after every jailbreak without opening the app, add the AutoRun entry from Option B.

1. Copy the PKG over FTP to **`/data/pkg/Bluetooth-Audio-2.8.0.pkg`** on the PS4 (create the
   `pkg` folder if it doesn't exist).
2. In GoldHEN's settings menu, open Debug Settings, set the package source to **HDD**, then
   run **Package Installer** and install it. If an older version is installed, delete it from
   the home screen first (Options, Delete).
3. Make sure GoldHEN's BinLoader is on: **Enable PayLoader Server** (port 9090) in GoldHEN's
   Servers Settings. If it is off, the app's Start dialog says so. The PayLoader Server keeps
   listening after a payload, so the app can start the service again later in the same
   jailbreak; a second copy sent while the service runs only reports that it is already
   running. Turning the PayLoader Server off doesn't affect a running service. Then open
   **Bluetooth Audio** from the home screen. The app renders at 1080p like a PS4 Settings screen: the devices
   that have played sound through it before (others nearby are listed under **Add
   Device**), then **Add Device**, **Volume** (left/right to change it), **Keep TV Sound On**,
   **Audio Latency**, **Allow Phone Control** and **Stop Audio Service**. The confirm
   button connects the focused device; on the connected one it opens the Options menu
   (**Test Sound**, **Disconnect**), as does the Options button. The cancel button goes
   back from sub-screens and menus; on the main screen it reminds you that the PS button
   leaves the app. Confirm and cancel follow the console's enter-button setting (Cross in
   most regions, Circle in Japan). The app is a mini app like the system's own: it opens
   next to a running game without closing it, stays open when you leave it with the PS
   button, and closes from the Quick Menu (**Close Application**).
   If the service isn't running, the app offers to start it. The audio service keeps
   running after you close the app; **Stop Audio Service** ends it and disconnects the
   device.
4. If the service still doesn't start, check `/data/bta_audio/app.log` on the console, or send the
   payload yourself with Option B below. For connection problems, `/data/bta_audio/service.log`
   lists what the service did, step by step.

### Option B: payload only (no app)

Send it from your PC without touching the PS4's file system. **Never open a bare connection
to port 9090**: GoldHEN's PayLoader Server runs whatever arrives on a connection as a
payload inside a system process.

```powershell
python tools/deploy_variant.py BTA-Audio-2.8.0.bin --host <PS4_IP> --connect
```

To start it automatically after each jailbreak, install it as a GoldHEN AutoRun payload:

```powershell
python tools/install_bta.py --host <PS4_IP> --payload BTA-Audio-2.8.0.bin
python tools/install_bta.py --host <PS4_IP> --disable-autorun   # undo
```

### Where files end up on the PS4

| Path on the PS4 | What it is |
|---|---|
| `/data/pkg/Bluetooth-Audio-2.8.0.pkg` | PKG you copy there before installing (Option A) |
| `/data/payloads/BTA-Audio.bin` | Boot payload written by `install_bta.py` (Option B) |
| `/data/GoldHEN/payloads.ini` | GoldHEN AutoRun list. The installer adds `/user/data/payloads/BTA-Audio.bin = 1` under `[AutoRun]` and keeps your other entries |
| `/data/bta_audio/` | Runtime state folder, created automatically: remembered headphones, volume, latency mode, `status.json` and logs |
| `/data/bta_audio/profiles.bin` | Remembered headphones: the SBC format that last produced sound, success/failure counts and last-connected time. Migrated once from the pre-2.8.0 `device.bin` and `dual-<address>` files, which are no longer written |
| `/data/bta_audio/volume.bin` | Saved headphone volume (0–100) |
| `/data/bta_audio/latency.bin` | Saved latency mode (0 lowest, 1 balanced, 2 most stable) |
| `/data/bta_audio/lan_on` | Create this empty file to allow phone control; delete it to lock the API to the console |
| `/data/bta_audio/pair_code.txt` | The current six-digit pairing code, written while phone control is on |
| `/data/bta_audio/service.log` | The service's log: status messages, connection steps, failed requests and an audio summary every 10 minutes, each stamped with the UTC time. Each start, and every 256 KB, moves it to `service-previous.log` |
| `/data/bta_audio/app.log` | The TV app's log, UTC times. Past 64 KB it moves to `app-previous.log` |
| `/data/bta_audio/status.json` | The service's status, rewritten every 10 s (the same fields as `GET /api/status`) |
| `/data/bta_audio/app-frame.ppm` | A screenshot of the app, saved when you press L3+R3 together |
| `/data/bta_audio/stop` | Create this empty file over FTP to request a clean shutdown |
| `/user/app/BTAD00001/` | Where the PKG installs the app (managed by the system) |

`install_bta.py` backs up any file it replaces into an `install_backups/` folder on your PC.

### Uninstall

Deleting the app from the home screen leaves the AutoRun entry and the saved settings behind,
so remove them too for a clean uninstall:

1. **Remove the app (Option A).** On the home screen, highlight **Bluetooth Audio**, press
   Options and choose **Delete**. Delete `/data/pkg/Bluetooth-Audio-2.8.0.pkg` over FTP if you
   left the PKG there.
2. **Remove the AutoRun entry (Option B).** Run
   `python tools/install_bta.py --host <PS4_IP> --disable-autorun`. This sets the entry to `0`
   in `/data/GoldHEN/payloads.ini` and backs the file up; delete `/data/payloads/BTA-Audio.bin`
   over FTP to remove the boot payload itself.
3. **Remove the settings.** Delete the `/data/bta_audio/` folder over FTP to clear remembered
   headphones, volume, latency mode, logs and any phone-control flag. This is optional — the
   folder is small and is reused if you reinstall.

The acceptance patch lives only in RAM, so a reboot (without re-running the jailbreak) already
removes it; nothing in system firmware is modified.

### Using it

Put your headphones in pairing mode, then scan and pick them from the app or from
`http://<PS4_IP>:8097/` in a browser on your network. **Disconnect headphones** restores TV
sound. The selected headphones are remembered across restarts, and AutoRun reconnects them
after the jailbreak, so switch them on first.

**Add Device** lists the devices that answer the search, strongest signal first, so put
the new headphones in pairing mode before opening it; devices you have used before stay
on the main screen. It first lets go of the current device: it stops trying to reconnect, and a
device that is playing is disconnected (TV sound comes back while you search). Leaving Add
Device without choosing reconnects it. The search may take a moment to begin, because a
search must never run while the PS4 is still trying to connect to a device. On 13.52
that combination leaves the console's Bluetooth stuck, controller included, until a
full power-off. The service waits for the driver to confirm the attempt ended, or
for its 30 s window to pass (see [docs/API.md](docs/API.md)).

To make the PS4 discover headphones from scratch, remove them in **Settings → Devices →
Bluetooth Devices** (Options, Forget Device), then put them in pairing mode and scan.

**Not every headset plays.** So far a Libratone ONE speaker plays. AirPods Pro (1st and
2nd generation) and the Xbox Wireless Headset connect, pair and accept every audio
packet, but stay silent with either stereo format. What differs happens inside the
console's Bluetooth driver, out of the service's view; finding it needs a recording of
what the PS4 sends.

## Latency and audio pacing

SBC over Bluetooth can never match a wired headset, and most of the delay comes
from the headphones' own buffer, which no PS4 software can change. What the
service controls is how much audio it keeps buffered before the radio. At the
start of playback it briefly prebuffers a small cushion — the latency target —
so ordinary scheduling jitter has something to draw down and does not cause
dropouts. It then paces sends from a clock, one ~4 ms packet at a time, so the
console-side buffer is fed at the playout rate rather than in bursts, and it
holds the cushion shallow by dropping the oldest few milliseconds if it ever
runs ahead, instead of letting up to ~128 ms pile up before a single large
resync. The audio handed to the radio stays at or below the latency target plus
one packet (12–20 ms).

Pick a **latency mode** on the web page, or `POST /api/latency/<0-2>`:

| Mode | Service buffer | Trade-off |
|---|---|---|
| 0 Lowest | ~8 ms target (≤12 ms) | Least delay, least tolerance for Wi-Fi interference |
| 1 Balanced (default) | ~12 ms target (≤16 ms) | Good delay with some headroom |
| 2 Most stable | ~16 ms target (≤20 ms) | Rides out game loads and busy 2.4 GHz Wi-Fi |

The choice is saved in `/data/bta_audio/latency.bin` and survives restarts.
`GET /api/status` reports `latency_mode`, and the running `drops` (small backlog
trims) and `underruns` (slots with no audio ready) counters; `GET /api/timing`
still reports `maxback_ms` and the late-packet counts per second.

The pacing logic is covered off-console by a producer/consumer simulation in
`tests/test_pacing.c` (run with `make test`), which proves the audio handed to the driver is
never more than one target-plus-a-packet old, even across a stall. How the real Bluetooth
driver and headphones behave is the one thing the host tests cannot check: the full set of
console checks for every feature is in
[`docs/ON_CONSOLE_TESTING.md`](docs/ON_CONSOLE_TESTING.md).

## Build from source

The reference build runs on Linux in Docker, the same way GitHub Actions builds every push.
With Docker installed (on Windows, from WSL):

```sh
scripts/docker-build.sh            # build/dist: BTA-Audio-<version>.bin, Bluetooth-Audio-<version>.pkg, SHA256SUMS
scripts/docker-build.sh payload    # only build/BTA-Audio.bin
```

The image (`docker/Dockerfile`) pins Ubuntu 24.04 with clang/lld 18.1.3, the
[OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (v0.5.4), OpenSSL 1.1
for PkgTool, and exact Python package versions for the asset generator. The build is
deterministic: with that pinned toolchain `make dist` reproduces the payload and PKG byte for
byte, and every release publishes the resulting `SHA256SUMS` so you can verify a download
against the build (each push also prints the checksums in the Actions log).

Without Docker, run `make` directly. The payload needs only `clang` and `ld.lld`; the PKG also
needs `OO_PS4_TOOLCHAIN` pointing at the OpenOrbis toolchain folder, Python with Pillow and
matplotlib, and OpenSSL 1.1 (`libssl1.1`, needed by the runtime bundled with PkgTool):

```sh
make payload      # build/BTA-Audio.bin
make pkg          # build/Bluetooth-Audio-<version>.pkg
make dist         # both, renamed, plus SHA256SUMS in build/dist
make host-ui      # build/host_ui: renders a UI scene file to a PPM on the PC
```

### Releasing

Bump `VERSION` together with the version strings in `app/web.html`, `app/worker.c` and
`native/main.c` (CI fails if they disagree), then push a tag such as `v2.8.1` (the tag must be
`v` + `VERSION`). CI builds it and creates a draft GitHub Release with the PKG, the payload and
`SHA256SUMS`, which you review and publish.

### Windows (legacy)

`build_app.ps1` and `build_native.ps1` still work with a `toolchain` folder beside the repo
(`..\toolchain\llvm\bin` and `..\toolchain\OpenOrbis\PS4Toolchain`) and write to `build/`.
The Docker build is the reference; use it for releases.

The worker variants that used to live in `variants/src/` are in the git history at commit
`628163a`.

## Credits and licenses

- Font assets: DejaVu, see `native/LICENSE_DEJAVU.txt`.
- AutoRun entry format: documented in the [orbisRPC installer](https://github.com/SirHumza/orbisRPC/tree/main/installer); no code from it is included.
- Requires [GoldHEN](https://github.com/GoldHEN/GoldHEN) (not included).
- The PKG contains OpenOrbis sample modules (`libc.prx`, `libSceFios2.prx`, `right.sprx`)
  from the [OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain). They are
  not covered by this project's license.

This project's own code is released under the [MIT License](LICENSE).
