#!/usr/bin/env python3
"""Every hakios.* kernel source reaches the tree at bsd/dev/ or iokit/Kernel/ through a list that names it.

usage: ci/map-check.py [--root DIR]          exit 0 if the tree and drivers/map agree, 1 and the list of problems if not

WHY THIS EXISTS, AND WHAT IT ADDS TO tools/drivers-check.py

tools/drivers-check.py (CI, and the one to run when adding a driver) checks the common repository's files: drivers/<subsystem>/
against drivers/map, and that drivers/*/conf builds each of them. It is right about that and it is not repeated here.

This check is about the other three ways a hakios.* file gets into bsd/dev/ or iokit/Kernel/, none of which drivers-check.py
looks at, and each of which has bitten this port:

  xnu/files/           the new files that xnu/apply.sh copies over Apple's tree. A hakios.* driver that is here and not in
                       drivers/map is a file the build gets and nobody lists: xnu/export.sh refuses it, so it does not survive
                       the next export, but until that is run the tree has a driver no conf builds and no review covers.
  xnu/hakios.patch     the changes to Apple's own files. A new file there is laid at its path directly, and the map's rule
                       -- every hakios.* file of bsd/dev/ or iokit/Kernel/ is listed -- applies to it too.
  devices/*/drivers/   a SoC's or a device's own drivers, put in by tools/hakios-config.py external-files. They must NOT be in
    socs/*/drivers/    drivers/map (its header says why: there is no drivers/<subsystem>/ copy of them, so xnu/apply.sh stops
                       on "<path> is in drivers/map but missing"). They are checked the other way round: the path their conf
                       names is where apply.sh puts them, and the source file has to be there.

The four sources are collected and required to agree:

  - a path from drivers/map is in drivers/<subsystem>/<name> and is there; no path is listed twice; no two share a name
    (they would sit in one directory in the tree);
  - a path from a device's or SoC's conf is in that repository's drivers/<subsystem>/, exists, and is not in drivers/map;
  - a path from xnu/files/ or from xnu/hakios.patch is in drivers/map and is not a device's or SoC's own file.

A file sitting in drivers/<subsystem>/ and named in no map line is NOT reported here: tools/drivers-check.py's last rule is
exactly that ("every file under drivers/<subsystem>/ is listed"), it runs in the same CI, and reporting it twice would
only make the two outputs disagree about their counts.

The headers that xnu/files/bsd/sys/ holds (hakios.epd.h, hakios.input.h ...) are not drivers: they are the interfaces of
drivers and they are shared with user space, so they belong at bsd/sys/ and not in drivers/map. Only bsd/dev/ and
iokit/Kernel/ are a driver's home in Apple's tree, which is why TREE_DIRS is what it is.

It reads the tree only: no build, no git, and no import of the repository's tools.
"""
import os
import sys

TREE_DIRS = ("bsd/dev/", "iokit/Kernel/")
# The conf format of docs/drivers-config.md: a top-level `driver NAME` or `standard PATH`, and an indented `file PATH`
# under either. Only a `file` or `standard` line names a source; requires, default and help do not, so they are ignored.
SOURCE_WORDS = ("file", "standard")


def is_driver_path(path):
    """True for the paths drivers/map governs: bsd/dev/ or iokit/Kernel/, file name hakios.*."""
    return path.startswith(TREE_DIRS) and os.path.basename(path).startswith("hakios.")


def read_map(root):
    """[(subsystem, tree path, where)] from drivers/map, and the problems met on the way."""
    entries, problems = [], []
    path = os.path.join(root, "drivers", "map")
    if not os.path.isfile(path):
        return entries, ["drivers/map does not exist"]
    for number, line in enumerate(open(path, encoding="utf-8"), 1):
        text = line.split("#", 1)[0].strip()
        if not text:
            continue
        parts = text.split()
        if len(parts) != 2:
            problems.append("drivers/map:%d: want '<subsystem> <path>', got %r" % (number, text))
            continue
        sub, tree = parts
        if not is_driver_path(tree):
            problems.append("drivers/map:%d: %r is not a hakios.* file of bsd/dev/ or iokit/Kernel/" % (number, tree))
        entries.append((sub, tree, "drivers/map:%d" % number))
    return entries, problems


def files_tree(root):
    """[(tree path, file on disk, where)] of the new files xnu/apply.sh copies over Apple's tree."""
    out, torn = [], os.path.join(root, "xnu", "files")
    if not os.path.isdir(torn):
        return out
    for directory, _dirs, names in os.walk(torn):
        for name in sorted(names):
            path = os.path.join(directory, name)
            tree = os.path.relpath(path, torn).replace(os.sep, "/")
            if is_driver_path(tree):
                out.append((tree, path, "xnu/files/" + tree))
    return out


