#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws docs/midi-template.svg: the TRIMFX template on a nanoKONTROL Studio layout."""
import sys
from xml.sax.saxutils import escape

KNOB = ["P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"]
FADER = ["TONE MIX", "MOD MIX", "DLY MIX", "VERB MIX", "IN", "DRY", "OUT", "TEMPO"]
SOLO = ["TONE ON", "MOD ON", "DLY ON", "VERB ON", "FRZ DLY", "FRZ VERB", "HOLD", "LATCH"]
MUTE = ["TONE ALG-", "MOD ALG-", "DLY ALG-", "VERB ALG-", "ORDER-", "MATRIX", "", ""]
REC = ["TONE ALG+", "MOD ALG+", "DLY ALG+", "VERB ALG+", "ORDER+", "", "", ""]
SEL = ["PG TONE", "PG MOD", "PG DLY", "PG VERB", "PG MASTER", "", "", ""]
CC = dict(knob=21, fader=31, solo=41, mute=51, rec=61, sel=71)
TRANSPORT = [("TRACK <", 81, "PAGE <"), ("TRACK >", 82, "PAGE >"), ("MARKER <", 83, "PRESET <"),
             ("MARKER >", 84, "PRESET >"), ("SET", 85, "TAP"), ("CYCLE", 86, "FOCUS/MATRIX"),
             ("REW", 87, "ALGO <"), ("FF", 88, "ALGO >"), ("PLAY", 89, "FREEZE ALL"),
             ("STOP", 90, "PAGE ON/OFF"), ("REC", 91, "SAVE"), ("JOG", 92, "TURN SELECTED")]

out = []
W, H = 1180, 560
e = out.append
e(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="Helvetica,Arial,sans-serif">')
e(f'<rect width="{W}" height="{H}" rx="14" fill="#1b1c1f"/>')
e('<text x="24" y="38" fill="#e9eaeb" font-size="22" font-weight="bold" letter-spacing="3">TRIMFX</text>')
e('<text x="140" y="38" fill="#ff9a1f" font-size="14">nanoKONTROL Studio template - scene 1 = ch 1 knobs follow the page; '
  'scenes 2-5 = ch 2-5 knobs = TONE / MOD / DELAY / REVERB</text>')
for i in range(8):
    x = 30 + i * 92
    def btn(y, label, cc, color):
        e(f'<rect x="{x}" y="{y}" width="78" height="34" rx="5" fill="{color}"/>')
        e(f'<text x="{x+39}" y="{y+15}" fill="#111" font-size="11" text-anchor="middle" font-weight="bold">{label}</text>')
        e(f'<text x="{x+39}" y="{y+28}" fill="#333" font-size="10" text-anchor="middle">CC {cc}</text>')
    e(f'<circle cx="{x+39}" cy="92" r="26" fill="#0d0e10" stroke="#555"/>')
    e(f'<text x="{x+39}" y="97" fill="#ff9a1f" font-size="15" text-anchor="middle" font-weight="bold">{KNOB[i]}</text>')
    e(f'<text x="{x+39}" y="134" fill="#9a9ca0" font-size="10" text-anchor="middle">CC {CC["knob"]+i}</text>')
    btn(146, SOLO[i] or "-", CC["solo"] + i, "#d9dadb")
    btn(186, MUTE[i] or "-", CC["mute"] + i, "#d9dadb" if MUTE[i] else "#77797c")
    btn(226, REC[i] or "-", CC["rec"] + i, "#d9dadb" if REC[i] else "#77797c")
    btn(266, SEL[i] or "-", CC["sel"] + i, "#ffcf8a" if SEL[i] else "#77797c")
    e(f'<rect x="{x+33}" y="316" width="12" height="170" rx="4" fill="#0d0e10"/>')
    e(f'<rect x="{x+21}" y="380" width="36" height="22" rx="3" fill="#c4c6c7"/>')
    e(f'<text x="{x+39}" y="506" fill="#e9eaeb" font-size="12" text-anchor="middle" font-weight="bold">{FADER[i]}</text>')
    e(f'<text x="{x+39}" y="522" fill="#9a9ca0" font-size="10" text-anchor="middle">CC {CC["fader"]+i}</text>')
for r, txt in enumerate(["SOLO", "MUTE", "REC", "SELECT"]):
    e(f'<text x="22" y="{168+40*r}" fill="#9a9ca0" font-size="9" text-anchor="end" transform="rotate(-90 22 {168+40*r})">{txt}</text>')
for k, (name, cc, what) in enumerate(TRANSPORT):
    x, y = 790 + (k % 3) * 128, 70 + (k // 3) * 74
    e(f'<rect x="{x}" y="{y}" width="116" height="60" rx="6" fill="#2a2c31" stroke="#444"/>')
    e(f'<text x="{x+58}" y="{y+18}" fill="#9a9ca0" font-size="11" text-anchor="middle">{escape(name)}  CC {cc}</text>')
    e(f'<text x="{x+58}" y="{y+40}" fill="#ff9a1f" font-size="13" text-anchor="middle" font-weight="bold">{escape(what)}</text>')
e('<text x="790" y="390" fill="#9a9ca0" font-size="12">Buttons: Momentary, on 127 / off 0.</text>')
e('<text x="790" y="410" fill="#9a9ca0" font-size="12">Jog: relative, 1 = right, 127 = left.</text>')
e('<text x="790" y="430" fill="#9a9ca0" font-size="12">Knobs, faders: CC, 0 - 127.</text>')
e('<text x="790" y="458" fill="#9a9ca0" font-size="12">P1-P4 are the stick parameters;</text>')
e('<text x="790" y="476" fill="#9a9ca0" font-size="12">P5-P8 the small knobs on each box.</text>')
e('<text x="790" y="504" fill="#9a9ca0" font-size="12">Anything else: SETUP &gt; MIDI LEARN.</text>')
e('</svg>')
open(sys.argv[1] if len(sys.argv) > 1 else "docs/midi-template.svg", "w").write("\n".join(out) + "\n")
