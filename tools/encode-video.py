#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["imageio[pyav]>=2.34", "pillow>=11", "numpy>=1.26", "mido>=1.3"]
# ///

"""Encode a video into the RTV1 stream the E1001 video player consumes.

The container carries everything the firmware needs so playback behavior can
be tuned by re-encoding alone, without reflashing:

- three UC8179 command scripts: init (full-refresh setup for the first
  frame), video (the fast partial-update waveform, register LUTs with
  configurable phase timings), and cleanup (ghost-flush refresh run at
  scene cuts);
- an optional monophonic buzzer note track extracted from a MIDI file;
- 1bpp frames (bit set = white, MSB leftmost) delta-encoded against the
  previous frame as copy/literal runs.

Example:

  ./tools/encode-video.py badapple.mp4 --fps 8 --waveform faster \\
      --midi badapple.mid --midi-track 3 --output badapple.rtv

The video and MIDI inputs stay out of the repository; only this tool is
committed. Waveform phases are in panel frame periods; shorter phases mean
faster refreshes, more ghosting, and less contrast — tune with the playback
stats the firmware reports over MQTT.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import imageio.v3 as iio
import numpy as np

WIDTH, HEIGHT = 800, 480
ROW_BYTES = WIDTH // 8

WAVEFORMS = {
    "balanced": (30, 5, 30, 5, 1),  # GxEPD2's proven register LUT
    "fast": (15, 2, 15, 2, 1),
    "faster": (8, 1, 8, 1, 1),
    "insane": (4, 1, 4, 1, 1),
}


def script(*entries: tuple[int, bytes]) -> bytes:
    out = bytearray()
    for cmd, data in entries:
        out += bytes((cmd, len(data))) + data
    return bytes(out)


def lut(level: int, t1: int, t2: int, t3: int, t4: int, repeat: int) -> bytes:
    return bytes((level, t1, t2, t3, t4, repeat)).ljust(42, b"\x00")


def build_scripts(phases: tuple[int, int, int, int, int],
                  pll: int | None, drive: str) -> tuple[bytes, bytes, bytes]:
    t1, t2, t3, t4, rep = phases
    # "balanced" keeps GxEPD2's pre-pulsed patterns (half the phases swing the
    # wrong way as an activation pulse — good image, weak at short phases).
    # "direct" drives every phase toward the target for double the effective
    # push at the same refresh time; the DC imbalance is compensated by the
    # balanced cleanup refreshes and the final full refresh.
    kw_level, wk_level = (0xAA, 0x55) if drive == "direct" else (0x5A, 0x84)
    init = script(
        (0x00, b"\x1f"),                      # panel setting, OTP full LUT
        (0x01, b"\x07\x07\x3f\x3f\x09"),      # power setting
        (0x06, b"\x17\x17\x28\x17"),          # booster soft start
        (0x61, struct.pack(">HH", WIDTH, HEIGHT)),
        (0x15, b"\x00"),                      # DUSPI off
        (0x50, b"\x29\x07"),                  # VCOM/CDI, N2OCP copy new->old
        (0x60, b"\x22"),                      # TCON
        (0xE3, b"\x22"),                      # PWS
        (0xE0, b"\x02"),                      # CCSET: TSFIX
        (0xE5, b"\x5a"),                      # fast full update (temp 90)
        # PLL before power-on: the frame-rate register does not take effect
        # when written on a powered panel.
        *(((0x30, bytes((pll,))),) if pll is not None else ()),
        (0x04, b""),                          # power on (firmware busy-waits)
    )
    video = script(
        (0x00, b"\x3f"),                      # partial update LUT from registers
        *(((0x30, bytes((pll,))),) if pll is not None else ()),  # PLL frame rate
        (0x82, b"\x30"),                      # VCOM DC -2.5V
        (0x50, b"\x39\x07"),                  # LUTBD, N2OCP
        (0x20, lut(0x00, t1, t2, t3, t4, rep)),
        (0x21, lut(0x00, t1, t2, t3, t4, rep)),
        (0x22, lut(kw_level, t1, t2, t3, t4, rep)),  # black -> white
        (0x23, lut(wk_level, t1, t2, t3, t4, rep)),  # white -> black
        (0x24, lut(0x00, t1, t2, t3, t4, rep)),
        (0x25, lut(0x00, t1, t2, t3, t4, rep)),
    )
    # Ghost flush without leaving register-LUT mode: switching to the OTP
    # LUTs mid-video leaves the resumed register mode degraded (~2.2 s
    # refreshes), so the cleanup is simply the strong "balanced" register
    # waveform; the firmware re-runs the video script right after.
    cleanup = script(
        (0x20, lut(0x00, 30, 5, 30, 5, 1)),
        (0x21, lut(0x00, 30, 5, 30, 5, 1)),
        (0x22, lut(0x5A, 30, 5, 30, 5, 1)),
        (0x23, lut(0x84, 30, 5, 30, 5, 1)),
        (0x24, lut(0x00, 30, 5, 30, 5, 1)),
        (0x25, lut(0x00, 30, 5, 30, 5, 1)),
    )
    return init, video, cleanup


def pack_frame(gray: np.ndarray) -> bytes:
    bits = (gray >= 128).astype(np.uint8)
    return np.packbits(bits, axis=1).tobytes()


def delta_encode(prev: bytes | None, cur: bytes) -> bytes:
    out = bytearray()
    n = len(cur)
    i = 0
    if prev is None:
        while i < n:
            run = min(0x7FFF, n - i)
            out += struct.pack("<H", 0x8000 | run) + cur[i:i + run]
            i += run
        return bytes(out)
    pv = np.frombuffer(prev, dtype=np.uint8)
    cv = np.frombuffer(cur, dtype=np.uint8)
    same = pv == cv
    while i < n:
        if same[i]:
            j = i
            while j < n and same[j]:
                j += 1
            run = j - i
            while run > 0:
                r = min(0x7FFF, run)
                out += struct.pack("<H", r)
                run -= r
            i = j
        else:
            j = i
            # Absorb short "same" gaps into literals: a 2-byte copy token for
            # a 1-2 byte match is not worth breaking the literal run.
            while j < n and (not same[j] or (j + 3 <= n and not same[j:j + 3].all())):
                j += 1
            run = j - i
            while run > 0:
                r = min(0x7FFF, run)
                out += struct.pack("<H", 0x8000 | r) + cur[i:i + r]
                i += r
                run -= r
            i = j
    return bytes(out)


def pcm_track(path: Path, rate: int, start_s: float, duration_s: float,
              volume: float) -> np.ndarray:
    """Decode any ffmpeg-readable audio to 8-bit unsigned mono PCM tuned for
    a piezo: resampled, high-passed (the disc has nothing below ~200 Hz
    anyway and DC offsets waste drive), peak-normalized to `volume`."""
    import av

    container = av.open(str(path))
    resampler = av.AudioResampler(format="s16", layout="mono", rate=rate)
    chunks = []
    for frame in container.decode(audio=0):
        for resampled in resampler.resample(frame):
            chunks.append(resampled.to_ndarray().reshape(-1))
    audio = np.concatenate(chunks).astype(np.float32) / 32768.0
    lo = int(start_s * rate)
    hi = lo + int(duration_s * rate)
    audio = audio[lo:hi]
    if len(audio) < hi - lo:
        audio = np.pad(audio, (0, hi - lo - len(audio)))
    return finish_pcm(audio, rate, volume)


def finish_pcm(audio: np.ndarray, rate: int, volume: float) -> np.ndarray:
    """Shared mastering for the piezo: band-limit into the disc's usable
    range (its resonance sits near 4 kHz; content outside 200 Hz - 5 kHz
    only comes back as hash), soft-compress so a dense mix survives 8 bits,
    and TPDF-dither so quantization error becomes hiss instead of crackle."""
    spectrum = np.fft.rfft(audio)
    freqs = np.fft.rfftfreq(len(audio), 1.0 / rate)
    spectrum[freqs < 200] = 0
    spectrum[freqs > 5000] = 0
    audio = np.fft.irfft(spectrum, n=len(audio))
    peak = float(np.max(np.abs(audio))) or 1.0
    audio = np.tanh(2.2 * audio / peak) / np.tanh(2.2)
    rng = np.random.default_rng(1)
    dither = (rng.random(len(audio)) + rng.random(len(audio)) - 1.0) / 127.0
    return np.clip((audio + dither) * volume * 127.0 + 128.0,
                   0, 255).astype(np.uint8)


def synth_chiptune(midi_path: Path, rate: int, duration_s: float,
                   volume: float) -> np.ndarray:
    """Render the MIDI as a chiptune (square lead, triangle bass, kick and
    hats) — limited voices and no broadband mix content, which is exactly
    what a resonant piezo disc reproduces cleanly."""
    n = int(duration_s * rate)
    mix = np.zeros(n, np.float32)

    def add_notes(track: int, render, level: float) -> None:
        for t_ms, freq, dur_ms in melody_from_midi(midi_path, track,
                                                   duration_s):
            start = t_ms * rate // 1000
            count = min(n - start, max(1, dur_ms * rate // 1000))
            if count <= 0:
                continue
            t = np.arange(count, dtype=np.float32) / rate
            mix[start:start + count] += level * render(t, freq)

    add_notes(3, lambda t, f:  # square lead, plucky decay
              np.sign(np.sin(2 * np.pi * f * t)) * np.exp(-t / 0.22), 0.42)
    add_notes(4, lambda t, f:  # triangle bass, rounder
              (2 / np.pi) * np.arcsin(np.sin(2 * np.pi * f * t)) *
              np.exp(-t / 0.30), 0.30)
    add_notes(5, lambda t, f:  # kick: pitch-swept thump
              np.sin(2 * np.pi * (140 - 90 * np.minimum(t / 0.12, 1)) * t) *
              np.exp(-t / 0.09), 0.60)
    rng = np.random.default_rng(2)
    add_notes(6, lambda t, f:  # hat: short quiet noise tick
              rng.standard_normal(len(t)).astype(np.float32) *
              np.exp(-t / 0.025), 0.10)
    return finish_pcm(mix, rate, volume)


def melody_from_midi(path: Path, track_index: int, limit_s: float) -> list[tuple[int, int, int]]:
    import mido

    mid = mido.MidiFile(str(path))
    events = []  # (t_ms, note, on)
    # MidiFile iteration applies tempo changes; filter to the wanted track's
    # channels since the merged stream loses track identity.
    t = 0.0
    track = mid.tracks[track_index]
    channels = {msg.channel for msg in track if hasattr(msg, "channel")}
    for msg in mid:  # yields absolute-timed messages with tempo applied
        t += msg.time
        if t > limit_s:
            break
        if msg.type in ("note_on", "note_off") and getattr(msg, "channel", None) in channels:
            events.append((int(t * 1000), msg.note, msg.type == "note_on" and msg.velocity > 0))
    notes = []  # (t_ms, freq, dur_ms)
    active: tuple[int, int] | None = None  # (start_ms, note)
    for t_ms, note, on in events:
        if on:
            if active is not None and note >= active[1]:
                start, cur = active
                if t_ms > start:
                    notes.append((start, cur, t_ms - start))
                active = (t_ms, note)
            elif active is None:
                active = (t_ms, note)
        else:
            if active is not None and active[1] == note:
                start, cur = active
                if t_ms > start:
                    notes.append((start, cur, t_ms - start))
                active = None
    packed = []
    for start, note, dur in notes:
        freq = int(round(440.0 * 2 ** ((note - 69) / 12)))
        packed.append((start, freq, min(dur, 0xFFFF)))
    return packed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fps", type=float, default=8.0)
    parser.add_argument("--seconds", type=float, default=0.0,
                        help="encode only the first N seconds (0 = all)")
    parser.add_argument("--start", type=float, default=0.0,
                        help="skip the first N seconds of the source")
    parser.add_argument("--waveform", default="faster",
                        help="named preset (%s) or T1,T2,T3,T4,REP" %
                             "/".join(WAVEFORMS))
    parser.add_argument("--spi-mhz", type=int, default=10)
    parser.add_argument("--pll", type=lambda v: int(v, 16), default=None,
                        help="UC8179 PLL byte in hex (e.g. 3a = 100 Hz frame "
                             "rate); omitted = panel OTP default")
    parser.add_argument("--drive", default="balanced",
                        choices=("balanced", "direct"),
                        help="video LUT polarity: balanced pre-pulse (best "
                             "image) or direct all-phases-toward-target "
                             "(double push per refresh, needs cleanups)")
    parser.add_argument("--redrive", type=int, default=2, choices=(0, 1, 2),
                        help="re-drive changed pixels for N extra frames by "
                             "spoofing the controller's old-data RAM; kills "
                             "ghost trails at the cost of one extra windowed "
                             "RAM write per frame")
    parser.add_argument("--busy-timeout-ms", type=int, default=3000)
    parser.add_argument("--cleanup-change", type=float, default=0.5,
                        help="changed-byte fraction that flags a scene-cut cleanup refresh")
    parser.add_argument("--cleanup-every", type=float, default=20.0,
                        help="force a cleanup refresh at least every N seconds")
    parser.add_argument("--midi", type=Path)
    parser.add_argument("--midi-track", type=int, default=3)
    parser.add_argument("--audio", type=Path,
                        help="audio file for sigma-delta PCM playback through "
                             "the piezo (any ffmpeg-readable format); "
                             "interleaved with the frames")
    parser.add_argument("--chiptune", action="store_true",
                        help="synthesize the PCM track from --midi instead of "
                             "--audio: square lead, triangle bass, drums — "
                             "much cleaner on a piezo than a full mix")
    parser.add_argument("--audio-rate", type=int, default=16000,
                        help="PCM sample rate; 16 MHz timer base must divide "
                             "it evenly (default: %(default)s)")
    parser.add_argument("--volume", type=float, default=0.8)
    args = parser.parse_args()

    if args.waveform in WAVEFORMS:
        phases = WAVEFORMS[args.waveform]
    else:
        parts = tuple(int(p) for p in args.waveform.split(","))
        if len(parts) != 5:
            parser.error("--waveform needs a preset name or T1,T2,T3,T4,REP")
        phases = parts
    init_script, video_script, cleanup_script = build_scripts(phases, args.pll, args.drive)

    meta = iio.immeta(args.video, plugin="pyav")
    src_fps = meta.get("fps", 30.0)
    interval_ms = int(round(1000.0 / args.fps))
    step = max(1, int(round(src_fps / args.fps)))

    frames = []
    prev: bytes | None = None
    last_cleanup_t = 0.0
    raw_total = 0
    for index, frame in enumerate(iio.imiter(args.video, plugin="pyav")):
        t_src = index / src_fps
        if t_src < args.start:
            continue
        if args.seconds and t_src - args.start > args.seconds:
            break
        if (index % step) != 0:
            continue
        from PIL import Image

        im = Image.fromarray(frame).convert("L")
        scale = HEIGHT / im.height
        im = im.resize((int(round(im.width * scale)), HEIGHT))
        canvas = Image.new("L", (WIDTH, HEIGHT), 255)
        canvas.paste(im, ((WIDTH - im.width) // 2, 0))
        cur = pack_frame(np.asarray(canvas))
        payload = delta_encode(prev, cur)
        raw_total += len(payload)
        t_out = len(frames) * interval_ms / 1000.0
        changed = 1.0 if prev is None else float(
            np.count_nonzero(np.frombuffer(prev, np.uint8) !=
                             np.frombuffer(cur, np.uint8))) / len(cur)
        cleanup = False
        if len(frames) > 0:
            if changed >= args.cleanup_change and t_out - last_cleanup_t >= 2.0:
                cleanup = True
            elif t_out - last_cleanup_t >= args.cleanup_every:
                cleanup = True
        if cleanup:
            last_cleanup_t = t_out
        frames.append((cleanup, payload))
        prev = cur
    if not frames:
        print("no frames encoded", file=sys.stderr)
        return 1

    duration_s = len(frames) * interval_ms / 1000.0
    notes = []
    audio = None
    audio_rate = 0
    if args.chiptune:
        if not args.midi:
            parser.error("--chiptune needs --midi")
        audio_rate = args.audio_rate
        audio = synth_chiptune(args.midi, audio_rate, duration_s, args.volume)
    elif args.audio:
        audio_rate = args.audio_rate
        audio = pcm_track(args.audio, audio_rate, args.start, duration_s,
                          args.volume)
    elif args.midi:
        notes = melody_from_midi(args.midi, args.midi_track, duration_s)

    with open(args.output, "wb") as f:
        f.write(b"RTV1")
        f.write(struct.pack("<HHHHHBBBH", WIDTH, HEIGHT, interval_ms,
                            len(frames), len(notes), args.spi_mhz,
                            min(255, args.busy_timeout_ms // 100),
                            args.redrive, audio_rate))
        f.write(struct.pack("<HHH", len(init_script), len(video_script),
                            len(cleanup_script)))
        f.write(init_script + video_script + cleanup_script)
        for t_ms, freq, dur in notes:
            f.write(struct.pack("<IHH", t_ms, freq, dur))
        for index, (cleanup, payload) in enumerate(frames):
            f.write(struct.pack("<I", len(payload) | (0x01000000 if cleanup else 0)))
            f.write(payload)
            if audio is not None:
                # Interleave this frame's slice of the PCM track so audio
                # streams alongside the frames it accompanies.
                lo = index * audio_rate * interval_ms // 1000
                hi = (index + 1) * audio_rate * interval_ms // 1000
                chunk = audio[lo:hi].tobytes()
                f.write(struct.pack("<H", len(chunk)))
                f.write(chunk)

    total = args.output.stat().st_size
    cleanups = sum(1 for c, _ in frames if c)
    print(f"{len(frames)} frames @ {interval_ms} ms ({duration_s:.1f} s), "
          f"{cleanups} cleanup refreshes, {len(notes)} notes, "
          f"audio {audio_rate} Hz, "
          f"{total / 1e6:.2f} MB ({raw_total // max(1, len(frames))} B/frame avg), "
          f"waveform {phases}, spi {args.spi_mhz} MHz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
