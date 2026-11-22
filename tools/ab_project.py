#!/usr/bin/env python3
"""An A/B test project for Bitwig: the same playing on two tracks, PianoCraft and another piano.

    python3 tools/ab_project.py [out.dawproject]   writes renders/ab/ab-test.dawproject and its section list
    python3 tools/ab_project.py --events out.txt   the same playing as an event list, for an offline render
    python3 tools/ab_project.py --check            runs the self-check

At 60 bpm, so a beat is a second. Sections, each a clip and a marker: single notes at velocities 40, 80 and 120
(the Meta recordings' layers), staccato and chords (the dampers), repeated notes, the pedal alone (its noise), a
pedalled chord's bloom and the dampers landing on it, half-pedalling, the soft pedal, a pedalled chorale, the
Old House tune, and a sweep of the keyboard under the pedal. CC64 and CC67 are track automation, as piano.py writes
them: Bitwig places points inside clips at the project's start. Bitwig drops devices when it imports a
.dawproject: load PianoCraft Upright on one track and the other piano on the other, with no effects, and bounce
each. Diagnosis only (docs/clean-room.md): nothing is fitted to another instrument's bounce.
"""
import itertools
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

GAP = 2.0
C_MAJOR = [0, 2, 4, 5, 7, 9, 11]


