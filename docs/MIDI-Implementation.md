# Chordially 1.0: MIDI implementation

Chordially listens to **every MIDI input** (the KeyStep 37, the S-1, the T-8, anything connected over USB).

## Received

| Message | What it does |
|---|---|
| Note on / off | **CHORD on:** the key is the bass note and plays that key's chord from the chord set (any octave). **CHORD off:** the held notes (up to 4) become the chord. Channel: SETUP > "Keys in on channel" (ALL = every channel except a THRU IN channel of the S-1 / T-8). |
| CC 64 sustain | Keeps the chord after you let go of the keys. |
| CC 1 mod wheel, pitch bend | Go to the built-in synth. |
| CC 74 / 71 / 76 / 77 | KeyStep 37 knobs 1–4 (factory bank 1): **FILTER, STYLE, VARIATION, TEMPO** (60–187 BPM). Reassign with MIDI LEARN. |
| Any CC (learned) | MIDI LEARN targets: FILTER, RESO, EG INT, ATTACK, DECAY, SUSTAIN, RELEASE, STYLE, VARIATION, TEMPO, GATE, GATE RND, SWING, DELAY, REVERB, VOLUME, CHORD SET, SOUND, S-1 / T-8 LEVEL and REVERB. In SETUP click LEARN and turn a knob. You can also Alt-click a knob on the panel. |
| Program change 0–63 | Selects pattern 1-1 … 8-8. While playing, the change waits for the pattern's end (or the next step / bar: CHANGE). SETUP > Program change in. |
| Start (FA) / Continue (FB) / Stop (FC) | Start and stop Chordially: the KeyStep's PLAY / STOP. SETUP > Start / stop in. |
| Clock (F8) | Followed only when SETUP > Clock = EXTERNAL. The tempo is measured from the clock, and the LCD shows **EXT**. |

## Transmitted

| Message | Where |
|---|---|
| Clock (F8, 24 per quarter) | Sent continuously to every output with clock ON (SETUP > Clock out to). Every port is on by default except the IAC bus. Only sent while Chordially is the master (Clock = INTERNAL). |
| Start (FA) / Continue (FB) / Stop (FC) | Same ports, on PLAY / STOP. |
| Note on / off | The chord part's notes, exactly as they sound (phrases, strums, held chords), go to the port chosen in SETUP > "Chord notes out to", on its channel. With "Built-in synth" OFF, only an external synth plays them. |
| Notes, CC, pitch bend (THRU) | An S-1 / T-8 strip with THRU IN set to a channel passes what comes in on that channel to the box's own port, on its THRU OUT channel. For example, KeyStep on channel 2 → S-1 on channel 1. |

Timing: outgoing clock and notes are stamped to the audio. They leave at the same sample the built-in synth sounds, with the output device's latency taken into account.

## S-1 and T-8 over USB

- **Audio:** an audio device whose name contains "S-1" (or "T-8") is connected automatically. Its USB audio is mixed into Chordially's output with LEVEL, MUTE, DELAY and REVERB. On first use, macOS asks for microphone access, which Chordially needs to hear USB audio: allow it. You can change the name it looks for in the strip's Device field.
- **Sync:** the box's USB MIDI port gets the clock and start / stop. Set the S-1 / T-8 to follow USB MIDI clock (their SYNC / clock source setting), and they play in time with Chordially.

## KeyStep 37 setup

- Leave the KeyStep's own CHORD mode off: Chordially makes the chords.
- For its knobs to send CCs, use its default (factory) knob bank, or map them with MIDI LEARN.
- PLAY / STOP on the KeyStep start and stop Chordially. If the KeyStep's sync is set to USB, its arpeggiator / sequencer follows Chordially's clock.
