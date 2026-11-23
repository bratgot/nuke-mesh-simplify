# Headless timing of MeshSimplify on a dense sphere.
#   set MS_PLUGIN_DIR=<build-ver>/Release
#   Nuke14.1.exe -ti test/perf.py
# Times nuke.execute() of a WriteGeo (obj) after MeshSimplify, minus the same
# graph with MeshSimplify disabled at ratio ~0 output (sphere build + tiny write).
import os
import sys
import time

import nuke

plug = os.environ.get("MS_PLUGIN_DIR")
if plug:
    nuke.pluginAddPath(plug, addToSysPath=False)
nuke.load("MeshSimplify")

out_dir = os.environ.get("MS_OUT", os.path.join(os.path.dirname(__file__), "out")).replace("\\", "/")
if not os.path.isdir(out_dir):
    os.makedirs(out_dir)

rows = int(os.environ.get("MS_ROWS", "500"))  # Sphere clamps rows/columns at 512
sph = nuke.nodes.Sphere(rows=rows, columns=rows)
ms = nuke.nodes.MeshSimplify(inputs=[sph])
ms["target_ratio"].setValue(0.05)
ms["target_error"].setValue(0.05)
wg = nuke.nodes.WriteGeo(inputs=[ms], file=out_dir + "/perf.obj")

configs = [
    ("plain", {}),
    ("pin_silhouette", {"pin_silhouette": True}),
    ("quads", {"output_quads": True}),
]
extra = os.environ.get("MS_EXTRA", "")
for name, kv in configs:
    for k in ("pin_silhouette", "output_quads"):
        ms[k].setValue(False)
    for k, v in kv.items():
        ms[k].setValue(v)
    best = 1e9
    for rep in range(3):
        sph["rows"].setValue(rows - 3 * configs.index((name, kv)) - rep)   # distinct -> real recook
        t0 = time.time()
        nuke.execute(wg, 1, 1)
        best = min(best, time.time() - t0)
    print("PERF %s rows=%d tris~%d best=%.3fs" % (name, rows, 2 * rows * rows, best))
sys.stdout.flush()