def plan():
    """Sections [(name, start, end, notes [(t, length, key, velocity 0-1)])] and CC points {controller: [(t, v)]},
    all in seconds from the project's start."""
    sections, cc = [], {64: [(0.0, 0.0)], 67: [(0.0, 0.0)]}
    t = 0.0

    def section(name, notes, length):
        nonlocal t
        sections.append((name, t, t + length, [(t + a, l, k, round(v, 3)) for a, l, k, v in notes]))
        t += length + GAP

    def pedal(controller, points):  # points relative to the section about to be added
        cc[controller] += [(round(t + a, 3), v) for a, v in points]

    keys = [21, 33, 48, 60, 72, 84, 96]
    section("Notes 40-80-120", [(4.0 * (i * len(keys) + j), 3.0, k, v / 127)
                                for i, v in enumerate((40, 80, 120)) for j, k in enumerate(keys)], 4.0 * 3 * len(keys))
    section("Staccato", [(1.5 * i, 0.15, k, 0.7) for i, k in enumerate((33, 45, 57, 69, 81))] +
            [(8.0, 0.2, k, 0.8) for k in (36, 40, 43, 48)] + [(10.0, 0.2, k, 0.8) for k in (60, 64, 67, 72)], 12.0)
    runs = [(0.25 * i, 0.2, 60, 0.3 + 0.6 * i / 7) for i in range(8)] + \
           [(3.0 + 0.4 * i, 0.3, 33, 0.5 + 0.4 * i / 5) for i in range(6)] + \
           [(6.0 + 0.25 * i, 0.2, 60, 0.3 + 0.6 * i / 7) for i in range(8)]
    pedal(64, [(5.9, 0.0), (5.95, 1.0), (9.0, 1.0), (9.05, 0.0)])
    section("Repeats (last run pedalled)", runs, 10.0)
    pedal(64, [(1.0, 0.0), (1.005, 1.0), (3.0, 1.0), (3.005, 0.0), (4.0, 0.0), (4.5, 1.0), (6.0, 1.0), (6.5, 0.0)])
    section("Pedal alone: stamp, then slow", [], 8.0)
    pedal(64, [(0.0, 0.0), (0.05, 1.0), (4.5, 1.0), (4.505, 0.0)])
    section("Pedal bloom, dampers land; then unpedalled", [(0.5, 0.1, k, 0.85) for k in (36, 43, 48)] +
            [(6.5, 0.1, k, 0.85) for k in (36, 43, 48)], 9.0)
    pedal(64, [(0.0, 0.0), (0.05, 1.0), (1.0, 1.0), (1.3, 0.5), (3.0, 0.5), (5.0, 0.0)])
    section("Half pedal", [(0.2, 0.2, k, 0.7) for k in (48, 52, 55, 60)], 6.0)
    arpeggio = [(0.4 * i, 0.35, k, 0.7) for i, k in enumerate((60, 64, 67, 72, 76))]
    pedal(67, [(2.4, 0.0), (2.45, 1.0), (4.6, 1.0), (4.65, 0.0)])
    section("Soft pedal: off, then on", arpeggio + [(2.5 + a, l, k, v) for a, l, k, v in arpeggio], 5.0)

    # The chorale: I vi IV ii V I, two seconds each, rising, with legato pedalling (lifted just after each chord,
    # down again 0.13 s later), as piano.py's pedal() does.
    chords = [(48, (60, 64, 67, 72)), (45, (60, 64, 69, 72)), (41, (60, 65, 69, 72)), (50, (62, 65, 69, 74)),
              (43, (59, 62, 67, 71)), (48, (60, 64, 67, 72))]
    notes, points = [], [(0.0, 0.0), (0.05, 1.0)]
    for i, (bass, upper) in enumerate(chords):
        v = 0.3 + 0.65 * min(i, 4) / 4 if i < 5 else 0.6
        notes += [(2.0 * i, 2.0, k, v) for k in (bass - 12, bass, *upper)]
        if i:
            points += [(2.0 * i + 0.02, 1.0), (2.0 * i + 0.03, 0.0), (2.0 * i + 0.15, 0.0), (2.0 * i + 0.17, 1.0)]
    points += [(12.0, 1.0), (12.05, 0.0)]
    pedal(64, sorted(points))
    section("Chorale, legato pedal, rising", notes, 13.0)

    # The Old House tune's first eight bars (piano.toml), 3/4 at 80 bpm: melody over a bass note and a broken chord.
    beat = 0.75
    melody = [(67, 2), (69, 1), (67, 2), (64, 1), (65, 1), (69, 1), (72, 1), (67, 3),
              (65, 2), (62, 1), (71, 1), (67, 1), (62, 1), (65, 1), (62, 1), (59, 1), (60, 3)]
    bars = [(48, (52, 55)), (48, (52, 55)), (41, (57, 60)), (48, (52, 55)), (50, (53, 57)), (43, (50, 55)),
            (43, (50, 53)), (48, (52, 55))]
    notes, at = [], 0.0
    for key, beats in melody:
        notes.append((at, beats * beat * 0.95, key, 0.62))
        at += beats * beat
    points = [(0.0, 0.0), (0.05, 1.0)]
    for b, (root, (third, fifth)) in enumerate(bars):
        start = 3 * beat * b
        notes += [(start, 3 * beat, root, 0.5), (start + beat, beat, third, 0.38), (start + 2 * beat, beat, fifth, 0.38)]
        if b:
            points += [(start + 0.02, 1.0), (start + 0.03, 0.0), (start + 0.15, 0.0), (start + 0.17, 1.0)]
    points += [(24 * beat + 2.0, 1.0), (24 * beat + 2.05, 0.0)]
    pedal(64, points)
    section("The Old House tune, bars 1-8", notes, 24 * beat + 3.0)

    sweep = [12 * o + s for o in range(1, 9) for s in (0, 4, 7)]
    sweep = [k for k in sweep if 24 <= k <= 108]
    pedal(64, [(0.0, 0.0), (0.05, 1.0), (len(sweep) * 0.08 + 4.0, 1.0), (len(sweep) * 0.08 + 4.05, 0.0)])
    section("Sweep under the pedal", [(0.1 + 0.08 * i, 0.1, k, 0.6) for i, k in enumerate(sweep)], len(sweep) * 0.08 + 6.0)
    return sections, {c: sorted(p) for c, p in cc.items()}


