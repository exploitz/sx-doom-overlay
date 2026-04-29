#!/usr/bin/env python3
"""Extract music from a Doom WAD as MIDI files, optionally rendering to OGG.

Standalone — no pip dependencies. Reads the WAD directly, converts any MUS
lumps to MIDI in-process, then (if fluidsynth + ffmpeg + a SoundFont are
available) renders each MIDI to OGG so you can drop them straight into
sdmc:/switch/sx-doom-overlay/music/ for the OGG branch.

Usage:
    extract-wad-music.py CHEX.WAD
    extract-wad-music.py CHEX.WAD -o out/ --sf2 ~/sf2/SC-55.sf2

Tools needed for the OGG render step (optional):
    fluidsynth   apt: fluidsynth   |   brew: fluid-synth   |   choco: fluidsynth
    ffmpeg       apt: ffmpeg       |   brew: ffmpeg        |   choco: ffmpeg
    SoundFont    e.g. SC-55.sf2 — same source the Brandon Blume OGG pack used.

Without those it still emits .mid files; convert them however you like.

Licensed under GPLv2 (matching the rest of sx-doom-overlay).
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# WAD reader
# ---------------------------------------------------------------------------

def read_wad_lumps(path: Path):
    """Yield (name, data) for every D_* lump in the WAD."""
    with path.open("rb") as f:
        magic = f.read(4)
        if magic not in (b"IWAD", b"PWAD"):
            raise ValueError(f"{path}: not a WAD (magic={magic!r})")
        num_lumps, dir_offset = struct.unpack("<II", f.read(8))
        f.seek(dir_offset)
        directory = []
        for _ in range(num_lumps):
            offset, size = struct.unpack("<II", f.read(8))
            name = f.read(8).rstrip(b"\x00").decode("ascii", errors="ignore")
            directory.append((offset, size, name))
        for offset, size, name in directory:
            if not name.startswith("D_") or size == 0:
                continue
            f.seek(offset)
            yield name, f.read(size)


# ---------------------------------------------------------------------------
# MUS → MIDI converter
#
# MUS format reference: https://www.doomworld.com/idgames/docs/editing/mus_form
# Algorithm mirrors chocolate-doom/src/mus2mid.c — public domain, ~1996.
# ---------------------------------------------------------------------------

# MUS channel N maps to MIDI channel below. MUS 15 (percussion) → MIDI 9
# (GM percussion); MUS 9-14 shift up by 1 to skip MIDI 9. Indexed 0..15 so
# `mus_chan = ev & 0x0f` is always a valid lookup — no Optional path.
MUS_TO_MIDI_CHAN: list[int] = [n if n < 9 else n + 1 for n in range(15)] + [9]

# MUS controller index → MIDI controller number. Indices 0/1 are special
# (program change / bank select) and handled inline; the rest are vanilla CC.
MUS_CTRL_TO_MIDI_CC = {
    1: 0,    # bank select MSB
    2: 1,    # modulation
    3: 7,    # volume
    4: 10,   # pan
    5: 11,   # expression
    6: 91,   # reverb depth
    7: 93,   # chorus depth
    8: 64,   # sustain pedal
    9: 67,   # soft pedal
    10: 120, # all sounds off
    11: 123, # all notes off
    12: 126, # mono mode
    13: 127, # poly mode
    14: 121, # reset all controllers
}

MIDI_PPQ        = 89                  # ticks per quarter — chocolate-doom value
MIDI_TEMPO_USEC = int(1_000_000 * 60 / 70)   # 70 BPM


def _vlq(n: int) -> bytes:
    """Encode n as a MIDI variable-length quantity."""
    if n == 0:
        return b"\x00"
    out = []
    out.append(n & 0x7f)
    n >>= 7
    while n > 0:
        out.append((n & 0x7f) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def mus_to_midi(mus: bytes) -> bytes | None:
    """Convert a MUS lump to a Format-0 MIDI file. Returns None if not MUS."""
    if mus[:4] != b"MUS\x1a":
        return None

    score_len, score_start = struct.unpack_from("<HH", mus, 4)
    end = min(len(mus), score_start + score_len)

    track = bytearray()

    # Set tempo (meta event)
    track += b"\x00\xff\x51\x03"
    track += MIDI_TEMPO_USEC.to_bytes(3, "big")

    # Reset all controllers on every channel for predictable starting state.
    for ch in range(16):
        track += b"\x00"
        track += bytes([0xb0 | ch, 121, 0])

    last_volume = [127] * 16
    delay = 0
    pos = score_start

    while pos < end:
        ev = mus[pos]
        pos += 1
        last     = bool(ev & 0x80)
        ev_type  = (ev >> 4) & 7
        mus_chan = ev & 0x0f
        midi_ch  = MUS_TO_MIDI_CHAN[mus_chan]

        wrote_event = False

        if ev_type == 0:        # release note
            note = mus[pos] & 0x7f
            pos += 1
            track += _vlq(delay)
            track += bytes([0x80 | midi_ch, note, 0])
            delay = 0
            wrote_event = True

        elif ev_type == 1:      # play note
            b = mus[pos]
            pos += 1
            note = b & 0x7f
            if b & 0x80:
                vol = mus[pos] & 0x7f
                pos += 1
                last_volume[mus_chan] = vol
            else:
                vol = last_volume[mus_chan]
            track += _vlq(delay)
            track += bytes([0x90 | midi_ch, note, vol])
            delay = 0
            wrote_event = True

        elif ev_type == 2:      # pitch wheel
            pw = mus[pos]
            pos += 1
            midi_pw = max(0, min(16383, (pw - 128) * 64 + 8192))
            track += _vlq(delay)
            track += bytes([0xe0 | midi_ch, midi_pw & 0x7f, (midi_pw >> 7) & 0x7f])
            delay = 0
            wrote_event = True

        elif ev_type == 3:      # system event (valueless controller)
            ctrl = mus[pos] & 0x7f
            pos += 1
            cc = MUS_CTRL_TO_MIDI_CC.get(ctrl)
            if cc is not None:
                track += _vlq(delay)
                track += bytes([0xb0 | midi_ch, cc, 0])
                delay = 0
                wrote_event = True

        elif ev_type == 4:      # change controller
            ctrl = mus[pos] & 0x7f
            pos += 1
            val  = mus[pos] & 0x7f
            pos += 1
            if ctrl == 0:
                track += _vlq(delay)
                track += bytes([0xc0 | midi_ch, val])
                delay = 0
                wrote_event = True
            else:
                cc = MUS_CTRL_TO_MIDI_CC.get(ctrl)
                if cc is not None:
                    track += _vlq(delay)
                    track += bytes([0xb0 | midi_ch, cc, val])
                    delay = 0
                    wrote_event = True

        elif ev_type == 5:      # end of measure — ignored
            pass

        elif ev_type == 6:      # score end
            track += _vlq(delay)
            track += b"\xff\x2f\x00"
            delay = 0
            break

        elif ev_type == 7:      # unused — skip one byte
            pos += 1

        if last:
            d = 0
            while pos < end:
                b = mus[pos]
                pos += 1
                d = (d << 7) | (b & 0x7f)
                if not (b & 0x80):
                    break
            delay += d

        # Avoid leaving an event dangling if we wrote nothing this iteration.
        if not wrote_event and ev_type not in (5,):
            pass

    if not track.endswith(b"\xff\x2f\x00"):
        track += b"\x00\xff\x2f\x00"

    out = bytearray()
    out += b"MThd"
    out += struct.pack(">I", 6)
    out += struct.pack(">HHH", 0, 1, MIDI_PPQ)   # format 0, 1 track, PPQ
    out += b"MTrk"
    out += struct.pack(">I", len(track))
    out += track
    return bytes(out)


# ---------------------------------------------------------------------------
# Pipeline
# ---------------------------------------------------------------------------

def render_to_ogg(midi_path: Path, sf2: Path, ogg_path: Path,
                  keep_wav: bool, gain: float, oggq: int) -> bool:
    """Render midi_path → ogg_path via fluidsynth + ffmpeg. Return True on success."""
    wav_path = ogg_path.with_suffix(".wav")
    try:
        subprocess.run(
            ["fluidsynth", "-F", str(wav_path), "-i", "-q",
             "-r", "44100", "-g", str(gain),
             str(sf2), str(midi_path)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error",
             "-i", str(wav_path),
             "-codec:a", "libvorbis", "-qscale:a", str(oggq),
             str(ogg_path)],
            check=True,
        )
        if not keep_wav:
            wav_path.unlink(missing_ok=True)
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ! render failed for {midi_path.name}: {e}", file=sys.stderr)
        return False


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Extract music from a Doom WAD as MIDI/OGG.")
    ap.add_argument("wad", type=Path, help="path to .wad file")
    ap.add_argument("-o", "--outdir", type=Path,
                    help="output directory (default: <wad-stem>-music/)")
    ap.add_argument("--sf2", type=Path, default=None,
                    help="SoundFont for OGG render (omit to stop after MIDI)")
    ap.add_argument("--gain", type=float, default=0.6,
                    help="fluidsynth output gain (default 0.6, range 0.0–10.0)")
    ap.add_argument("--ogg-quality", type=int, default=5,
                    help="vorbis -qscale:a (default 5, range -1–10)")
    ap.add_argument("--keep-mus", action="store_true",
                    help="also write raw .mus lumps")
    ap.add_argument("--keep-wav", action="store_true",
                    help="keep intermediate .wav files")
    ap.add_argument("--keep-mid", action="store_true",
                    help="keep .mid files even after OGG render")
    args = ap.parse_args()

    if not args.wad.exists():
        print(f"error: {args.wad} not found", file=sys.stderr)
        return 1

    outdir = args.outdir or args.wad.parent / f"{args.wad.stem.lower()}-music"
    outdir.mkdir(parents=True, exist_ok=True)

    have_fs = args.sf2 is not None and args.sf2.exists() and shutil.which("fluidsynth")
    have_ff = shutil.which("ffmpeg") is not None
    if args.sf2 and not have_fs:
        print(f"warn: --sf2 given but fluidsynth/SoundFont missing — MIDI only",
              file=sys.stderr)
    if have_fs and not have_ff:
        print(f"warn: ffmpeg missing — leaving WAVs, no OGG encode",
              file=sys.stderr)
    do_ogg = have_fs and have_ff

    print(f"WAD: {args.wad}")
    print(f"Out: {outdir}/")
    print(f"Pipeline: WAD → MIDI{' → WAV → OGG' if do_ogg else ''}")
    print()

    n_mus, n_mid, n_skip, n_ogg = 0, 0, 0, 0
    for name, data in read_wad_lumps(args.wad):
        base = name.lower()
        if data[:4] == b"MUS\x1a":
            midi = mus_to_midi(data)
            if midi is None:
                print(f"  skip {name}: MUS parse failed", file=sys.stderr)
                n_skip += 1
                continue
            if args.keep_mus:
                (outdir / f"{base}.mus").write_bytes(data)
            n_mus += 1
        elif data[:4] == b"MThd":
            midi = data
            n_mid += 1
        else:
            print(f"  skip {name}: unknown format ({data[:4]!r})", file=sys.stderr)
            n_skip += 1
            continue

        midi_path = outdir / f"{base}.mid"
        midi_path.write_bytes(midi)

        if do_ogg:
            ogg_path = outdir / f"{base}.ogg"
            ok = render_to_ogg(midi_path, args.sf2, ogg_path,
                               args.keep_wav, args.gain, args.ogg_quality)
            if ok:
                n_ogg += 1
                print(f"  {base:<12} → {ogg_path.name}")
                if not args.keep_mid:
                    midi_path.unlink(missing_ok=True)
            else:
                print(f"  {base:<12} → {midi_path.name} (OGG render failed)")
        else:
            print(f"  {base:<12} → {midi_path.name}")

    print()
    print(f"Done: {n_mus} MUS converted, {n_mid} MIDI passed through, "
          f"{n_skip} skipped, {n_ogg} OGG rendered")
    print(f"Drop the .ogg files into sdmc:/switch/sx-doom-overlay/music/ on the SD card")
    return 0


if __name__ == "__main__":
    sys.exit(main())
