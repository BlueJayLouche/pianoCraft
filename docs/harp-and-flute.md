# Harp and flute

The first two voices of the final act (`docs/old-house-needs.md`, section 6) ship as plugins of
their own in the PianoCraft bundle: PianoCraft Harp and PianoCraft Flute. Both follow the
orchestral-library conventions there: CC11 trims the output, pitch bend is ±2 semitones, there are
no keyswitches, and articulations are parameters and presets.

Neither has a reference corpus yet, so every law below is analytic: published physics, with
constants set by ear and by `tests/orchestra_tests.cpp`. `tools/evaluate_model.py` scores only the
piano. The clean-room policy applies unchanged: a calibration corpus for either needs a licence
that permits analysis and derived-data distribution, recorded in `model/provenance.json`.

## Harp

One string per key, B0 to G#7. Each string is a bank of two-pole modes (up to 40 partials, the
first 12 doubled as the weaker, slower polarization along the board, 0.05% sharp). They ring until
they die away, so a glissando's strings keep sounding with no re-damping. A re-plucked string keeps
a quarter of its motion.

| Law | Status | Source and check |
|---|---|---|
| Pluck: released from the finger's triangle, partial n drives the board with sin(n pi beta) / n | Analytic | The ideal plucked string (Fletcher & Rossing, *The Physics of Musical Instruments*, ch. 2); harpists pluck near a third to a half of the string (Chadefaux, Le Carrou & Fabre on harp plucking, JASA 2012-13). Pluck Position default 0.35. |
| Fingertip: -12 dB/octave above 3 kHz x Brightness x (0.55 + 0.65 velocity) | Chosen by ear | A Gaussian corner left the top octave nearly a sine (C7's second harmonic -43 dB). |
| Près de la table: pluck point moves to 0.06, corner x2.5, level -3 dB | Chosen by ear | Thin and guitar-like, as the articulation is described. |
| Harmonics: the string an octave down, touched at its middle; only its even partials ring (odd ones at 6% for 50 ms) | Analytic | The key names the sounding pitch. Test: the string's own fundamental is >40 dB below the sounding one. |
| Decay T60 = 5 s (f / 440 Hz)^-0.7 x Sustain Length | Chosen by ear | About 7 s at C4, 1.7 s at C7. f^-0.85 killed upper partials of mid strings within 2 s. |
| Wire-wound bass strings lose more: a loss factor 2e-3 below E2, fading out by C4 (T60 = ln 1000 / (pi f eta)) | Chosen by ear | The bass sounded synthetic: a steady sawtooth, C2's 4th to 10th partials only 10-25 dB down after a second. Now 22-62 dB down. |
| Pluck from rest: the modes start at full swing, so the string's static load on the board, their first sample, is taken off as the board springs back over a period | Analytic | The output starts at zero and steps when the kink reaches the bridge (about 0.7 ms at C4), a plucked string's square-wave force. Before, every note jumped to full level on its first sample, like a hammer. |
| Tension modulation: a hard pluck stretches a low string, up to 8 cents x velocity^2 sharp, fading as its swing squared (half its T60 in time), C2 and below to nothing above E5 | Chosen by ear | The plucked string's twang. C2 ff: +6.5 cents at the pluck, under 1 cent by 2.4 s. |
| Inharmonicity 1.5e-5 x 2^((key - 60) / 24) | Analytic | Gut and nylon are nearly harmonic; short treble strings stiffer. |
| Soundbox: 12 band-pass modes 125 Hz-2.6 kHz, Q 10-30 | Chosen by ear | Mixed at Body Level. |
| Sympathetic strings: every string the pedals sound rings undamped, its first 6 partials (tuned and damped as a plucked string's) driven by the board's motion and radiating through it; a partial in tune settles at 0.2 of the board's; the string being played is left to its pluck | Analytic, coupling by ear | A harp's strings are never damped as a piano's are, so octaves, fifths and (a little off) thirds ring on (Le Carrou, Gautier and Badeau on the harp's sympathetic strings). A C3 damped by the hand leaves a halo about 30 dB down, mostly C3 itself once free again, C4 and C5, then G4 and E5. Sympathetic Level scales it. |
| Pedals: seven strings an octave, set by what is played, starting in C major; a note outside the setting takes the place of the neighbour (sharing its letter) played least recently | Analytic | So the halo follows the passage's key, not every chromatic string as a pedalled piano's would. |
| Stereo: bass left, treble right, amplitude panning only | Analytic | Width 0 is bit-identical left and right. |
| Note-Off Damping adds a decay rate of Damping / 0.12 s at note-off; CC64 holds it off | Analytic | Default 0: the strings ring, as the score's glissandi need. |

Velocity is the dynamic (pp about 16 dB below mf); CC1 does nothing, as in the orchestral
libraries' plucked patches. Timbre and per-note modulation move Brightness and Pluck Position at
the pluck. 40 ringing strings cost 4.6% of a 512-frame block at 48 kHz.

## Flute

A monophonic jet-drive waveguide. The bore is a delay line of one period with a one-pole loss at
its open end. The jet is deflected by the wave returning to the embouchure, crosses to the labium
in a fraction of a period, and splits the flow there as tanh (Fletcher, JASA 60, 1976; Verge's
jet-drive work). Its waveguide form follows Karjalainen, Laine, Laakso and Välimäki (ICMC 1991).

| Law | Status | Source and check |
|---|---|---|
| Jet flow U (tanh(eta + 1.0) - tanh(1.0)), deflection eta = -2.2 (0.8 + 0.4 CC1) (1 + 0.3 h) x reflected / U, h = 0 at C4 and 1 at C7 | Analytic form, constants by ear | The deflection scales with acoustic velocity over jet speed, so soft breath still speaks. The jet saturates hard at every breath that speaks (eta swings +-3 to +-24), so this loop sets pitch, attack and level, not the partials. The (1 + 0.3 h) makes up the top's higher losses: every key to C8 speaks from CC1 0.15. |
| Jet flight 0.4 period, plus a jet low-pass at 2 f0 | Selected | At 0.5 the twelfth is in phase too and C#4-E4 overblew. 0.42 / 0.38 left the third harmonic 7-12 / 10-15 dB down. 0.4 plays 52 cents sharp, plus 5 h cents, which the tuning takes back: within 2.5 cents C4-G7 at every breath, rising 2-3 cents from soft to loud as a flute does. |
| Bore delay exact: one period less the loss filter's and DC blocker's phase delay at f0 | Analytic | Without it the flute ran 19-72 cents sharp. |
| Radiated partials: the bore wave at unit amplitude, split again at the labium, tanh(d x + 0.9) - tanh(0.9), its fundamental normalised out; d = 0.3 + 4.7 breath^2, scaled by (1 kHz / f0)^2 above 1 kHz | Fitted to Fletcher's measurements | Fletcher (JASA 57, 233-237, 1975): from p to f the low octave's upper partials grow and its fundamental barely moves; in the third octave the fundamental grows and the partials stay over 10 dB down. Above 1 kHz the full drive aliased (G7: -26 dB lines); now -52 dB or less to G7. |
| Formant, normalised at f0: 12 dB/octave rise below 500 x breath Hz, 6 dB/octave fall above 500 x 2^breath Hz | Fletcher's formant, first-order fall | His forte formant rises to 500 Hz and falls above 1 kHz; at piano it falls from 500 Hz and barely rises. The fall is first-order because the jet's partials already fall about 6 dB/octave. |
| CC1 (or the sounding note's Pressure, which `piano.py` writes alongside it) sets jet speed, breath^(0.4 + 0.8 h); velocity sets only the attack (90 ms at pp to 30 ms at ff, a tongued puff up to +35% that settles within 50 ms) | Fletcher | CC1 0.3 to 1.0: A4 +5 dB with H1 +5 and H3 +21; C4's H2 and H3 come within 2-7 dB of H1; C7 +13 dB, its partials 17-25 dB down. Before any CC1 arrives, breath is 0.8. |
| Breath noise: turbulence on the jet (plus 8x for the tongue's first 20 ms) and band-limited air (0.8-5 kHz) on the output, louder with jet speed, sqrt(f / C4) | Chosen by ear; pitch law after Fletcher | Breath Noise scales the steady turbulence and the air; the tongue's burst is what starts the note, so the flute speaks at Breath Noise 0. Players blow high notes harder (Fletcher: pressure ~ frequency), so the jet is faster and its air louder. |
| Level rises 3 dB an octave, sqrt(f / C4) | Fletcher's pressure law; open-end radiation | Without it C7 played 5 dB under C4 at the same breath, thin and whistly. At CC1 0.6, C4 to C7 is -15.5 to -12 dBFS; at CC1 1, C7 peaks at -2 dBFS. |
| Compass B3 (with the B foot) to D7 | Analytic | C3 barely spoke (-53 dBFS), and slurring out of it clicked. |
| Vibrato: +-10% breath pressure (+-5% jet speed) and +-4 cents, entering 0.25 s into a note over 0.6 s; CC21 scales it (64 is the Vibrato Depth setting) | Fletcher | Flute vibrato is a 5 Hz, 10% pressure pulse that mostly modulates the upper partials. |
| Legato: a note-on while a key is held, or at the instant the last note ends (piano.py's slides touch their notes), changes the bore to the new note's length with no new attack, if the last note was sounding. Over Legato Time (12 ms: the keys' travel, Almeida, Chow, Smith and Wolfe, JASA 2009) the radiated sound crossfades from the bore read at the old length to the new, while the loop itself takes the new length half way, over ~2.4 ms (a pad seating); for ~15 ms the jet is 15% slower and its turbulence 4x. A tongued note stops the air first | Analytic, the disturbance by ear | A woodwind has no glide: a key changes the tube's length outright, the old resonance dying as the new one builds. Gliding the bore's length, even over 12 ms, swept through the pitches between: in an octave slur the tritone between was 7 dB louder than either note. Crossfading the bore's length inside the loop still swept (+3 dB). Now it is 9-11 dB under them, for flute, oboe and clarinet alike, with no sample-step clicks; the level dips briefly while the new note forms. Shared by the flute, oboe and clarinet (MonoWind). | The slur tests dip under 1 dB; a repeated pitch is tongued (dips 10 dB). The slurred-from note gets its NOTE_END. |

Releasing the sounding note ends the phrase even if an earlier key is still held: the score's
approach notes are released after the melody note starts, which this handles. Return to a held key
only if trills need it. Timbre moves Brightness continuously; it is a sustained instrument.

## Next

- A calibration corpus: recordings whose licence permits derived-data distribution, or Andrew's
  own. Spectra by dynamic, decay times and the flute's noise level are the first things to fit.
- By ear first: the harp's soundbox, the flute's breath level, and its top octave.
