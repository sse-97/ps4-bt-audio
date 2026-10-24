# On-console test checklist

Everything that can be verified without a PS4 is covered by the host unit tests (`make test`,
7500+ checks) and the CI build (payload, PKG and UI-scene renders). This document is the one
thing those cannot check: how the build behaves on real hardware. Run it once, start to
finish, on the target console after installing a release build.

- **Scope:** base PS4 (CUH-1000/1100/1200) on firmware **13.520.001** with GoldHEN, English
  system language. Slim/Pro and other firmwares are out of scope for this release.
- **What you need:** the release PKG and payload (`Bluetooth-Audio-2.8.0.pkg`,
  `BTA-Audio-2.8.0.bin`) from the draft GitHub Release, two Bluetooth headphones (ideally one
  that prefers joint stereo and one that needs dual channel), two DualShock 4 controllers, and
  FTP access (port 2121) to the console.
- **How to read results:** the web page at `http://<PS4_IP>:8097/` → **Diagnostics**, and the
  raw `GET /api/status` and `GET /api/timing` JSON, show the counters named below. The app
  also writes `/data/bta_audio/app.log` and the service writes `status.json`.

Tick each box. If anything fails, note the `message`, `state` and the relevant counters.

## 1. Install and first launch

- [ ] **PKG install (Option A).** Copy the PKG to `/data/pkg/`, install it from GoldHEN's
      Package Installer (package source HDD). A **Bluetooth Audio** icon appears on the home
      screen. Opening it shows the app at **1080p**.
- [ ] **Service autostart from the app.** With the service not already running, opening the app
      sends the bundled payload to BinLoader (port 9090) once and the service comes up
      (`status.json` appears, the web page answers). `app.log` shows `payload sent to
      BinLoader`.
- [ ] **Payload only (Option B).** On a fresh jailbreak (BinLoader unused), run
      `tools/deploy_variant.py BTA-Audio-2.8.0.bin --host <PS4_IP> --connect`. The service
      reports `version 2.8.0`.
- [ ] **AutoRun install.** `tools/install_bta.py --host <PS4_IP> --payload BTA-Audio-2.8.0.bin`
      adds the entry; after a reboot + jailbreak the service starts on its own and a
      notification says it is reconnecting (if a device is remembered) or idle.
- [ ] **Uninstall.** `--disable-autorun` stops the service starting at boot; deleting the app
      and the `/data/bta_audio/` folder leaves nothing behind (see the README Uninstall steps).

## 2. First connection (Add Device)

- [ ] **Add flow.** The Home list's **Add Device** opens **Searching...** with a live scan
      listing audio devices newest-first; Triangle searches again.
- [ ] **Home lists your devices only.** Before the first connection Home shows no devices
      (nearby TVs and car stereos appear only under **Add Device**); once a device has
      played sound, it stays on Home.
- [ ] **Connect.** Selecting a listed device connects it; `state` moves
      `discovering → connecting → configuring → streaming` and `link` reaches **2 (playing)**.
- [ ] **Test tone.** The flow auto-plays a test tone on success and you hear it. The
      app asks "Can you hear the test tone?": **Yes** returns to Home, **No, Play Again**
      replays it, **Disconnect** drops the link and returns to Home.
- [ ] **Region buttons.** On a Japan/Asia console (enter = Circle), Circle confirms and Cross
      cancels; elsewhere Cross confirms. The on-screen legend matches.

## 3. Everyday use, reconnect and multiple devices

- [ ] **Remembered on restart.** After a service restart, the last-used device auto-reconnects
      when it is turned on (no app interaction needed).
- [ ] **Two remembered devices.** Pair a second headset. With both remembered, turning on
      either one first makes the service connect to that one ("connect to whichever is on").
