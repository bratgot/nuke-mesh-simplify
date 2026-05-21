# menu.py
#
# MeshSimplify -- Nuke 14 mesh simplification plugin
# Created by Marten Blumen
#
# This file lives in %USERPROFILE%\.nuke\MeshSimplify\ alongside the
# MeshSimplify.dll. It is auto-loaded by Nuke once the MeshSimplify
# subfolder is registered via pluginAddPath in the main .nuke\menu.py.
#
# Recommended install: run `.\deploy.ps1 -Install` from the project root.
# That script copies the .dll and this file into the subfolder, then
# appends (does not overwrite) a registration block to .nuke\menu.py.
#
# Manual install:
#   1. Create %USERPROFILE%\.nuke\MeshSimplify\
#   2. Copy MeshSimplify.dll and this file into it.
#   3. Add (or append) this to %USERPROFILE%\.nuke\menu.py:
#          import nuke
#          nuke.pluginAddPath('./MeshSimplify')

import nuke


def _register_meshsimplify():
    """Add MeshSimplify to the Nodes toolbar under 3D > Modify."""

    toolbar = nuke.menu("Nodes")

    # Use the existing 3D menu if present (Nuke's classic 3D system lives
    # there); fall back to creating one if for some reason it isn't.
    three_d = toolbar.findItem("3D") or toolbar.addMenu("3D")

    # Modify submenu is where deformers and topology ops sit.
    modify = three_d.findItem("Modify") or three_d.addMenu("Modify")

    modify.addCommand(
        "MeshSimplify",
        "nuke.createNode('MeshSimplify')",
        # Uncomment if you add a 24x24 PNG named MeshSimplify.png to the
        # same folder:
        # icon="MeshSimplify.png",
    )


_register_meshsimplify()