def project_xml(sections, cc, tracks=("PianoCraft", "Other piano")):
    ids = map("id{}".format, itertools.count())

    def el(parent, tag, **attrs):
        return ET.SubElement(parent, tag, {k: str(v) for k, v in attrs.items()})

    root = ET.Element("Project", version="1.0")
    el(root, "Application", name="ab_project.py", version="1")
    transport = el(root, "Transport")
    el(transport, "Tempo", unit="bpm", value=60, id=next(ids), name="Tempo")
    el(transport, "TimeSignature", numerator=4, denominator=4, id=next(ids), name="Time Signature")
    structure = el(root, "Structure")
    arrangement = el(root, "Arrangement", id=next(ids))
    lanes = el(arrangement, "Lanes", timeUnit="beats", id=next(ids))
    markers = el(arrangement, "Markers", timeUnit="beats", id=next(ids))
    master = next(ids)
    for name in tracks:
        t = el(structure, "Track", contentType="notes", loaded="true", id=next(ids), name=name)
        ch = el(t, "Channel", audioChannels=2, solo="false", role="regular", destination=master, id=next(ids))
        el(ch, "Mute", value="false", id=next(ids), name="Mute")
        el(ch, "Pan", max=1, min=0, unit="normalized", value=0.5, id=next(ids), name="Pan")
        el(ch, "Volume", max=2, min=0, unit="linear", value=0.5, id=next(ids), name="Volume")
        track_lanes = el(lanes, "Lanes", track=t.get("id"), id=next(ids))
        clips = el(track_lanes, "Clips", id=next(ids))
        for title, start, end, notes in sections:
            clip = el(clips, "Clip", time=start, duration=round(end - start, 3), playStart=0.0, enable="true", name=title)
            ns = el(clip, "Notes", id=next(ids))
            for at, length, key, velocity in notes:
                el(ns, "Note", time=round(at - start, 4), duration=round(length, 4), channel=0, key=key, vel=velocity,
                   rel=0.5)
        for controller, points in cc.items():
            lane = el(track_lanes, "Points", unit="normalized", id=next(ids))
            el(lane, "Target", expression="channelController", channel=0, controller=controller)
            for at, v in points:
                el(lane, "RealPoint", time=at, value=v, interpolation="linear")
    t = el(structure, "Track", contentType="audio notes", loaded="true", id=next(ids), name="Master")
    ch = el(t, "Channel", audioChannels=2, solo="false", role="master", id=master)  # tracks route to its channel
    el(ch, "Volume", max=2, min=0, unit="linear", value=1.0, id=next(ids), name="Volume")
    for title, start, *_ in sections:
        el(markers, "Marker", time=start, name=title)
    return ET.tostring(root, encoding="UTF-8", xml_declaration=True)


def events(sections, cc):
    """Time-ordered lines 'seconds on|off|cc key-or-controller value' for an offline render."""
    rows = [(at, 1, f"on {key} {v}") for *_, notes in sections for at, length, key, v in notes]
    rows += [(at + length, 0, f"off {key} 0.5") for *_, notes in sections for at, length, key, v in notes]
    rows += [(at, 2, f"cc {c} {v}") for c, points in cc.items() for at, v in points]
    return "".join(f"{at:.4f} {text}\n" for at, _, text in sorted(rows))


def check():
    sections, cc = plan()
    for title, start, end, notes in sections:
        for at, length, key, v in notes:
            assert start <= at and at + length <= end + 1e-9, (title, at)
            assert 21 <= key <= 108 and 0 < v <= 1, (title, key, v)
    for points in cc.values():
        assert all(a <= b for (a, _), (b, _) in zip(points, points[1:])), points
        assert all(0.0 <= v <= 1.0 for _, v in points)
    root = ET.fromstring(project_xml(sections, cc))
    assert len(root.findall(".//Track")) == 3 and len(root.findall(".//Marker")) == len(sections)
    assert not root.findall(".//Clip/Points")  # CCs on the track, not in clips
    assert events(sections, cc).count(" on ") == sum(len(n) for *_, n in sections)
    print(f"ok: {len(sections)} sections, {sum(len(n) for *_, n in sections)} notes, {sections[-1][2]:.0f} s")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--check"]:
        check()
    elif args[:1] == ["--events"]:
        Path(args[1]).write_text(events(*plan()))
    else:
        out = Path(args[0] if args else Path(__file__).resolve().parent.parent / "renders/ab/ab-test.dawproject")
        out.parent.mkdir(parents=True, exist_ok=True)
        sections, cc = plan()
        meta = ET.Element("MetaData")
        ET.SubElement(meta, "Title").text = out.stem
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("project.xml", project_xml(sections, cc))
            z.writestr("metadata.xml", ET.tostring(meta, encoding="UTF-8", xml_declaration=True))
        listing = "".join(f"{start:6.1f} s  {title}\n" for title, start, *_ in sections)
        out.with_suffix(".txt").write_text(listing)
        print(f"wrote {out}\n{listing}{sections[-1][2]:.0f} s at 60 bpm")
