#!/usr/bin/env python3
"""qemu/epd.py's waveform table and devices/b651/WAVEFORMS.md say the same numbers.

usage: ci/waveform-check.py [--root DIR]         exit 0 if they agree, 1 and the disagreement if not

WHY THIS EXISTS

qemu/epd.py's MODES -- frames to settle, whether the waveform flashes, how much residue it leaves -- are the numbers the
e-ink viewer draws with, and devices/b651/WAVEFORMS.md section 3 is the document that says which of them are real, which
are borrowed and which are invented. The doc is the ground truth ("Ground truth for the e-ink model in qemu/epd.py",
its first line) and the code is what a reader actually sees. Between the two, nothing was checked, and that is the drift
that matters here: WAVEFORMS.md's rows carry the code's numbers *and* the `qemu/epd.py:<line>` they came from, so a
number changed in the model leaves the doc quoting an old number at an old line, and a reader has no way to tell.

So this reads both and requires, for each waveform the model has:

  - the doc's 3.1 row (frames), 3.2 row (flash) and 3.3 row (residue) carry the model's value;
  - the doc's 3.1 `site` names the line of qemu/epd.py where that mode's entry is (a `file:line` that has moved is a
    citation that lies);
  - the doc names no waveform the model does not have, and the model has no waveform the doc does not describe.

The model is read as source, with ast, not imported: importing qemu/epd.py needs Pillow and the whole viewer, and a
number in a dict literal is readable without either.

Only the numbers are compared, exactly as WAVEFORMS.md 3.1-3.3 state them. Whether a number is right is not the
question -- every one of them is ASSUMED until the panel's waveform blob is read (WAVEFORMS.md section 5) -- only that
the two files say the same thing.
"""
import ast
import os
import re
import sys

MODEL = os.path.join("qemu", "epd.py")
DOC = os.path.join("devices", "b651", "WAVEFORMS.md")

# The columns of the doc's tables: | mode | status | value | site |. Section 3.1 has the site in the fourth cell,
# 3.2 and 3.3 in the fourth as well; the sections are told apart by their heading number.
SECTION = re.compile(r"^###\s+(\d+\.\d+)\s")
WAVEFORMS = ("gc16", "gl16", "du", "a2")
# "unknown; model uses 24", "unknown; model uses 2": the model's value is the last integer of the cell
MODEL_USES = re.compile(r"model uses\s+([0-9]+)")
# the site cell, `qemu/epd.py:48`
SITE = re.compile(r"`?%s:([0-9]+)`?" % re.escape(MODEL))


def model_modes(root):
    """{mode: (frames, flashes, residue, line)} from qemu/epd.py's MODES literal, and the problems met reading it."""
    path = os.path.join(root, MODEL)
    if not os.path.isfile(path):
        return {}, ["%s does not exist" % MODEL]
    try:
        tree = ast.parse(open(path, encoding="utf-8").read(), filename=MODEL)
    except (OSError, SyntaxError) as e:
        return {}, ["%s does not parse: %s" % (MODEL, e)]
    found = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == "MODES" for t in node.targets):
            continue
        if not isinstance(node.value, ast.Dict):
            continue
        for key, value in zip(node.value.keys, node.value.values):
            try:
                mode, entry = ast.literal_eval(key), ast.literal_eval(value)
            except ValueError:
                continue                     # a computed key or value: not this check's to read
            if not isinstance(entry, tuple) or len(entry) != 3:
                continue
            found[mode] = (entry[0], entry[1], entry[2], key.lineno)
    if not found:
        return {}, ["%s: no MODES = { mode: (frames, flashes, residue) } literal" % MODEL]
    return found, []


