# MeshSimplify init.py - runs in every Nuke mode (GUI, -t, -ti, farm).
# The compiled plugin is version-specific, so binaries live in per-version
# subfolders (14.1/, 15.2/, ...), each holding MeshSimplify.dll (Windows)
# and/or MeshSimplify.so (Linux). Add the one matching the running Nuke;
# Nuke only loads the binary with its own platform's extension.
import os

import nuke

_dir = os.path.dirname(os.path.abspath(__file__))
_mm = "%d.%d" % (nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR)
_bin = os.path.join(_dir, _mm)
if os.path.isdir(_bin):
    nuke.pluginAddPath(_bin.replace("\\", "/"), addToSysPath=False)
else:
    nuke.tprint("MeshSimplify: no binary for Nuke %s (looked in %s)" % (_mm, _bin))
