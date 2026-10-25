# PS4 Bluetooth Audio

Homebrew for a jailbroken PS4 running GoldHEN. It sends game audio from the console's
own Bluetooth radio straight to Bluetooth Classic headphones, with no PC in the audio
path. TV sound can be muted or kept alongside. Output only: no microphone or chat
input.

**It installs and tries on any firmware and any model** (Base, Slim, Pro). It does not
check a version number and does not refuse. What it finds in your kernel decides how
much of it works, and it tells you in one line.

Audio is **confirmed to play** on the launch-model PS4 on firmware 13.520.001, with QCY
AilyBuds Pro, Baseus Bass BS1 NC and a Libratone ONE speaker. On another firmware or
model it may resolve nothing, connect but stay silent, or work: the kernel routines it
looks for were cut from 13.520.001, and elsewhere they are candidate patterns,
frequently right but not guaranteed. Nothing in system firmware is modified — the one
patch lives in RAM and a reboot removes it — so the worst case is a crash: reboot, and
delete the app if you want it gone.

Use it only on a console you own. [`docs/FIRMWARE.md`](docs/FIRMWARE.md) explains what
resolves where, and what to quote in a bug report.

## Phone control and security

The control server runs on **port 8097**. The home-screen app reaches it over the
console's own loopback address and always has full access. Any phone or PC on your
network is a LAN client and, **by default, can read nothing and change nothing**.

To control audio from your phone, turn **phone control** on at the console (or create
`/data/bta_audio/lan_on` over FTP). The service then shows a **six-digit pairing code**
on the TV, and the app shows a **QR code** for `http://<PS4_IP>:8097` beside it. Scan it
or open the address on your phone and enter the code once; the phone is remembered until
the service restarts or you turn phone control off. Connections from outside your local
network are always refused. Endpoints, the pairing flow and the status fields are in
[`docs/API.md`](docs/API.md).

## Known issues

- **A silent headphone usually recovers on its own**: the service tries the other SBC
  format and then re-establishes the link. If it still plays nothing, turn the
  headphones off and on, or press Disconnect and connect again.
- **Not every headset plays.** AirPods Pro (1st and 2nd generation) and the Xbox
  Wireless Headset connect, pair and accept every audio packet, but stay silent with
  either stereo format. What differs happens inside the console's Bluetooth driver, out
  of the service's view; finding it needs a recording of what the PS4 sends.
- **Brief glitches during game loads** are possible on some headphone models.
- **Controller drops.** An early build polled the Bluetooth driver while streaming and
  starved the wireless controller's link. The current worker does no driver polling
  while streaming, which fixed it on the test console. Test with your own controller.
- Longer numeric PINs (via the browser controls) are untested.

## What's here

| Path | Contents |
|---|---|
| `app/` | Payload source: launcher, worker (A2DP bridge and web UI), kernel resolver and accept patch, device cache |
| `app/kernel.h` | The one place firmware knowledge lives: the signature resolver, the capability rules and the 13.520.001 table |
| `native/` | Home-screen app source (OpenOrbis), asset generator and PC preview harness |
| `tools/` | Install and deploy scripts |
| `Makefile`, `docker/`, `scripts/` | Linux build, pinned Docker build image, helper scripts |
| `VERSION` | Release version used for file names and the PKG's `param.sfo` |

