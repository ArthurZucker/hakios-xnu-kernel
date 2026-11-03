#!/usr/bin/env python3
"""The panel's geometry is one fact, and every place that states it states the same one.

usage: ci/geometry-check.py [--root DIR]         exit 0 if all of them agree, 1 and the disagreement if not

WHY THIS EXISTS

1125x2436 is the mode the loader sets and XNU draws at, and it is copied into at least four places: qemu/epd.py's
PANEL_W/PANEL_H (the e-ink model's idea of the panel), qemu/b651_epdc.c's B651_EPDC_PANEL_WIDTH/HEIGHT (the device
the guest actually drives), and the documents that explain them. A crop, a scale factor or a residency register that
works at one size and not the other is not a crash -- it is a picture with its right edge missing, which SIM.md:132-134
records happening once already ("cropping captures to [824x1648] sliced the right edge off every console line"). So the
one number is pinned here.

WHAT IT READS, AND WHAT IT CANNOT FIND IN EPDC.md

The document this check was asked for is devices/b651/EPDC.md, and it states no panel geometry: no "1125", no "2436",
no `PANEL_W`, no width/height row (grep -c 1125 devices/b651/EPDC.md prints 0). It is a scoping document for the
controller -- what an EPDC is, what must be reverse-engineered -- and the geometry lives in the two places that describe
the panel as it is drawn: devices/b651/WAVEFORMS.md section 3.6 ("panel geometry | ... | 1125x2436") and
devices/b651/SIM.md ("PANEL_W, PANEL_H = 1125, 2436"). Rather than fail on a correct tree because the brief named the
wrong file, this reads EPDC.md too if it ever grows a statement, and reads the two that have one now. The success line
says which files it read, so the gap is visible instead of assumed.

A "statement" is one of two determinate forms, which is how every file here writes it:
  - `PANEL_W, PANEL_H = 1125, 2436`                        (qemu/epd.py:43, and quoted in SIM.md:134 and :215)
  - a Markdown table row whose first cell is "panel geometry", with the size in it (WAVEFORMS.md section 3.6)
and in C, `#define B651_EPDC_PANEL_WIDTH 1125` with `..._HEIGHT 2436` as a pair.

Prose is deliberately not read. SIM.md:132 says "The panel is 1125×2436, not the 824×1648 `screencap` reports" -- the
second number is the contrast the sentence exists to make, and a regex that took every WxH off a line mentioning the
panel would call that a disagreement with itself. A check that has to guess is worse than one that reads less, so the
two forms above are what is compared, and the output names them.

Read-only, no build, no imports beyond the standard library.
"""
import ast
import os
import re
import sys

MODEL = os.path.join("qemu", "epd.py")
EPDC = os.path.join("devices", "b651", "EPDC.md")
# The documents that state the geometry besides EPDC.md: read for the statement, and named in the output.
DOCS = (EPDC, os.path.join("devices", "b651", "WAVEFORMS.md"), os.path.join("devices", "b651", "SIM.md"))
# The second copy in code, which the guest's framebuffer size and the residency registers are computed from.
DEVICE = os.path.join("qemu", "b651_epdc.c")

# `PANEL_W, PANEL_H = 1125, 2436`, in code or quoted in a document
ASSIGNMENT = re.compile(r"PANEL_W\s*,\s*PANEL_H\s*=\s*([0-9]+)\s*,\s*([0-9]+)")
# a Markdown table row whose first cell is "panel geometry": | panel geometry | ... | 1125x2436 | ...
GEOMETRY_ROW = re.compile(r"^\|\s*panel\s+geometry\s*\|", re.I)
# the size in it, "1125x2436" / "1125 x 2436" / "1125×2436". Three digits at least on each side, so a hex literal
# (0x54) or a register number is not read as a size.
GEOMETRY = re.compile(r"(?<![0-9A-Za-z])([0-9]{3,5})\s*[x×]\s*([0-9]{3,5})(?![0-9A-Za-z])")
# the C pair
DEFINE_W = re.compile(r"^\s*#define\s+\w*PANEL_WIDTH\s+([0-9]+)")
DEFINE_H = re.compile(r"^\s*#define\s+\w*PANEL_HEIGHT\s+([0-9]+)")


def model_geometry(root):
    """(width, height, where) from qemu/epd.py's `PANEL_W, PANEL_H = ...`, and the problems met reading it."""
    path = os.path.join(root, MODEL)
    if not os.path.isfile(path):
        return None, ["%s does not exist" % MODEL]
    try:
        tree = ast.parse(open(path, encoding="utf-8").read(), filename=MODEL)
    except (OSError, SyntaxError) as e:
        return None, ["%s does not parse: %s" % (MODEL, e)]
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.Tuple):
            continue
        names = [t.id for t in node.targets[0].elts if isinstance(t, ast.Name)] if isinstance(node.targets[0], ast.Tuple) else []
        if names != ["PANEL_W", "PANEL_H"] or len(node.value.elts) != 2:
            continue
        try:
            return (ast.literal_eval(node.value.elts[0]), ast.literal_eval(node.value.elts[1]),
                    "%s:%d" % (MODEL, node.lineno)), []
        except ValueError:
            return None, ["%s:%d: PANEL_W, PANEL_H is not two literals" % (MODEL, node.lineno)]
    return None, ["%s: no `PANEL_W, PANEL_H = <w>, <h>` assignment" % MODEL]


