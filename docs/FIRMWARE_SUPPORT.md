# Supporting more firmwares (and PS4 Slim/Pro)

The service is built for one firmware, **13.520.001**. Everything specific to it
lives in [`app/firmware.h`](../app/firmware.h): nine kernel-text offsets, two data
layouts, the LSTAR base-recovery constants, a prologue fingerprint, and the one
acceptance-patch descriptor. Every other source file is firmware-neutral —
[`app/firmware.h`](../app/firmware.h) says so itself: *"Supporting another firmware
would mean adding a second table beside this one and choosing between them by the
live fingerprint; no other source file would change."*

This document is the plan for doing that honestly. It does not add support for any
firmware; it describes the design that would, the research each firmware needs, and
— just as important — the limits of what research alone can promise.

## Two independent problems, often confused

Users ask for "older firmwares" and "Slim/Pro" in one breath, but these are
different axes and only one of them is about kernel offsets.

| Axis | What changes | What it costs us |
| --- | --- | --- |
| **Firmware version** | The kernel is a different build, so the nine routines and two data layouts sit at different addresses. | A new per-firmware table (or a signature that finds them). |
| **Console model** (Slim/Pro) | The Bluetooth *hardware* differs — the launch unit is Bluetooth 2.1, Slim/Pro are 4.0-class — and so can the driver's behaviour. | Nothing in `firmware.h`: all models run the **same kernel** for a given firmware, so a Slim or Pro already on a supported firmware needs **no new offsets**. The unknown is whether audio actually flows on its radio. |

The practical consequence is worth stating plainly: **a Slim or Pro on 13.52 is not
an offset problem at all.** It would use the exact table we already have. What we
cannot know from a desk is whether its 4.0 radio streams SBC the way the launch
unit's 2.1 radio does — which is the same class of unknown as the silent-headset
problem (see [ON_CONSOLE_TESTING.md](ON_CONSOLE_TESTING.md), "Silent headsets").

## Today's design, and why it is already safe

The acceptance patch ([`probe/bt_accept_patch.c`](../probe/bt_accept_patch.c)) and the
read-only [firmware probe](FIRMWARE_PROBE.md) both:

1. recover the kernel text base from the `LSTAR` MSR minus a fixed delta, and
   reject the result unless it has the shape of a kernel base
   (`(base >> 48) == 0xffff`, 16 KiB-aligned);
2. refuse outright unless `sceKernelGetSystemSwVersion` reports exactly
   `0x13520001`;
3. before writing, read the target bytes and compare them to the expected value,
   writing only on an exact match and reading the result back.

So the current failure mode on any other firmware is a **clean refusal**, never a
wrong write. That invariant is the thing to preserve through everything below.

## The generalization: find the routines, don't hardcode them

The version gate in step 2 is a stand-in for the real question — *"are the nine
routines where I think they are?"* On 13.52 we answer it with a fixed offset plus a
fingerprint check. The generalization is to answer it by **search**:

- For each routine, carry a byte-pattern (a *signature*) that identifies it by its
  own code, with the bytes that legitimately move between builds masked out — the
  operand bytes that encode addresses (RIP-relative displacements, `call`/`jmp`
  rel32, absolute pointers), leaving the opcode/ModRM skeleton.
- At startup, scan the kernel text for each signature. Require **exactly one**
  match; derive the offset from where it matched.
- Keep it **all-or-nothing**: unless every signature (and the anchors for the two
  data layouts) resolves to a single address, the service refuses and patches
  nothing — exactly as it refuses an unknown firmware today. The patch step still
  verifies the live bytes before the flip, so search only ever *locates*; it never
  loosens the write check.

Done this way, an unknown or mis-detected firmware is safe by construction: the
worst case stays "this firmware isn't recognised." This is worth doing even while
13.52 is the only target, because it replaces "trust one hardcoded address" with
"verify the kernel actually contains these routines before touching it."

## How a signature must be built — the part that needs real research

A signature is only as good as the evidence it was cut from, and this is exactly
where guessing fails:

- **The source of truth is the target firmware's own kernel**, read in a
  disassembler (Ghidra/IDA). Each routine is identified by its *role* — the
  acceptance check, `copyout`, the BT lock/unlock pair, the command dispatcher and
  its format string, the two fingerprinted prologues — not by any address carried
  over from 13.52.
- **The mask must be confirmed, not assumed.** A byte is only safe to treat as
  "fixed" if it is actually identical across more than one firmware; a byte is only
  safe to mask if it actually moves. That is a cross-firmware *diff*, so a trustworthy
  signature wants at least two firmwares' bytes behind it. A pattern cut from a single
  build is a hypothesis about which bytes are stable — frequently right, but not
  "high confidence," and not something to ship as if it were.
- **Uniqueness is a requirement, not a nicety.** If a signature matches twice in
  kernel text, it is not a signature yet; tighten it until it matches once.

Only the launch unit on 13.52 has given us real bytes so far (the probe's
`fw_probe.bin`). That is one data point: enough to *propose* signatures, not enough
to *trust* them elsewhere. This is the honest reason the project does not simply
ship guessed patterns.

## What is implemented

The "find the routines, don't hardcode them" mechanism now exists in code, and is
covered by the host tests (`make test`):

