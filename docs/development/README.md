# Development notes

These are the notes the program was ported with. Exbit was first a Python program (version 0.4,
then the Exbit-Linux rolling release); this C++ program is a recreation of it. The notes refer to
the Python sources (`daw/…`, `main.py`), which are in this repository's history (before the commit
that made the C++ program the project's root), and to a clone of Exbit-Linux used while porting.

- `PORTING.md` — rules and layout for the port
- `UI_PORTING.md` — how the Qt screens map onto our toolkit
- `API_NOTES.md`, `INTEGRATION.md` — the modules' APIs and how the main window wires them
- `FEATURES_LINUX.md` — the Exbit-Linux features and who ported what