def doc_statements(root, shown):
    """[(width, height, where)] that a document states, so that each can be held against the model.

    Both determinate forms are looked for in every document: the assignment (SIM.md quotes it twice, qemu/epd.py is the
    original) and the "panel geometry" table row (WAVEFORMS.md 3.6). Anything else -- a captured screen's "800x600", the
    "824x1648" a sentence names to rule it out -- is not a claim about what the panel is, and is not read.
    """
    out = []
    path = os.path.join(root, shown)
    if not os.path.isfile(path):
        return out
    for number, line in enumerate(open(path, encoding="utf-8"), 1):
        for m in ASSIGNMENT.finditer(line):
            out.append((int(m.group(1)), int(m.group(2)), "%s:%d" % (shown, number)))
        if not GEOMETRY_ROW.match(line):
            continue
        for m in GEOMETRY.finditer(line):
            out.append((int(m.group(1)), int(m.group(2)), "%s:%d" % (shown, number)))
    return out


def device_geometry(root):
    """(width, height, where) from qemu/b651_epdc.c's B651_EPDC_PANEL_WIDTH/HEIGHT, or None.

    Both halves are needed: a file with only one of them is a problem, not a statement, so it is reported by the caller.
    """
    path = os.path.join(root, DEVICE)
    if not os.path.isfile(path):
        return None, []
    width = height = None
    where = None
    for number, line in enumerate(open(path, encoding="utf-8"), 1):
        w, h = DEFINE_W.match(line), DEFINE_H.match(line)
        if w:
            width, where = int(w.group(1)), "%s:%d" % (DEVICE, number)
        if h:
            height = int(h.group(1))
    if width is None or height is None:
        return None, ["%s: no B651_EPDC_PANEL_WIDTH and B651_EPDC_PANEL_HEIGHT pair" % DEVICE]
    return (width, height, where), []


def compare(root):
    """(statements, problems): every statement of the geometry, and where they disagree with the model."""
    problems, statements = [], []
    model, model_problems = model_geometry(root)
    problems += model_problems
    if model is None:
        return statements, problems
    statements.append(("model", model))
    for shown in DOCS:
        for w, h, where in doc_statements(root, shown):
            statements.append(("doc", (w, h, where)))
    device, device_problems = device_geometry(root)
    problems += device_problems
    if device is not None:
        statements.append(("device", device))

    want = (model[0], model[1])
    for kind, (w, h, where) in statements:
        if (w, h) == want:
            continue
        if kind == "model":
            continue                         # the model is what the others are held against
        problems.append("%s states %dx%d, and %s has %dx%d" % (where, w, h, model[2], want[0], want[1]))
        problems.append("    - %-40s %dx%d" % (where, w, h))
        problems.append("    + %-40s %dx%d" % (model[2], want[0], want[1]))
    if not any(kind == "doc" for kind, _s in statements):
        problems.append("no document states the panel geometry: EPDC.md, WAVEFORMS.md and SIM.md were read, and none "
                        "has a `PANEL_W, PANEL_H = ...` or a line naming the panel geometry, so nothing pins %s" % model[2])
    return statements, problems


def main(argv):
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    if len(argv) == 3 and argv[1] == "--root":
        root = argv[2]
    root = os.path.abspath(root)
    statements, problems = compare(root)
    if problems:
        for p in problems:
            print("geometry-check: " + p)
        print("geometry-check: %d problem(s)" % len(problems))
        return 1
    docs = [where for kind, (_w, _h, where) in statements if kind == "doc"]
    print("geometry-check: %dx%d in %s, and the same in %s" % (
        statements[0][1][0], statements[0][1][1], statements[0][1][2], ", ".join(docs)))
    # Say what was read and what it did not find, rather than assuming it: if EPDC.md grows a statement it is compared
    # above like any other, and this line stops claiming it has none.
    in_epdc = [where for kind, (_w, _h, where) in statements if kind == "doc" and where.startswith(EPDC + ":")]
    if in_epdc:
        print("geometry-check: %s states it too, at %s, and agrees" % (EPDC, ", ".join(in_epdc)))
    else:
        print("geometry-check: %s states no panel geometry (it is the controller's scoping document, not the panel's); "
              "the statements compared are the ones above" % EPDC)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
