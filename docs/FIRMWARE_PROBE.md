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
3. **on any firmware**, runs a read-only signature self-check: it scans a few MB of
   kernel text for each pattern in [`app/fw_signatures.h`](../app/fw_signatures.h)
   and records where each resolved — on 13.52, whether that is the address
   `firmware.h` expects; on another firmware, simply where (if anywhere) the 13.52
   pattern still lands;
4. writes a short summary to `/data/bta_audio/fw_probe.txt` (firmware version,
   whether the base recovered, and the self-check result);
5. shows a notification and exits.

It **writes nothing to the kernel and patches nothing** — it only reads kernel
memory (the service already does that before fingerprinting; the self-check just
scans a bounded span of kernel text). On a firmware whose offsets are unknown it
reads none of the nine windows (they would be meaningless), so the worst case is a
report that says "no table for this firmware yet" plus whatever the signature scan
found. It is safe to run on a console whose layout we do not know.

## Running it

With GoldHEN's PayLoader Server on, send the payload the same way as the service
(for example `tools/deploy_variant.py BTA-Probe-<version>.bin --host <PS4_IP>`, or
any payload sender). Then copy the results back over FTP (port 2121):

- `/data/bta_audio/fw_probe.txt` — always written (now including the self-check).
- `/data/bta_audio/fw_probe.bin` — written only on 13.52 (the capture).

## What to send back

- **On 13.52:** send `fw_probe.bin`. It carries the real bytes the search
  signatures are built from. The `.txt` also shows the self-check — ideally every
  signature resolving to its expected address (an `AMBIGUOUS` line means that
  pattern needs tightening).
- **On another firmware:** send `fw_probe.txt`. It confirms the exact firmware
  version, that base recovery works there, and whether any of the 13.52 signatures
  still resolve — the first hint of how much carries over.

The probe now runs this signature search read-only and reports the result; a later
service build will run the same search before patching and refuse cleanly when any
signature is missing or ambiguous, so an unknown firmware is never patched blind.
See [FIRMWARE_SUPPORT.md](FIRMWARE_SUPPORT.md) for the full plan and the per-firmware
checklist.