def patch_files(root):
    """[(tree path, where)] of the files xnu/hakios.patch adds (not the ones it edits).

    A git patch names the file in `diff --git a/X b/Y`, and says a file is new either with `new file mode ...` or with
    `--- /dev/null`; both are accepted, so a patch from a tool that writes only one of them is read as well. The node
    is reported at the `+++` line, which is where a reader looks.
    """
    out, path = [], os.path.join(root, "xnu", "hakios.patch")
    if not os.path.isfile(path):
        return out
    current, new, where = None, False, None
    for number, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        if line.startswith("diff --git "):
            if current and new:
                out.append((current, where))
            current, new, where = line.split(" b/", 1)[-1].rstrip("\n"), False, None
        elif current and line.startswith("+++ b/") and new:
            where = "xnu/hakios.patch:%d" % number
        elif current and (line.startswith("new file mode") or line.startswith("--- /dev/null")):
            new = True
    if current and new:
        out.append((current, where or "xnu/hakios.patch (end)"))
    return [(tree, where) for tree, where in out if is_driver_path(tree)]


def repositories(root):
    """[(directory)] of the SoC and device repositories checked out here (the layers of docs/porting/repositories.md)."""
    out = []
    for top, marker in (("socs", "soc.conf"), ("devices", "device.conf")):
        base = os.path.join(root, top)
        for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
            if os.path.isfile(os.path.join(base, name, marker)):
                out.append(os.path.join(base, name))
    return out


def external_files(root):
    """[(tree path, source file, where)] that a device's or SoC's drivers/<subsystem>/conf names, which is what
    tools/hakios-config.py external-files prints for xnu/apply.sh to copy."""
    out = []
    for top in repositories(root):
        drivers = os.path.join(top, "drivers")
        if not os.path.isdir(drivers):
            continue
        for sub in sorted(os.listdir(drivers)):
            conf = os.path.join(drivers, sub, "conf")
            if not os.path.isfile(conf):
                continue
            shown = os.path.relpath(conf, root).replace(os.sep, "/")
            for number, line in enumerate(open(conf, encoding="utf-8"), 1):
                words = line.split("#", 1)[0].split()
                if len(words) < 2 or words[0] not in SOURCE_WORDS:
                    continue
                tree = words[1]
                source = os.path.join(drivers, sub, os.path.basename(tree))
                out.append((tree, source, "%s:%d" % (shown, number)))
    return out


def check(root):
    entries, problems = read_map(root)
    listed, by_name = {}, {}
    for sub, tree, where in entries:
        name = os.path.basename(tree)
        local = os.path.join(root, "drivers", sub, name)
        if tree in listed:
            problems.append("%s: %s is listed twice (also %s)" % (where, tree, listed[tree][1]))
            continue
        listed[tree] = (sub, where)
        if not os.path.isfile(local):
            problems.append("%s: %s is in drivers/map as drivers/%s/%s, which is not there" % (where, tree, sub, name))
        if name in by_name and by_name[name] != tree:
            problems.append("%s: %s collides with %s: both would be %s in one directory of the tree" % (
                where, tree, by_name[name], name))
        by_name[name] = tree

    external = external_files(root)
    external_paths = {}
    for tree, source, where in external:
        external_paths[tree] = source
        if not is_driver_path(tree):
            problems.append("%s: %s is not a hakios.* file of bsd/dev/ or iokit/Kernel/" % (where, tree))
        if not os.path.isfile(source):
            problems.append("%s: names %s, and %s is not there" % (where, tree, os.path.relpath(source, root)))
        if tree in listed:
            problems.append("%s: %s is a device's or SoC's own file and is also in drivers/map (%s): drivers/map keeps "
                            "only this repository's drivers, and xnu/apply.sh would look for drivers/%s/%s and stop" % (
                                where, tree, listed[tree][1], listed[tree][0], os.path.basename(tree)))

    for tree, where in [(t, w) for t, _p, w in files_tree(root)] + patch_files(root):
        if tree in external_paths:
            problems.append("%s: %s is also a device's or SoC's own file (%s): one of the two places has to give it up" % (
                where, tree, external_paths[tree]))
        elif tree not in listed:
            problems.append("%s: %s reaches bsd/dev/ or iokit/Kernel/ and drivers/map does not list it (add "
                            "'<subsystem> %s', or make it a device's own file in its repository's conf)" % (where, tree, tree))
    return entries, external, problems


def main(argv):
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    if len(argv) == 3 and argv[1] == "--root":
        root = argv[2]
    root = os.path.abspath(root)
    entries, external, problems = check(root)
    if problems:
        for p in problems:
            print("map-check: " + p)
        print("map-check: %d problem(s)" % len(problems))
        return 1
    print("map-check: %d files in drivers/map, %d from a device's or SoC's conf, each listed once and each there" % (
        len(entries), len(external)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
