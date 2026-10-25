# Firmware probe

The audio service is built for one firmware (**13.520.001**): every kernel offset
it uses lives in [`app/firmware.h`](../app/firmware.h), recovered by hand from that
build. Supporting another firmware means finding the same nine routines and two
data layouts on it. The probe is the first, read-only step toward that — it does
**not** add support for any firmware, it only gathers the facts needed to.

## What it does

`BTA-Probe-<version>.bin` is a standalone GoldHEN payload. When you send it, it:

1. recovers the kernel text base from the `LSTAR` MSR (the same way the service
   does before it applies its patch);
2. **on 13.52**, copies a 64-byte window around each of the nine offsets in
   `firmware.h` into `/data/bta_audio/fw_probe.bin`, so byte-pattern signatures
   can be built from the real code;
3. on **any** firmware, writes a short summary to `/data/bta_audio/fw_probe.txt`
   (the firmware version, and whether the base was recovered);
4. shows a notification and exits.

It **writes nothing to the kernel and patches nothing** — it only reads kernel
memory, which the service already does before fingerprinting. On a firmware whose
offsets are unknown it reads none of them (they would be meaningless), so the
worst case is a report that says "no table for this firmware yet". It is safe to
run on a console whose layout we do not know.

## Running it

With GoldHEN's PayLoader Server on, send the payload the same way as the service
(for example `tools/deploy_variant.py BTA-Probe-<version>.bin --host <PS4_IP>`, or
any payload sender). Then copy the results back over FTP (port 2121):

- `/data/bta_audio/fw_probe.txt` — always written.
- `/data/bta_audio/fw_probe.bin` — written only on 13.52 (the capture).

## What to send back

- **On 13.52:** send `fw_probe.bin`. It carries the real bytes the search
  signatures are built from.
- **On another firmware:** send `fw_probe.txt`. It confirms the exact firmware
  version and that the base-recovery step works there, which is what decides
  whether that firmware can be worked on next.

Once signatures exist, a later build will search for them at startup and refuse
cleanly when any is missing, so an unknown firmware is never patched blind.
