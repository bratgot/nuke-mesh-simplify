# MeshSimplify menu.py (GUI only). Lives in .nuke/MeshSimplify/ next to
# init.py, which adds the per-version binary folder at startup.
#
# Install: cmake --install <build dir> --prefix ~/.nuke (scripts/build_all.*
# -Install / --install does this), or by hand:
#   1. copy the MeshSimplify folder into ~/.nuke
#   2. add to ~/.nuke/init.py:   nuke.pluginAddPath('./MeshSimplify')

import nuke


def _register_meshsimplify():
    """Add MeshSimplify to the Nodes toolbar under 3D > Modify."""
    toolbar = nuke.menu("Nodes")
    three_d = toolbar.findItem("3D") or toolbar.addMenu("3D")
    modify = three_d.findItem("Modify") or three_d.addMenu("Modify")
    modify.addCommand("MeshSimplify", "nuke.createNode('MeshSimplify')")


_register_meshsimplify()
