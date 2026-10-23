# PS4 Bluetooth Audio (experimental)

Homebrew for a **jailbroken PS4 on firmware 13.520.001**. It sends game audio from the
console's own Bluetooth radio straight to Bluetooth Classic headphones, with no PC in the
audio path. TV sound can be muted or kept alongside.

Tested and confirmed audible on QCY AilyBuds Pro and Baseus Bass BS1 NC. Other headphones may or may
not work. Output only: no microphone or chat input.

> **Status: experimental, one console, one firmware.** Use it at your own risk and only on a
> console you own. Other firmware versions are unsupported: the payload checks kernel
> instruction fingerprints before patching and refuses to run if they don't match.

## What's new in 2.7.5

- **Joint stereo by default.** Audio is sent as standard joint-stereo SBC, the format
  headphones agree to when they connect. The old format (dual channel) made the PS4
  renegotiate the stream with the headphones, and some models stayed connected but silent.
  You can turn joint stereo off per headphone on the web page if a model needs the old format.
- **Automatic recovery.** If the PS4 refuses audio for 4 seconds, the service drops the
  Bluetooth link and reconnects by itself (up to 3 times in a row).
- **Disconnect really disconnects.** The Disconnect button (web page, or Triangle in the
  app) drops the Bluetooth link itself, so the next connect starts fresh.
- **Steadier sending.** Audio goes out from its own thread with the same scheduling the
  PS4's audio service uses for its Bluetooth senders (real-time priority, cores 6–7).
- **Test tone follows the volume slider.**
- **Timing diagnostics.** `GET /api/timing` returns per-second packet timing (sent, refused,
  late, longest gap) for the last two minutes.

## Security note

The control page (port 8097) has no login. Anyone on your local network can scan, change
volume, connect or stop the service. Use it on a trusted home network only.

## Known issues

- **Some headphones need a reconnect or two.** If a pair connects but plays nothing, press
  Disconnect and connect again, or turn the headphones off and on. The automatic recovery
  handles most of these cases by itself.
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

`Bluetooth-Audio-2.7.5.pkg` from the Releases page installs a **Bluetooth Audio** app (title ID `BTAD00001`) on the
home screen. It bundles the audio payload: when you open the app and the service isn't
running, it sends the payload to GoldHEN's BinLoader (port 9090) for you. For the service to
also start after every jailbreak without opening the app, add the AutoRun entry from Option B.

1. Copy the PKG over FTP to **`/data/pkg/Bluetooth-Audio-2.7.5.pkg`** on the PS4 (create the
   `pkg` folder if it doesn't exist).
2. In GoldHEN's settings menu, open Debug Settings, set the package source to **HDD**, then
   run **Package Installer** and install it. If an older version is installed, delete it from
   the home screen first (Options, Delete).
3. Make sure GoldHEN's **BinLoader (port 9090)** is enabled. It accepts one payload per
   jailbreak (any connection uses it up), so either let the app start the service **or**
   send it from your PC with Option B, not both. Then open **Bluetooth Audio** from
   the home screen. Triangle scans (or disconnects the headphones while connected), the d-pad selects, X connects (or starts the service if it
   is offline), Square toggles TV sound, Circle closes the app. The audio service keeps running.
4. If the service still doesn't start, check `/data/bta_audio/app.log` on the console, or send the
   payload yourself with Option B below.

### Option B: payload only (no app)

Send it from your PC without touching the PS4's file system. **Never open a bare connection
to port 9090**: the loader treats any connection as a payload and stops listening until you
reboot and re-jailbreak.

```powershell
python tools/deploy_variant.py BTA-Audio-2.7.5.bin --host <PS4_IP> --connect
```

To start it automatically after each jailbreak, install it as a GoldHEN AutoRun payload:

```powershell
python tools/install_bta.py --host <PS4_IP> --payload BTA-Audio-2.7.5.bin
python tools/install_bta.py --host <PS4_IP> --disable-autorun   # undo
```

### Where files end up on the PS4

| Path on the PS4 | What it is |
|---|---|
| `/data/pkg/Bluetooth-Audio-2.7.5.pkg` | PKG you copy there before installing (Option A) |
| `/data/payloads/BTA-Audio.bin` | Boot payload written by `install_bta.py` (Option B) |
| `/data/GoldHEN/payloads.ini` | GoldHEN AutoRun list. The installer adds `/user/data/payloads/BTA-Audio.bin = 1` under `[AutoRun]` and keeps your other entries |
| `/data/bta_audio/` | Runtime state: saved headphones (`device.bin`), volume, `status.json`, logs |
| `/data/bta_audio/dual-<address>` | Present only if you turned joint stereo off for that headphone |
| `/data/bta_audio/stop` | Create this empty file over FTP to request a clean shutdown |
| `/user/app/BTAD00001/` | Where the PKG installs the app (managed by the system) |

`install_bta.py` backs up any file it replaces into an `install_backups/` folder on your PC.

### Using it

Put your headphones in pairing mode, then scan and pick them from the app or from
`http://<PS4_IP>:8097/` in a browser on your network. **Disconnect headphones** restores TV
sound. The selected headphones are remembered across restarts, and AutoRun reconnects them
after the jailbreak, so switch them on first.

To make the PS4 discover headphones from scratch, remove them in **Settings → Devices →
Bluetooth Devices** (Options, Forget Device), then put them in pairing mode and scan.

## Build from source

The reference build runs on Linux in Docker, the same way GitHub Actions builds every push.
With Docker installed (on Windows, from WSL):

```sh
scripts/docker-build.sh            # build/dist: BTA-Audio-<version>.bin, Bluetooth-Audio-<version>.pkg, SHA256SUMS
scripts/docker-build.sh payload    # only build/BTA-Audio.bin
```

The image (`docker/Dockerfile`) pins Ubuntu 24.04 with clang/lld 18.1.3, the
[OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (v0.5.4), OpenSSL 1.1
for PkgTool, and exact Python package versions for the asset generator. With that compiler the
2.7.5 payload rebuilds byte for byte (sha256 `7db52b84…b337b3`).

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
`native/main.c` (CI fails if they disagree), then push a tag such as `v2.7.6`. CI builds it and
creates a draft GitHub Release with the PKG, the payload and `SHA256SUMS`.

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
