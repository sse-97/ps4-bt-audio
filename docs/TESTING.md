# Testing on a console

Everything that can be checked without a PS4 is covered by the host unit tests
(`make test`) and the CI build (payload, PKG and UI-scene renders). This is the part
they cannot: how the build behaves on real hardware. Any firmware and any model is
worth a run — that is the point of the release.

**What you need:** the release PKG and payload (`Bluetooth-Audio-3.0.0.pkg`,
`BTA-Audio-3.0.0.bin`), a jailbroken console with GoldHEN, Bluetooth headphones, and
FTP access (port 2121).

**Where to read results:** the web page at `http://<PS4_IP>:8097/` → **Diagnostics**,
the raw `GET /api/status` and `GET /api/timing` JSON, `/data/bta_audio/service.log`
(messages, connection steps, failed requests, audio summaries) and
`/data/bta_audio/app.log`. Both logs stamp lines with the UTC time; L3+R3 in the app
saves a screenshot to `app-frame.ppm`.

## Start here

- **The kernel line.** `service.log`'s second line, the `kernel` field in
  `/api/status`, and the app's **Kernel Info** dialog (Triangle on Home) all carry it.
  Read it first: it says how much of the service your kernel gave it. See
  [FIRMWARE.md](FIRMWARE.md).
- **Install.** Copy the PKG to `/data/pkg/`, install it from GoldHEN's Package
  Installer (package source HDD). The **Bluetooth Audio** icon appears and the app
  opens at 1080p. Opening it with the service down sends the bundled payload to
  BinLoader once and the service comes up (`app.log`: `payload sent to BinLoader`).
- **Payload only.** `tools/deploy_variant.py BTA-Audio-3.0.0.bin --host <PS4_IP>
  --connect` reports `version 3.0.0`. `tools/install_bta.py --host <PS4_IP> --payload
  BTA-Audio-3.0.0.bin` adds the AutoRun entry; after a reboot and jailbreak the service
  starts on its own. `--disable-autorun` undoes it.
- **Connect.** Put the headphones in pairing mode, open **Add Device**, pick them.
  `state` moves `discovering → connecting → configuring → streaming` and `link` reaches
  **2**. A test tone plays on success and the app asks whether you heard it.

## What to check once it plays

- **Reconnect.** After a service restart the last-used device reconnects on its own
  when it is on. With two devices remembered, whichever is on gets connected. With the
  headphones off, attempts back off at ~2, 4, 8, 16 s (`attempts`, `retry_ms`) and then
  stop with a "turn them on" message.
- **Silent-headset recovery.** A headset that connects but stays silent should
  auto-switch SBC format (`send_type` flips) and then relink, without a manual
  reconnect.
- **Latency.** `maxback_ms` at `/api/timing` stays at or below the mode's ceiling
  (12/16/20 ms) and does not creep toward ~128 ms. Audio stays in sync across several
  game loads; a load may log a `resyncs` or a few `drops`, but `underruns` stays near
  zero. Lowest should feel tighter than Most stable, and Most stable should ride out
  busy 2.4 GHz Wi-Fi better.
- **Controller coexistence.** An hour of play, with two controllers in use, without
  audible dropouts or controller stutter.
- **Rest mode.** Sleep while streaming, then wake: the service re-opens the driver,
  re-applies the patch and reconnects within about 20 s of the home screen returning
  (`service.log`: `wake: seconds asleep …`, then `http: listening again`).
- **Phone control.** From a phone, `/api/status` returns `401` until you turn phone
  control on at the console and enter the six-digit code from the TV. `/api/lan-on`
  from a LAN client is refused. Turning it off clears the code and all tokens.
  `pair_code` never appears in a LAN response or in `status.json`.
- **Add Device never searches during a dial.** On 13.52 a search started while the PS4
  is still trying to reach a device that doesn't answer leaves Bluetooth stuck — the
  controller drops and won't come back, even by USB, until a full power-off. Press **Add
  Device** while "Connecting to …" is on screen: the controller must stay connected, and
  `service.log` must show `radio: dial cancelled after N ms, the driver confirmed it`
  (or `radio: waited N ms for the dial to end (no confirmation)`) **before**
  `scan: started`.
- **The app beside a game.** Opening Bluetooth Audio over a running game does not ask
  to close it (mini app, CATEGORY `gde`); closing it returns to the game with no
  CE-34878-0 and leaves audio running. Back on Home closes nothing.

## What to report

Whatever happened, including nothing. Include:

1. **The kernel line**, verbatim.
2. **Your firmware version** and **console model** (Base, Slim, Pro).
3. **What the headset did** — nothing, connected but silent, or played — and its make
   and model.
4. `service.log` if anything failed, plus the `message`, `state` and the counters
   named above.

A console that resolved nothing is as useful a report as one that worked.

## Known limits

- **Silent headsets.** AirPods Pro (1st and 2nd generation) and the Xbox Wireless
  Headset connect, pair and accept every packet, but play nothing in either stereo
  format; a Libratone ONE plays. The service sees no difference (`bt event` type 11
  after setup reads 115 for the Libratone, 95/56/88 for the others). Next step: record
  what the PS4 sends, e.g. with a Linux machine posing as a speaker.
- **Slim/Pro radios.** They share the kernel, so offsets are not the question; whether
  their 4.0-class radio streams SBC the way the launch unit's 2.1 radio does is.
- **HDR games.** Opening the app over an HDR game switches the TV out of HDR and back
  (about a second of black each way).
- **SBC bitpool.** The driver encodes SBC itself and exposes only joint stereo vs.
  dual channel; whether the bitpool can be set from the service is unverified.
- **Longer numeric PINs** (via the browser controls) are untested.
- The patch is RAM-only and clears on reboot. There is no rollback on shutdown, which
  is the intended behaviour.
