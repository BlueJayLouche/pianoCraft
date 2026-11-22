# Clean-room policy

PianoCraft is an original piano synthesis implementation. Pianoteq binaries,
code, presets, samples, model data, internal formats, and branding are not
inputs to this project and must not be decompiled, extracted, or copied.

Permitted inputs are published acoustic research, public CLAP and Bitwig
documentation, and reference recordings whose license permits the intended
analysis and derived-data distribution. Keep a source and license record for
every calibration corpus. Raw reference recordings are analysis inputs only;
the plugin must not contain or play them.

The initial target is an original upright-piano voice derived from the CC0
Meta Piano corpus. Generated coefficients must be reproducible from the
recorded transformations and checksums in `model/provenance.json`.

Audio we bounce from other instruments in our own host, Pianoteq included, may be
listened to and measured side by side with PianoCraft to find where PianoCraft falls
short. It is diagnosis only: no coefficient, table or target is fitted to it, and
every number in the model stays traceable to a permitted recording.

Implementation work uses only the public behavior required of a CLAP
instrument: MIDI/CLAP note handling, stereo audio, parameter automation, and
state persistence. Compatibility with Pianoteq presets or proprietary model
formats is not a project goal.