Prebuilt files are not kept in the repository. Download
`Bluetooth-Audio-<version>.pkg`, `BTA-Audio-<version>.bin` and `SHA256SUMS` from the
[Releases page](https://github.com/sse-97/ps4-bt-audio/releases), or build them
yourself (below). Every push also uploads them as a build artifact on the Actions tab.

## Install

You need a jailbroken PS4 running GoldHEN, with its FTP server enabled (port 2121).
Use either option below, or both.

### Option A: home-screen app (PKG)

`Bluetooth-Audio-3.0.0.pkg` installs a **Bluetooth Audio** app (title ID `BTAD00001`)
on the home screen. It bundles the audio payload: when you open the app and the service
isn't running, it sends the payload to GoldHEN's BinLoader (port 9090) for you. For the
service to also start after every jailbreak without opening the app, add the AutoRun
entry from Option B.

1. Copy the PKG over FTP to **`/data/pkg/Bluetooth-Audio-3.0.0.pkg`** (create the `pkg`
   folder if it doesn't exist).
2. In GoldHEN's settings menu, open Debug Settings, set the package source to **HDD**,
   then run **Package Installer** and install it. If an older version is installed,
   delete it from the home screen first (Options, Delete).
3. Turn GoldHEN's BinLoader on: **Enable PayLoader Server** (port 9090) in Servers
   Settings. If it is off, the app's Start dialog says so. It keeps listening after a
   payload, so the app can start the service again later in the same jailbreak;
   turning it off doesn't affect a running service.
4. Open **Bluetooth Audio**. It renders at 1080p like a PS4 Settings screen: your
   devices, then **Add Device**, **Volume**, **Keep TV Sound On**, **Audio Latency**,
   **Allow Phone Control** and **Stop Audio Service**. Confirm connects the focused
   device; on the connected one it opens the Options menu (**Test Sound**,
   **Disconnect**). Confirm and cancel follow the console's enter-button setting (Cross
   in most regions, Circle in Japan), and **Triangle** on Home opens **Kernel Info**:
   what the service found in your kernel, and what that means. It is a mini app like
   the system's own — it opens next to a running game and closes from the Quick Menu —
   and the service keeps running after you close it.
5. If the service doesn't start, check `/data/bta_audio/app.log`, or send the payload
   yourself with Option B. For connection problems, `/data/bta_audio/service.log` lists
   what the service did, step by step; its second line is the kernel report.

### Option B: payload only (no app)

Send it from your PC without touching the PS4's file system. **Never open a bare
connection to port 9090**: GoldHEN's PayLoader Server runs whatever arrives on a
connection as a payload inside a system process.

```powershell
python tools/deploy_variant.py BTA-Audio-3.0.0.bin --host <PS4_IP> --connect
```

It stops a running service first, then sends the payload in one connection, and
`--connect` presses Connect once the new service answers. `--wait <seconds>` is how
long to keep retrying a refused connect to port 9090 (default 30), for when GoldHEN
isn't loaded yet.

To start it automatically after each jailbreak, install it as a GoldHEN AutoRun
payload:

```powershell
python tools/install_bta.py --host <PS4_IP> --payload BTA-Audio-3.0.0.bin
python tools/install_bta.py --host <PS4_IP> --disable-autorun   # undo
```

It creates `/data/payloads/` and `/data/GoldHEN/` if they are missing, so a console
that has never run a payload installs cleanly, and backs up any remote file it replaces
into `install_backups/` on your PC.

### Where files end up on the PS4

| Path on the PS4 | What it is |
|---|---|
| `/data/pkg/Bluetooth-Audio-3.0.0.pkg` | PKG you copy there before installing (Option A) |
| `/data/payloads/BTA-Audio.bin` | Boot payload written by `install_bta.py` (Option B) |
| `/data/GoldHEN/payloads.ini` | GoldHEN AutoRun list. The installer adds `/user/data/payloads/BTA-Audio.bin = 1` under `[AutoRun]` and keeps your other entries |
| `/data/bta_audio/` | Runtime state, created automatically: `profiles.bin` (remembered headphones — the SBC format that last produced sound, counts, last-connected time; migrated once from the pre-2.8.0 `device.bin` and `dual-<address>` files), `volume.bin`, `latency.bin`, and `app-frame.ppm` when you press L3+R3 in the app |
| `/data/bta_audio/lan_on`, `pair_code.txt` | Create `lan_on` to allow phone control, delete it to lock the API to the console; `pair_code.txt` holds the current six-digit code while it is on |
| `/data/bta_audio/service.log` | The kernel report, status messages, connection steps, failed requests and an audio summary every 10 minutes, each stamped UTC. Each start, and every 256 KB, moves it to `service-previous.log` |
| `/data/bta_audio/app.log` | The TV app's log, UTC times. Past 64 KB it moves to `app-previous.log` |
| `/data/bta_audio/status.json` | The service's status, rewritten every 10 s (the fields of `GET /api/status`) |
| `/data/bta_audio/stop` | Create this empty file over FTP to request a clean shutdown |
| `/user/app/BTAD00001/` | Where the PKG installs the app (managed by the system) |

### Uninstall

1. **The app.** On the home screen, highlight **Bluetooth Audio**, press Options and
   choose **Delete**. Delete `/data/pkg/Bluetooth-Audio-3.0.0.pkg` if you left it
   there.
2. **The AutoRun entry.** `python tools/install_bta.py --host <PS4_IP>
   --disable-autorun` sets the entry to `0` and backs the file up; delete
   `/data/payloads/BTA-Audio.bin` over FTP to remove the boot payload itself.
3. **The settings.** Delete `/data/bta_audio/` over FTP. Optional — the folder is small
   and is reused if you reinstall.

The patch lives only in RAM, so a reboot (without re-running the jailbreak) already
removes it.

## Using it

Put your headphones in pairing mode, then scan and pick them from the app or from
`http://<PS4_IP>:8097/` in a browser on your network. **Disconnect headphones** restores
TV sound. The selected headphones are remembered across restarts, and AutoRun
reconnects them after the jailbreak, so switch them on first. To make the PS4 discover
headphones from scratch, remove them in **Settings → Devices → Bluetooth Devices**
(Options, Forget Device) first.

**Add Device** lists the devices that answer the search, strongest signal first; devices
you have used before stay on the main screen. It first lets go of the current device, so
one that is playing is disconnected (TV sound comes back while you search), and leaving
without choosing reconnects it. The search may take a moment to begin: a search must
never run while the PS4 is still trying to connect, because on 13.52 that combination
leaves the console's Bluetooth stuck, controller included, until a full power-off
([`docs/API.md`](docs/API.md) has the detail).

Devices are listed whether or not they advertise audio playback, and one that doesn't
is dialled anyway with a warning — under-reported profiles are no reason to stop you
trying. If the service could not read the driver's device cache on your firmware, the
list comes from scan events instead and devices show their MAC rather than a name.

## Latency

SBC over Bluetooth can never match a wired headset, and most of the delay is in the
headphones' own buffer, which no PS4 software can change. What the service controls is
how much audio it keeps buffered before the radio: it paces sends from a clock, one
~4 ms packet at a time, and trims the oldest few milliseconds if it runs ahead rather
than letting ~128 ms pile up. Pick a mode on the web page, or `POST /api/latency/<0-2>`:

| Mode | Service buffer | Trade-off |
|---|---|---|
| 0 Lowest | ~8 ms target (≤12 ms) | Least delay, least tolerance for Wi-Fi interference |
| 1 Balanced (default) | ~12 ms target (≤16 ms) | Good delay with some headroom |
| 2 Most stable | ~16 ms target (≤20 ms) | Rides out game loads and busy 2.4 GHz Wi-Fi |

The choice survives restarts, and `/api/status` and `/api/timing` report the counters
that show whether it is holding ([`docs/API.md`](docs/API.md)). How the real driver and
headphones behave is the one thing the host tests cannot check: what to try on a
console, and what to report, is in [`docs/TESTING.md`](docs/TESTING.md).

## Build from source

The reference build runs on Linux in Docker, the same way GitHub Actions builds every
push. With Docker installed (on Windows, from WSL):

```sh
scripts/docker-build.sh            # build/dist: payload, PKG, SHA256SUMS
scripts/docker-build.sh payload    # only build/BTA-Audio.bin
```

The image (`docker/Dockerfile`) pins Ubuntu 24.04 with clang/lld 18.1.3, the
[OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (v0.5.4),
OpenSSL 1.1 for PkgTool and exact Python package versions for the asset generator; with
that toolchain `make dist` reproduces **the payload** byte for byte. The PKG does not
reproduce: PkgTool writes per-build metadata, so two builds of identical source give
PKGs that differ in their header, entry and digest tables while the content itself is
identical (measured between two 3.0.0 builds: 1733 bytes, all inside the first 1.1 MB,
nothing beyond `0x110b60`). Every release publishes its `SHA256SUMS`, so use it to
verify a download against that release; only the payload line also reproduces from
source. Without Docker, run `make` directly: the
payload needs only `clang` and `ld.lld`, the PKG also `OO_PS4_TOOLCHAIN`, Python with
Pillow and matplotlib, and `libssl1.1`.

```sh
make payload      # build/BTA-Audio.bin
make pkg          # build/Bluetooth-Audio-<version>.pkg
make dist         # both, renamed, plus SHA256SUMS in build/dist
make test         # host unit tests (needs only a native cc)
make host-ui      # build/host_ui: renders a UI scene file to a PPM on the PC
```

To release, bump `VERSION` together with the version strings in `app/web.html`,
`app/worker.c` and `native/main.c` (CI fails if they disagree), then push a tag `v` +
`VERSION`. CI creates a draft GitHub Release with the PKG, the payload and
`SHA256SUMS`, which you review and publish.

## Credits and licenses

- Font assets: DejaVu, see `native/LICENSE_DEJAVU.txt`.
- AutoRun entry format: documented in the [orbisRPC installer](https://github.com/SirHumza/orbisRPC/tree/main/installer); no code from it is included.
- Requires [GoldHEN](https://github.com/GoldHEN/GoldHEN) (not included).
- The PKG contains OpenOrbis sample modules (`libc.prx`, `libSceFios2.prx`,
  `right.sprx`) from the
  [OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain). They are
  not covered by this project's license.

This project's own code is released under the [MIT License](LICENSE).
