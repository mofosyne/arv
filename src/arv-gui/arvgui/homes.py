"""Finding the home catalogue: --home, $ARV_HOME, a .arv folder or pointer file, the machine config.

The archive keeps its catalogue in a `.arv` folder beside the files it describes (at the root of
the everyday tree, above any git or backup repositories), never inside them. Lookup order, the
first that answers wins:

1. ``--home PATH``, or ``--archive NAME`` (a name from the machine config);
2. ``$ARV_HOME``;
3. walking up from the folder being archived (`arv make FOLDER`), then from the current folder:
     ``.arv/`` folder       the catalogue itself;
     ``.arv`` file          a pointer: one line ``Home: PATH`` (relative to the file's folder),
                            for a second tree that belongs to the same archive;
     ``catalog.rec`` and ``catalog/archive.rec``  the root of an archive disc (read in place);
   git's ``.git`` does not stop the walk;
4. the machine config ``$XDG_CONFIG_HOME/arv/homes.rec``: its ``Default: yes`` home, or its only one.

None of these: there is no archive, and nothing makes one unasked (as git: ``arv init``).

Paths are machine-local, so the machine config never travels on discs; deleting it loses
nothing (the walk-up and --home still work).
"""

import os

from . import recfile

FOLDER = ".arv"

HOME_DESCRIPTOR = recfile.Record("Home", [
    ("%rec", "Home"),
    ("%doc", "Archive homes on this machine (paths are local; this file never goes on a disc).\n"
             "Name is what --archive takes; Default: yes picks the home used outside any .arv tree."),
    ("%key", "Name"),
    ("%mandatory", "Name Path"),
    ("%type", "Default enum yes no"),
])


def config_path():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "arv", "homes.rec")


def configured():
    """Home records from the machine config ([] when there is none)."""
    path = config_path()
    if not os.path.exists(path):
        return []
    return [r for r in recfile.read(path) if not r.is_descriptor and r.type == "Home"]


def register(name, path, default=False):
    """Add or update a home in the machine config."""
    homes = configured()
    path = os.path.abspath(path)
    rec = next((h for h in homes if h.get("Name") == name), None)
    if rec is None:
        rec = recfile.Record("Home", [("Name", name), ("Path", path)])
        homes.append(rec)
    rec.set("Path", path)
    if default:
        for h in homes:
            h.fields = [(k, v) for k, v in h.fields if k != "Default"]
        rec.add("Default", "yes")
    os.makedirs(os.path.dirname(config_path()), exist_ok=True)
    recfile.write(config_path(), [HOME_DESCRIPTOR] + homes)
    return rec


def read_pointer(path):
    """The home a `.arv` pointer file names, resolved against the file's folder."""
    with open(path, encoding="utf-8") as f:
        records = recfile.parse(f.read())
    target = next((r.get("Home") for r in records if r.get("Home")), None)
    if not target:
        raise SystemExit("Error: %s is a .arv pointer file without a 'Home: PATH' line" % path)
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(path)), os.path.expanduser(target)))


def write_pointer(folder, home):
    path = os.path.join(folder, FOLDER)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# This tree belongs to the archive whose catalogue is here (see `arv where`):\n")
        f.write("Home: %s\n" % home)
    return path


def is_disc_root(folder):
    return (os.path.isfile(os.path.join(folder, "catalog.rec"))
            and os.path.isfile(os.path.join(folder, "catalog", "archive.rec")))


def walk_up(start=None):
    """(home, how) from the nearest .arv folder, .arv pointer or disc root above ``start``; else None."""
    folder = os.path.abspath(start or os.getcwd())
    while True:
        candidate = os.path.join(folder, FOLDER)
        if os.path.isdir(candidate):
            return candidate, "the .arv folder in %s" % folder
        if os.path.isfile(candidate):
            return read_pointer(candidate), "the pointer file %s" % candidate
        if is_disc_root(folder):
            return os.path.join(folder, "catalog"), "the archive disc at %s" % folder
        parent = os.path.dirname(folder)
        if parent == folder:
            return None
        folder = parent


NO_HOME = ("arv: no archive here or in any folder above, and no default archive on this machine.\n"
           "  arv init FOLDER                          make one at the root of what it describes (e.g. /nas)\n"
           "  arv init FOLDER --name NAME --default    and use it from anywhere on this machine\n"
           "  (--home HOME or $ARV_HOME names one for a single run)")


def find(home=None, archive=None, start=None, source=None):
    """(home path, how it was found), following the order in the module docstring.

    ``source`` is a folder being archived: the walk starts there first, so `arv make ~/photos`
    finds the .arv above ~/photos wherever it is run from."""
    if home:
        return home, "--home"
    if archive:
        rec = next((h for h in configured() if h.get("Name") == archive), None)
        if rec is None:
            raise SystemExit("Error: no home named %s in %s (`arv init --name %s` adds one)"
                             % (archive, config_path(), archive))
        return rec.get("Path"), "--archive %s (%s)" % (archive, config_path())
    if os.environ.get("ARV_HOME"):
        return os.environ["ARV_HOME"], "$ARV_HOME"
    found = (source and os.path.isdir(source) and walk_up(source)) or walk_up(start)
    if found:
        return found
    homes = configured()
    default = [h for h in homes if (h.get("Default") or "").lower() == "yes"] or (homes if len(homes) == 1 else [])
    if default:
        return default[0].get("Path"), "the default home in %s" % config_path()
    raise SystemExit(NO_HOME)
