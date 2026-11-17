#!/usr/bin/env python3
# skins_reference.py: writes the variables part of docs/skins.md from data/skins/default.css (its :root
# variables, each with the comment beside it), between the markers
#     <!-- variables: made by tools/skins_reference.py -->  …  <!-- end of variables -->
# so the reference never says other than the stylesheet. Run it after changing default.css's variables:
#     python3 tools/skins_reference.py            (tests/test_ui_dialogs.cpp checks it is up to date)
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSS = os.path.join(ROOT, "data", "skins", "default.css")
DOC = os.path.join(ROOT, "docs", "skins.md")
START = "<!-- variables: made by tools/skins_reference.py -->"
END = "<!-- end of variables -->"

VAR = re.compile(r"^\s*(--[a-z0-9-]+)\s*:\s*(.*?);\s*(?:/\*\s*(.*?)\s*\*/)?\s*$")
GROUP = re.compile(r"^\s*/\*\s*([^*]+?)\s*\*/\s*$")
SECTION = re.compile(r"^/\* =+ (.+?) =+")


def cell(text):
    return text.replace("|", "\\|")


def read_tables(css):
    """[(title, [(name, value, comment)])]: the first :root's groups (/* Window */…), then each later :root
    under the section it is in"""
    tables, current, section, depth, in_root, first_root = [], None, "", 0, False, True
    for line in css.splitlines():
        s = SECTION.match(line)
        if s:
            section = s.group(1).strip()
        if not in_root and line.startswith(":root"):
            in_root = True
            if not first_root:
                current = (section + " sizes", [])
                tables.append(current)
            continue
        if in_root:
            if line.startswith("}"):
                in_root, first_root = False, False
                continue
            g = GROUP.match(line)
            if first_root and g:
                title = g.group(1)
                title = "Shades" if title.startswith("shades") else title
                current = (title, [])
                tables.append(current)
                continue
            v = VAR.match(line)
            if v and v.group(1) == "--mono-font":  # (with the fonts)
                fonts = next((t for t in tables if t[0] == "Fonts"), None)
                if fonts is None:
                    fonts = ("Fonts", [])
                    tables.insert(1 if tables else 0, fonts)
                fonts[1].append((v.group(1), v.group(2), v.group(3) or ""))
            elif v:
                if current is None:
                    current = ("The skin", [])
                    tables.append(current)
                current[1].append((v.group(1), v.group(2), v.group(3) or ""))
            elif first_root and re.match(r"^\s*(font-family|font-size)\s*:", line):
                prop, value = line.strip().rstrip(";").split(":", 1)
                value = value.split("/*")[0].strip().rstrip(";")
                note = re.search(r"/\*\s*(.*?)\s*\*/", line)
                fonts = next((t for t in tables if t[0] == "Fonts"), None)
                if fonts is None:
                    fonts = ("Fonts", [])
                    tables.insert(1 if tables else 0, fonts)
                fonts[1].append((prop.strip(), value, note.group(1) if note else ""))
    return [t for t in tables if t[1]]


def markdown(tables):
    out = [START, ""]
    for title, rows in tables:
        out.append("### " + title)
        out.append("")
        out.append("| Name | Default | What it is |")
        out.append("|---|---|---|")
        for name, value, comment in rows:
            out.append("| `%s` | `%s` | %s |" % (name, cell(value), cell(comment)))
        out.append("")
    out.append(END)
    return "\n".join(out)


def main():
    with open(CSS, encoding="utf-8") as f:
        tables = read_tables(f.read())
    with open(DOC, encoding="utf-8") as f:
        doc = f.read()
    if START not in doc or END not in doc:
        sys.exit("docs/skins.md has no variables markers")
    a, b = doc.index(START), doc.index(END) + len(END)
    new = doc[:a] + markdown(tables) + doc[b:]
    if "--check" in sys.argv:
        sys.exit(0 if new == doc else "docs/skins.md is out of date: run tools/skins_reference.py")
    if new != doc:
        with open(DOC, "w", encoding="utf-8") as f:
            f.write(new)
    print("%d variables in %d tables" % (sum(len(r) for _, r in tables), len(tables)))


if __name__ == "__main__":
    main()
