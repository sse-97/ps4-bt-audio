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
  also writes `/data/bta_audio/app.log`, and the service writes `status.json` (every 10 s)
  and `service.log` (messages, connection steps, failed requests, audio summaries). Both
  logs stamp lines with the UTC time; L3+R3 in the app saves a screenshot to
  `app-frame.ppm`.

Tick each box. If anything fails, note the `message`, `state` and the relevant counters.

## 1. Install and first launch

- [ ] **PKG install (Option A).** Copy the PKG to `/data/pkg/`, install it from GoldHEN's
      Package Installer (package source HDD). A **Bluetooth Audio** icon appears on the home
      screen. Opening it shows the app at **1080p**.
- [ ] **Service autostart from the app.** With the service not already running, opening the app
      sends the bundled payload to BinLoader (port 9090) once and the service comes up
      (`status.json` appears, the web page answers). `app.log` shows `payload sent to
      BinLoader`.
- [ ] **Payload only (Option B).** With the service not running, run
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
      and TV sound returns; connecting again right away works on the first try.
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

- [ ] **Sleep/wake resume.** Put the console into rest mode while streaming (a game running is
      fine), then wake it. The service re-opens the driver, re-applies the acceptance patch
      and reconnects on its own, reaching `streaming` again within about 20 s of the home
      screen returning. `service.log` shows `wake: seconds asleep …` and "Woke from rest
      mode. Reconnecting...".
- [ ] **App and phone after waking.** Right after waking, the app opens on Home (not the
      "isn't running" dialog) and the phone page loads: the service reopened its listening
      socket (`http: listening again` in `service.log`).
  > Note: on 13.52 the process clock stops in rest mode (measured: a 30 s connect timeout
  > spanned 12 minutes of sleep), and rest mode aborts the service's sockets. The service
  > therefore detects the wake by comparing it with the wall clock.

## 7. Phone control and security

- [ ] **LAN blocked by default.** From a phone/PC browser (not the console), `GET /api/status`
      returns **401** and the page shows the pairing prompt; no control is possible.
- [ ] **Enable at console only.** Turn phone control on from the app (or create
      `/data/bta_audio/lan_on`). A six-digit code appears on the TV and in
      `/data/bta_audio/pair_code.txt`; the TV notice names `http://<PS4_IP>:8097`.
      `/api/lan-on` from a LAN client is refused (console only).
- [ ] **QR code.** While phone control is on, every app screen shows the QR code, the
      address and the code on the left. Scanning the QR code with a phone opens the
      pairing page.
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
- [ ] **Stop Audio Service.** It asks first; **Stop** ends the service and disconnects the
      device, the app then shows "The audio service isn't running." and **Start** shows
      "Starting audio service..." with a spinner and brings it back, without a new
      jailbreak.
- [ ] **Sound after a restart.** Stop the service, wait a few minutes, start it again: the
      remembered device reconnects and plays sound (`service.log` shows a fresh dial,
      `connect: dial rc`, not a reused link).
- [ ] **Theme and text.** The screens read clearly on the TV in the app's gray theme.
- [ ] **Diagnostics.** The web Diagnostics panel and `/api/timing` show live packets, reconnects,
      resyncs, drops/underruns and late-packet counts.
- [ ] **Opens beside a game.** With a game running, opening Bluetooth Audio does not ask to
      close the game (the app is a mini app, CATEGORY `gde`). The app draws and reads the
      controller; `app.log` shows `Video memory: … alloc=0x0 map=0x0`. Closing it returns
      to the game.
- [ ] **Stays open like a system app.** Back on Home (and on the "isn't running"
      dialog) shows "Press the PS button to go to the home screen" and closes nothing;
      only the dialog's Close button closes the app. (The system's go-home call crashed
      the home screen on 13.52, so the app no longer makes it.) Leaving with the PS
      button and coming back (from the home screen or the Quick Menu) shows the app as
      it was, without starting it over; the button that brought it back does nothing in
      it. `app.log` shows `focus:` lines for each switch.
- [ ] **No black screen over an HDR game.** With an HDR game running (e.g. Horizon
      Zero Dawn on an HDR TV), open the app and go back: the TV should not go black.
      `app.log` shows `Video output mode: any, rc=…`; KLog's `video: port:0 …` line on
      each switch shows whether the mode still changes (`RGB2020_PQ` vs `CERGB`).
- [ ] **App is display-only.** Closing the app (Quick Menu > Close Application, or Close in
      the Start dialog) returns to the home screen without an error (no CE-34878-0), and
      leaves the service and audio running. If it does crash, `app.log`'s last `exit:`
      line has the result code of the system's close request.
- [ ] **TV stays muted across switches.** With Keep TV Sound On off and audio playing,
      switch between the game, the home screen and the app: the TV may sound for about a
      second after a switch, then stays silent.
- [ ] **One search at a time.** On Add Device, Search Again shows only when no search is
      running or waiting and no device is connecting; pressing Triangle repeatedly
      doesn't restart it (`service.log`: one `scan: started` per 16 s at most).
- [ ] **PayLoader off.** With GoldHEN's PayLoader Server off and the service stopped, the
      Start dialog says to turn it on; after turning it on, **Start** brings the service up.
      Turning it off while the service runs changes nothing.
- [ ] **Home-screen icon.** The app's tile is the white Bluetooth symbol on blue, like the
      system's own apps.

## 9. Add Device never searches during a connection attempt

On 13.52 a search started while the PS4 is still trying to reach a device that
doesn't answer leaves Bluetooth stuck: the controller drops and won't come back,
even by USB, until a full power-off (hold the power button until the second
beep). Every check here must leave the controller connected.

- [ ] **While connecting.** Speaker off, open the app (it starts "Connecting to …"),
      press **Add Device** within 20 s. The screen shows Searching..., the controller
      stays connected. `service.log`: `add: Add Device opened`, `add: reconnecting
      stopped`, `radio: cancelling the dial`, then either `radio: dial cancelled after
      N ms, the driver confirmed it` or `radio: waited N ms for the dial to end (no
      confirmation)`, and only then `scan: started`. Note which of the two appears
      and the `connect: link state` lines before it: they show whether the driver
      reports a dial in progress.
- [ ] **While playing.** With the speaker playing, press **Add Device**: TV sound
      returns, the speaker disconnects, the search starts about a second later
      (`link: dropped`, `radio: link down after N ms`, `scan: started`).
- [ ] **Leaving reconnects.** Press Back on Add Device without choosing: the speaker
      reconnects and plays (`add: Add Device closed`, then a dial).
- [ ] **Choosing during a search.** Choose a device while Searching... shows: the
      search stops and the dial follows about 2 s later (`connect: waiting for the
      radio` may appear), never during the search.
- [ ] **App put away.** Open Add Device, then switch to a game with the PS button:
      within about 10 s the speaker reconnects (`add: ended, the app went away`).
- [ ] **Phone search.** Press Scan on the phone page while the speaker plays: it
      disconnects, the search runs, and a minute after it ends without a choice the
      speaker reconnects (`add: ended, no device was chosen`).
- [ ] **Search Again during a connect.** After choosing a device, Search Again is not
      offered until it has connected or failed.
- [ ] **Quick retry.** With a device that doesn't answer at once, the next attempt
      follows about 2 s after the driver gives up (`connect: the driver ended the dial
      after ~15000 ms`), not after the 30 s window.
- [ ] **Driver events.** `service.log` has `bt event: type …` lines around each
      connection attempt (at most 12 in 10 s, and a stream's first 20). With a device
      that connects but stays silent (e.g. AirPods), they show what it asks for.

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