def doc_rows(root):
    """({mode: {section: (cells, line)}}) of WAVEFORMS.md's 3.1, 3.2 and 3.3 tables."""
    path = os.path.join(root, DOC)
    if not os.path.isfile(path):
        return {}, ["%s does not exist" % DOC]
    rows, section = {}, None
    for number, line in enumerate(open(path, encoding="utf-8"), 1):
        heading = SECTION.match(line)
        if heading:
            section = heading.group(1)
            continue
        if section not in ("3.1", "3.2", "3.3") or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 4 or cells[0] not in WAVEFORMS:
            continue
        rows.setdefault(cells[0], {})[section] = (cells, number)
    return rows, []


def frames_of(cell):
    """The model's frame count from a 3.1 value cell, or None if the cell does not state one."""
    m = MODEL_USES.search(cell)
    return int(m.group(1)) if m else None


def flash_of(cell):
    """The model's flash from a 3.2 value cell: True, False, or None if the cell does not state one."""
    word = cell.strip("*").strip().lower()
    if word == "flashes":
        return True
    if word == "does not flash":
        return False
    return None


def residue_of(cell):
    """The model's residue from a 3.3 value cell: a float, or None if the cell does not state one."""
    m = re.match(r"[0-9]*\.[0-9]+", cell.strip("*").strip())
    return float(m.group(0)) if m else None


def compare(root):
    """(the model's modes, the doc's rows, the problems): the two read, and where they disagree."""
    modes, problems = model_modes(root)
    rows, doc_problems = doc_rows(root)
    problems += doc_problems

    for mode, (frames, flashes, residue, line) in sorted(modes.items()):
        sections = rows.get(mode, {})
        for section, want in (("3.1", frames_of), ("3.2", flash_of), ("3.3", residue_of)):
            row = sections.get(section)
            if row is None:
                problems.append("%s section %s has no %s row: every waveform the model has is described there (add one, "
                                "or take the mode out of %s's MODES)" % (DOC, section, mode, MODEL))
                continue
            cells, doc_line = row
            got = want(cells[2])
            if got is None:
                problems.append("%s:%d: the %s row of %s does not state the model's value (want 'model uses N', "
                                "'flashes'/'does not flash', or a decimal)" % (DOC, doc_line, section, mode))
                continue
            mine = {"3.1": frames, "3.2": flashes, "3.3": residue}[section]
            if got != mine:
                what = {"3.1": "frames", "3.2": "flash", "3.3": "residue"}[section]
                problems.append("%s differs on %s, %s" % (DOC, mode, what))
                problems.append("    - %-6s %s" % (mode, cells[2]))
                problems.append("    + %-6s %s (%s:%d)" % (mode, mine, MODEL, line))
        cited = sections.get("3.1")
        if cited is not None:
            m = SITE.search(cited[0][3] if len(cited[0]) > 3 else "")
            if not m:
                problems.append("%s:%d: the 3.1 row of %s cites no %s:<line>, so nothing ties it to the model" % (
                    DOC, cited[1], mode, MODEL))
            elif int(m.group(1)) != line:
                problems.append("%s:%d: the 3.1 row of %s cites %s:%s, and the mode is on %s:%d" % (
                    DOC, cited[1], mode, MODEL, m.group(1), MODEL, line))
                problems.append("    - %s:%s" % (MODEL, m.group(1)))
                problems.append("    + %s:%d" % (MODEL, line))

    for mode, sections in sorted(rows.items()):
        if mode not in modes and any(s in sections for s in ("3.1", "3.2", "3.3")):
            problems.append("%s describes %s, and %s's MODES has no such waveform: the doc describes a mode the "
                            "model does not draw" % (DOC, mode, MODEL))
    return modes, rows, problems


def main(argv):
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    if len(argv) == 3 and argv[1] == "--root":
        root = argv[2]
    root = os.path.abspath(root)
    modes, rows, problems = compare(root)
    if problems:
        for p in problems:
            print("waveform-check: " + p)
        print("waveform-check: %d problem(s)" % len(problems))
        return 1
    print("waveform-check: %d waveforms of %s agree with %s sections 3.1, 3.2 and 3.3, and the line cited for each is "
          "where it is" % (len(modes), MODEL, DOC))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