- [`app/sigscan.h`](../app/sigscan.h) — the masked matcher. It scans a buffer for
  one signature and returns the single match, *no match*, or *ambiguous* (more than
  one). A caller that locates a routine this way refuses on anything but a single
  hit, so the all-or-nothing invariant above holds by construction.
- [`app/fw_signatures.h`](../app/fw_signatures.h) — candidate signatures for six of
  the nine routines, cut from the 13.520.001 capture and verified to match it
  uniquely: `copyout`, `bt_lock`, `bt_unlock`, the command dispatcher (which is also
  the second fingerprinted prologue, so six signatures cover seven offsets), the
  acceptance patch site, and the cache fingerprint site. The fixed bytes agree with
  the fingerprints already in [`firmware.h`](../app/firmware.h) — the accept check
  and the shared prologue — and the tests assert that.

Two of the nine are not code and still need their referencing instruction read per
firmware, not a pattern:

- **`CTX_PTR`** is a data pointer in a zeroed region — nothing to pattern on. It is
  recovered from the RIP-relative load that reads it.
- **`FMT_STRING`** lives inside a string table; its bytes are not distinctive.

And one layout falls out for free: the cache-fingerprint routine's own
displacements are `BTA_FW_CACHE_FIRST` and `BTA_FW_CACHE_END`, so that data layout
is read from the code rather than guessed.

This is the **Safe** rung for 13.52 and good *candidate* patterns for everything
else. It is **not** validation for another firmware — the masks are still a
one-firmware hypothesis (see above). The read-only [firmware probe](FIRMWARE_PROBE.md)
now runs these signatures against a live kernel (checklist item 6) and reports where
each resolves, so the candidates can be confirmed on hardware without patching
anything.

**Confirmed on 13.52 hardware (2026-10-25):** a probe run resolved all six
signatures to exactly their `firmware.h` offsets, each a single match. (It was done
on a freshly-booted kernel: with the service running and the accept patch applied,
`patch_site` instead reads `NOT FOUND`, because its bytes are the patched form, so
the self-check doubles as a check that the patch is live.) On 13.52 the search is
therefore **Validated**, not just a candidate; the masks stay a one-firmware
hypothesis for any *other* firmware until that firmware's bytes confirm them.

## Per-firmware research checklist

For each firmware we want to add (all read-only until the final step):

1. **Confirm it is exploitable.** It must be a firmware GoldHEN supports, on a
   console the user owns. If GoldHEN can't run on it, nothing here applies.
2. **Get the kernel for that firmware** from your own console/dump, for analysis in
   a disassembler. (We neither source nor redistribute firmware; this is the user's
   own, on hardware they own.)
3. **Locate each of the nine routines by role** and record a candidate signature
   for each, masking operand bytes as above.
4. **Locate the two data layouts** — the classic device cache and the per-link
   records — and their field offsets, the same way.
5. **Diff against 13.52** (and any other firmware already done) to confirm which
   masked bytes really are volatile. Tighten any signature that isn't unique.
6. **Validate on the actual console, read-only**, with the firmware probe: confirm
   the base recovers, the version reads, and — the probe now runs the signatures
   from [`app/fw_signatures.h`](../app/fw_signatures.h) — that each resolves to
   exactly one address. Nothing is patched in this step.
7. **Only then enable patching** for that firmware, behind the all-or-nothing
   search. Ship the acceptance patch for it only after its live bytes have been
   confirmed to match the expected before/after.

## Slim/Pro checklist

Because the kernel is shared per firmware, the offset work above is already done for
a Slim/Pro on a supported firmware. What remains is hardware confirmation:

1. Run the service on the Slim/Pro (same build, same table).
2. Confirm the acceptance patch applies (it will, if the firmware matches) and a
   device connects.
3. **Confirm audio actually plays**, in both SBC modes (joint stereo and dual
   channel). This is the step that cannot be predicted from offsets: the 4.0 radio
   and its driver may negotiate the stream differently.
4. If it connects but stays silent, that is a driver-behaviour problem in the same
   family as the silent headsets — captured and investigated the same way (record
   what the console sends), not fixed by any offset.

## What "supported" honestly means

Three different bars, and it is worth not blurring them:

- **Safe** — the service refuses cleanly and patches nothing it can't verify. The
  signature-search design gives us this *everywhere*, with no per-firmware work.
- **Validated** — on a specific firmware, the search has been confirmed (read-only)
  to resolve every routine to exactly one address. This needs that firmware's bytes
  once.
- **Supported** — on a specific firmware *and model*, a device has been confirmed to
  connect and **play sound**. This needs one real test on that hardware.

"Works with high confidence on firmwares and models nobody has ever run it on" is
not one of these bars, and it is not reachable: the confirming evidence only exists
on those consoles. Proper research gets us *Safe* everywhere and good *candidate*
signatures for free — but the step from candidate to *Validated*, and from Validated
to *Supported*, is a short read-only probe and a short audio test on the target.
That is what the probe is for, and why it is read-only: it makes each confirmation
cost the tester nothing and risk nothing, rather than asking anyone to patch a
kernel we have only ever seen from the outside.

## Non-goals (unchanged)

- No flash or firmware-file writes; the patch is RAM-only and clears on reboot.
- Refuse-cleanly is the invariant: no firmware is ever patched on a partial or
  ambiguous match.
- No firmware images are sourced, bundled, or redistributed by this project.
