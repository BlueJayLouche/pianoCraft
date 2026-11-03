# What *The Old House on the Corner* needs from PianoCraft

PianoCraft's first use is the score of *The Old House on the Corner*, a VR piece for Meta Quest (Unity and
FMOD). Andrew Cherry is the sound designer; Chris Luscri directs. Creative calls go to Chris through Andrew.
This note says what the piece needs, in priority order, so the instrument grows towards it.

PianoCraft never runs on the headset. It renders in Bitwig Studio 6.0.11. The bounces go into FMOD, and the
Quest plays them through Resonance Audio. Plugin CPU matters only in Bitwig.

## How the piano is used

- **One motif, decaying.** The score is one piano motif, written by `piano.py`
  (`~/Documents/Bitwig Studio/old-house-piano/`, which writes `piano.dawproject`). It's played in a different
  key per scene. The bounces are stretched (PaulXStretch, x2 to x16 and more), granulated and tape-worn,
  after The Caretaker's *Everywhere at the End of Time*. The piano is heard worn and far away far more often
  than clean.
- **The piano is Lola's.** The piece follows Lola, who played the piano and later had Alzheimer's. A tape
  labelled "Lola - piano" has turned up and is not yet digitised. Scene 3 is where the actual piano is first
  heard. In the final scene it comes back with George's spoken poem, *Home*.
- **The final act is orchestral.** The motif grows into a symphonic poem in the manner of Debussy's
  *Prélude à l'après-midi d'un faune*: flute, harp, muted strings and horns. `piano.py` writes it on five
  Poem tracks, with dynamics as CC1 curves in each clip. The fifth is Lola's piano.
- **Lola's piano in the poem is soft throughout** (added 3 Nov). Chris heard a forte piano part against the
  poem on 2 Nov: "too strident… straining/competing to make itself heard. It should be delicate, caressing,
  sensual even." His reference is Debussy's "Pagodes" (*Estampes* I). The new part stays at velocities
  0.14 to 0.34 (MIDI about 18 to 43): a low open fifth held for bars like a gong, and pentatonic eighths
  ringing under the tune. So the upright's soft end carries it: warm, round and even at *pp*, with
  long, clean decays in the bass. Chris may move this line to the woodwinds instead (*Faune*'s flute, oboe,
  clarinet), at the same soft level.

## Needs

### 1. A mono render that sums cleanly

Every bounce is mono, 48 kHz / 24-bit, because Resonance plays each emitter as a point source. A spaced pair
summed to mono comb-filters.
- Stereo Width at 0 (or a Mono switch) should give a true mono render that doesn't comb-filter at any Mic
  Position.
- Add a test that compares each mic position's mono sum with one channel and fails on deep notches.

### 2. An aged upright

The voice is already an upright. Lola's was a home piano: confirm its type from the tape. What the score needs
is wear, set per scene: lightly worn in the first scene, most worn in scene 3, whole at the close.
- Hammers hardened and grooved unevenly, so brightness varies from key to key.
- Unisons drifted apart, with a seeded detune per note, not one global amount.
- Uneven stretch and pitch slackened from A4.
- Some damper leakage, and uneven key response.

One Age (or Wear) control that moves these together would suit the score. It's set by hand per scene, not
automated, which fits the recompute cost of the tuning controls noted in the README.

### 3. Calibration from the house's own pianos

Two sources are coming. The clean-room policy applies to both.
- **Andrew's upright**, recorded close and in the room at 96 kHz in late November, if confirmed. It's his own
  recording, so it's fine as a calibration corpus once its source and licence are recorded in
  `model/provenance.json`.
- **Lola's tape.** Cassette means wow, flutter, hiss and lost high end. Use it to estimate tuning (A4,
  stretch, unison beating) and to judge character by ear, not as a spectral target. It's the family's
  recording: keep it out of `calibration/` and this repo until Chris confirms it may be used.

### 4. The same render every time

- A re-render goes into FMOD with Replace Audio File, so it has to line up with the one it replaces. The same
  MIDI and state must give the same audio: no unseeded randomness, and an offline bounce that matches
  real-time playback.
- It must work at 48 and 96 kHz.

### 5. Bitwig and CLAP

- **State.** It must survive save and reload in a Bitwig project. Andrew checks every new plugin on first
  use.
- **Presets.** Bitwig drops devices when it imports a `.dawproject`, so voicings have to be quick to reload.
  Ship CLAP presets, or document a `.bwpreset` per voicing.
- **Expressions.** Map Bitwig's per-note Timbre to brightness (Hammer Hardness for the piano). Keep velocity
  and CC64 as they are.

### 6. Woodwind and strings: what the final act needs, in order

Match the orchestral-library conventions so the same MIDI drives PianoCraft or a sample library:
- CC1 is continuous dynamics. On sustained instruments it shapes loudness and timbre; velocity shapes only the
  attack. `piano.py` writes each CC1 curve again as per-note Pressure, which Bitwig imports from a
  `.dawproject`, so Pressure is the note's dynamic too.
- CC11 is an expression trim, CC64 sustain, pitch bend ±2 semitones.
- No keyswitches: `piano.py` doesn't write them. Use one preset or parameter per articulation.

In order of priority:
1. **Harp.** A plucked string on the existing string and soundboard model, so it's the shortest step. It
   needs pluck position, *près de la table*, harmonics, and glissandi: fast runs across strings that keep
   ringing, without re-damping.
2. **Celesta and antique cymbals (crotales).** Struck bars and plates, modal: small.
3. **Strings, con sordino.** Bowed, so a continuous exciter. CC1 drives bow pressure and speed. It needs
   vibrato (CC21), tremolo as an articulation (bow reversals, not repeated notes),
   *sul tasto*, harmonics, and ensemble width (divisi, section detune) as well as solo.
4. **Flute.** The *Faune* solo lives in the low, veiled register (about C♯4 to E5).
   - Legato on overlapping notes (monophonic), because `piano.py` writes a chromatic approach note a third of
     a beat before many melody notes.
   - Breath noise level, vibrato, and CC1 as breath pressure.
   - Long held notes that don't fade by themselves.
5. **Horn.** Soft, distant, and stopped. Lowest priority.

### 7. Timing

PianoCraft doesn't hold anything up. Scene 5A's treatment is Mon 2 Nov and the handoff is Mon 16 Nov. Until
its voices are ready, the score uses Pianoteq 9, Bitwig's Orchestral Tools *Orchestral Strings* and Spitfire's
*BBC Symphony Orchestra Discover*. A PianoCraft render swaps in later, at the same length and loop points.

## An idea, not a requirement

An effect mode in which incoming audio rings the strings and soundboard, as if the sustain pedal were held.
The house's sounds, or George's voice, would set Lola's piano sounding. That fits two of Chris's notes: "house
as bell jar / conductor", and the final act marrying George's voice to her piano. Ask Andrew before building
it.
