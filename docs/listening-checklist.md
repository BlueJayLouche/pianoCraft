# PianoCraft listening check

Use dry renders without room reverb, limiting, or time-varying gain. Match
reference and PianoCraft levels with one static gain per file based on the
first 100 ms RMS, preserving the natural decay. Compare the same MIDI note,
velocity layer, and duration against the corresponding acoustic reference. Do
not count spectral similarity or a successful Bitwig load as a pass on its own.
Randomize A/B order and conceal file names where practical. Also compare soft,
medium, and loud renders without level matching so the velocity law remains
audible and cannot be hidden by normalization.

For bass, middle, and treble notes at soft, medium, and loud velocities, check:

- Attack: immediate hammer contact without a click, delayed onset, or synthetic noise burst.
- Dynamics: louder playing becomes brighter as well as louder.
- Tuning: stable pitch and plausible inharmonicity across the register.
- Unison decay: natural beating and a prompt/aftersound transition rather than static detuning.
- Release: key-up damping is prompt but not unnaturally clipped.
- Sustain: held notes and pedal-held notes decay naturally without looping or sudden level changes.
- Sympathy: undamped strings respond selectively; no broadband reverb-like wash.
- Restrikes: repeated notes excite the existing string state without resetting its phase or clicking.
- Polyphony: dense chords remain clear and free of dropouts, uncontrolled peaks, and obvious stolen voices.

Record the reference and PianoCraft file names, level-matching method, listening
device, audible defects, and whether the result is accepted. A failed listening
check leaves the convincing-piano milestone open even when all technical tests
pass.
