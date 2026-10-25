# Firmwares and console models

The service does not ask what firmware it is on before deciding whether to run. At
startup it looks for the kernel routines it needs by their own code, uses whatever it
finds, and turns off only the features whose addresses it could not resolve. Install
and try it on any firmware and any model; nothing refuses on a version number.

## How resolution works

1. **The kernel base** comes from the `LSTAR` MSR masked to the kernel text's 16 KiB
   alignment — no per-firmware constant. A result that doesn't have the shape of a
   kernel address resolves nothing.
2. **One ring-0 pass** sweeps a 6 MB window of kernel text, testing every signature
   at each position. A signature is a short run of bytes with a mask: `0xff` means the
   byte must match, `0x00` wildcards an operand byte that legitimately moves between
   builds (RIP-relative displacements, `call`/`jmp` rel32, absolute pointers), leaving
   the opcode skeleton. Exactly one match resolves the address; no match, or more than
   one, leaves it zero and turns its feature off. See [`app/sigscan.h`](../app/sigscan.h)
   and [`app/kernel_sigs.h`](../app/kernel_sigs.h).
3. **Six routines resolve this way on any firmware**: `copyout`, the Bluetooth
   lock/unlock pair, the command dispatcher, the accept patch site, and the
   cache-fingerprint routine. The bytes were cut from 13.520.001 and confirmed on that
   hardware — all six, each a single match. On another build they are candidate
   patterns: frequently right, not guaranteed.
4. **The classic device cache bounds are read out of the matched code's own operands**,
   not guessed, and are rejected unless they are ordered, bounded and an exact number
   of records apart.

Everything firmware-specific lives in [`app/kernel.h`](../app/kernel.h).

**Confirmed on hardware (2026-10-25, 3.0.0, launch-model PS4 on 13.520.001.)** A live
run resolved all six signatures, reported `patch+devlist+widen`, applied the patch
(`rc 0`), connected a Libratone ONE and played audible sound, with nothing in the
kernel log. So on 13.52 the search is confirmed, not merely a candidate — it reaches
the same addresses the fixed offsets used to name. That says nothing about any other
firmware: only that firmware's own bytes can.

## Two addresses that are not guessed

The Bluetooth context pointer is a data pointer in a zeroed region, and the format
string the dispatcher expects is a run inside a string table. Neither has anything to
pattern on, so both come from a small table applied **only** when the firmware reports
13.520.001, together with the record field offsets. On any other firmware they stay
zero.

That is deliberate rather than conservative housekeeping: `bt_prepare` *writes*
through the context pointer, so a guessed value would mean an unverified kernel write.
A feature that stays off is correct; a guess is not.

## The three capabilities

Each is independent, and each degrades on its own.

| Capability | Needs | Without it |
|---|---|---|
| Accept patch (bit 1) | the patch site and `copyout` — both signature-found, so any firmware | the service still tries to connect; the headset may link anyway |
| Device list (bit 2) | the context pointer, so 13.520.001 only | the list is built from radio scan events instead: address, class of device and signal strength, but **no name**, so the UI shows the MAC |
| Three-slot widening (bit 4) | the context pointer and the format string, so 13.520.001 only | skipped silently; it is an optimisation, and the link carries audio without it |

The whole audio path — connect, disconnect, link state, pairing, scan, send audio,
HDMI mute — runs on `/dev/bt` ioctls and needs no kernel offsets at all. That is why
the service can work on a firmware nobody has tried.

## What to expect on an untested console

It may resolve nothing, connect but stay silent, or simply work. The patch lives only
in RAM, so a reboot removes it and nothing in system firmware is modified; the worst
case is a crash, after which you reboot and, if you want, delete the app.

All models run the same kernel for a given firmware, so a Slim or Pro on 13.520.001
needs no new offsets — it uses the table already here. The open question for those is
only whether their newer radio streams SBC the way the launch unit's does.

## What to report

The service says what it resolved in one line, in three places: the top of
`/data/bta_audio/service.log` (written at startup, so it survives a console that later
dies), the `kernel` field of `GET /api/status`, and the app's **Kernel Info** dialog
(Triangle on Home). The web page shows it at the bottom of Diagnostics.

```
kernel: base ok, 6/6 signatures, fw 0x13520001, patch+devlist+widen
kernel: base ok, 5/6 signatures (command ambiguous), fw 0x09000001, patch only
kernel: base ok, 5/6 signatures (patch_site already patched), fw 0x13520001, patch+devlist+widen
kernel: base not recovered, fw 0x09000001
```

`patch_site already patched` is normal, not a fault: the pattern opens with the very
bytes the patch overwrites, so once the patch is live that form stops matching and a
seventh pattern recognises the flipped one instead. The service restarts far more
often than the console reboots, and the patch is RAM-only, so a restarted service
meets its own patch constantly.

A useful report is that line, your firmware version, your console model, and what the
headset did — nothing, connected silently, or played.