- [ ] **Disconnect restores TV.** Disconnect (web, or the app's Options menu) drops the link
      and TV sound returns; the next connect starts fresh.
- [ ] **Backoff then give up.** With the headphones off, a connect attempt retries at ~2, 4, 8,
      16 s (watch `attempts`/`retry_ms`) and then stops with a "turn them on" message rather
      than retrying forever.
- [ ] **Silent-headset recovery.** A headset that connects but stays silent auto-switches SBC
      format (`send_type` flips) and, if still silent, relinks — without a manual reconnect.
      The dual-channel headset ends up remembered in dual channel (`profiles.bin`).

## 4. Audio quality and latency

- [ ] **Backlog stays shallow.** `maxback_ms` at `/api/timing` stays at or below the mode's
      ceiling (12/16/20 ms) across an hour of play; it does not creep toward ~128 ms.
- [ ] **No creeping delay.** Audio stays in sync through several game loads. A load/menu may
      log one `resyncs` or a few `drops`, but these stay occasional and `underruns` stays near
      zero.
- [ ] **Latency modes.** Lowest noticeably tightens lip-sync versus Most stable; Most stable
      has fewer dropouts on busy 2.4 GHz Wi-Fi. The choice survives a restart
      (`/data/bta_audio/latency.bin`, `latency_mode` in status).
- [ ] **Joint-stereo toggle.** Turning joint stereo off/on per device changes `send_type` and
      is remembered.

## 5. Controller coexistence

- [ ] **One controller.** One hour of play on two headsets has no audible dropouts
      (`underruns` near zero) and no controller stutter.
- [ ] **Two controllers.** Repeat with two controllers connected and in use; the wireless
      controller link stays responsive (the sender never polls the driver while streaming).
- [ ] **Wi-Fi contention.** On 2.4 GHz Wi-Fi with traffic, Most stable rides out interference
      better than Lowest; wired Ethernet / 5 GHz is smoothest.

## 6. Rest mode

- [ ] **Sleep/wake resume.** Put the console into rest mode while streaming, then wake it. The
      service re-opens the driver, re-applies the acceptance patch and reconnects on its own
      (message "Woke from rest mode. Reconnecting..."), reaching `streaming` again.
  > Note: rest-mode resume assumes the process clock keeps counting across suspend. If it does
  > not on 13.52, the watchdog is a harmless no-op and recovery falls back to the
  > stream-failure path (slower but still works). This check confirms which happens.

## 7. Phone control and security

- [ ] **LAN blocked by default.** From a phone/PC browser (not the console), `GET /api/status`
      returns **401** and the page shows the pairing prompt; no control is possible.
- [ ] **Enable at console only.** Turn phone control on from the app (or create
      `/data/bta_audio/lan_on`). A six-digit code appears on the TV and in
      `/data/bta_audio/pair_code.txt`. `/api/lan-on` from a LAN client is refused (console
      only).
- [ ] **Pair and control.** Entering the code in the phone browser pairs it (token stored); it
      can then read status and control audio. A wrong code is rejected; ~10 wrong codes trigger
      a ~30 s cooldown that then clears.
- [ ] **Off clears access.** Turning phone control off (or restarting the service) clears the
      code and all tokens; the phone must re-pair.
- [ ] **Code never leaks.** `pair_code` appears only for the console (loopback), never in a LAN
      status response or in `status.json`.

## 8. TV app and diagnostics

- [ ] **Settings rows.** Volume changes with left/right; **Keep TV Sound On** and **Allow
      Phone Control** toggle; **Audio Latency** sets the mode; the Options menu's **Test
      Sound** plays the tone.
- [ ] **Stop Audio Service.** It asks first; **Stop** ends the service, the app then shows
      "The audio service isn't running." and **Start** brings it back while BinLoader is
      still unused this jailbreak.
- [ ] **Theme and text.** The screens read clearly on the TV in the app's gray theme.
- [ ] **Diagnostics.** The web Diagnostics panel and `/api/timing` show live packets, reconnects,
      resyncs, drops/underruns and late-packet counts.
- [ ] **App is display-only.** Closing the app (Back on Home, or Close in the Start dialog)
      returns to the home screen without an error (no CE-34878-0), and leaves the service
      and audio running. If it does crash, `app.log`'s last `exit:` line has the result
      code of the system's close request.
- [ ] **PayLoader off.** With GoldHEN's PayLoader Server off and the service stopped, the
      Start dialog says to turn it on; after turning it on, **Start** brings the service up.

## Known limitations / not verifiable here

- **Other console models:** Slim and Pro (Bluetooth 4.0 vs. the launch unit's 2.1) are
  untested; collect reports via the public beta later.
- **Firmware gate:** the payload refuses to run on any firmware but 13.520.001 (kernel
  fingerprint mismatch) and shows a message rather than exiting silently — this can only be
  confirmed by trying another firmware, which is out of scope.
- **SBC bitpool:** the driver encodes SBC itself and exposes only joint-stereo vs. dual-channel.
  Whether the bitpool can be set from the service (for a lower-bitpool "stable" or higher
  "high quality" mode) is unverified and would be the next console investigation.
- **Acceptance-patch rollback on shutdown** is not implemented; the patch is RAM-only and clears
  on reboot, which is the intended behaviour for this release.
- **Longer numeric PINs** (via the browser controls) are untested.
