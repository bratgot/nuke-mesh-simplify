# Builds Test-files/MeshSimplify_demo.nk: a camera move over a dense, textured
# landscape, cropped to what the camera sees over the whole shot, plus a Cube
# box crop. Run headless with the Nuke version you want the scene for:
#   set MS_PLUGIN_DIR=<repo>/build-<ver>/Release
#   Nuke<ver>.exe -ti test/make_demo_scene.py
import os
import sys

import nuke

plug = os.environ.get("MS_PLUGIN_DIR")
if plug:
    nuke.pluginAddPath(plug.replace("\\", "/"), addToSysPath=False)
nuke.load("MeshSimplify")

here = os.path.dirname(os.path.abspath(__file__))
out = os.environ.get("MS_SCENE", os.path.join(here, "..", "Test-files", "MeshSimplify_demo.nk"))
out = os.path.abspath(out).replace("\\", "/")


def node_of(classes, **kw):
    for c in classes:
        try:
            return getattr(nuke.nodes, c)(**kw)
        except Exception:
            continue
    raise RuntimeError("none of %s exist" % (classes,))


def place(n, x, y):
    n.setXYpos(int(x), int(y))
    return n


root = nuke.root()
root["format"].setValue("HD_1080")
root["first_frame"].setValue(1)
root["last_frame"].setValue(100)
root["fps"].setValue(24)

# --- geometry: a dense ground plane and a row of spheres ------------------
checker = place(nuke.nodes.CheckerBoard2(), 0, -400)
ground = place(node_of(["Card2", "Card"], rows=400, columns=400), 0, -300)
ground.setInput(0, checker)
ground["image_aspect"].setValue(False)
ground["uniform_scale"].setValue(1000.0)
ground["rotate"].setValue([-90.0, 0.0, 0.0])
ground["label"].setValue("ground 1000 x 1000: 160k quads")

sph_tex = place(nuke.nodes.ColorWheel(), 200, -400)
spheres = []
for i in range(9):
    # spread wider than the camera pan: the outer ones get cropped away
    s = place(nuke.nodes.Sphere(rows=150, columns=150), 200 + i * 110, -300)
    s.setInput(0, sph_tex)
    s["uniform_scale"].setValue(4.0)
    s["translate"].setValue([-160.0 + i * 40.0, 4.0, -60.0])
    spheres.append(s)
merge = place(nuke.nodes.MergeGeo(), 300, -180)
for i, n in enumerate([ground] + spheres):
    merge.setInput(i, n)
merge["label"].setValue("~360k faces in")

# --- camera: a pan across part of the scene -------------------------------
cam = place(node_of(["Camera2", "Camera3", "Camera"]), 650, -180)
cam["focal"].setValue(35.0)
cam["far"].setValue(150.0)   # with 'clip to near / far' the horizon is cropped too
tr = cam["translate"]
tr.setAnimated()
for f, x in ((1, -25.0), (100, 25.0)):
    tr.setValueAt(x, f, 0)
    tr.setValueAt(4.0, f, 1)
    tr.setValueAt(20.0, f, 2)
ro = cam["rotate"]
ro.setAnimated()
for f, y in ((1, 25.0), (100, -25.0)):
    ro.setValueAt(-6.0, f, 0)
    ro.setValueAt(y, f, 1)
cam["label"].setValue("pans frames 1-100")

# --- 1: crop to everything the camera sees over the shot, then simplify ---
ms_cam = place(nuke.nodes.MeshSimplify(), 300, -60)
ms_cam.setInput(0, merge)
ms_cam.setInput(1, cam)
ms_cam["crop_camera"].setValue(True)
ms_cam["crop_frames"].setValue("frame range")
ms_cam["overscan"].setValue(100.0)
ms_cam["target_ratio"].setValue(0.5)
ms_cam["lock_borders"].setValue(True)
ms_cam["name"].setValue("MeshSimplify_camera_crop")
ms_cam["label"].setValue("frustum union 1-100\n+100px overscan, ratio 0.5")

# --- 2: Cube box crop of the same geometry (no simplification) ------------
crop_cube = place(nuke.nodes.Cube(), 100, -60)
crop_cube["translate"].setValue([-60.0, 0.0, -60.0])
crop_cube["scaling"].setValue([100.0, 30.0, 60.0])
crop_cube["label"].setValue("crop region")
ms_box = place(nuke.nodes.MeshSimplify(), 100, 60)
ms_box.setInput(0, merge)
ms_box.setInput(2, crop_cube)
ms_box["crop_camera"].setValue(False)
ms_box["crop_box_enable"].setValue(True)
ms_box["target_ratio"].setValue(1.0)
ms_box["name"].setValue("MeshSimplify_box_crop")
ms_box["label"].setValue("Cube crop, quads kept")

# --- render the camera-cropped result through the camera ------------------
bg = place(nuke.nodes.Constant(), 520, 40)
bg["color"].setValue([0.05, 0.05, 0.06, 1.0])
scene = place(nuke.nodes.Scene(), 300, 60)
scene.setInput(0, ms_cam)
render = place(nuke.nodes.ScanlineRender(), 300, 160)
render.setInput(0, bg)
render.setInput(1, scene)
render.setInput(2, cam)
viewer = place(nuke.nodes.Viewer(), 300, 280)
viewer.setInput(0, render)
viewer.setInput(1, ms_cam)
viewer.setInput(2, ms_box)
viewer.setInput(3, merge)

bd = place(nuke.nodes.BackdropNode(), 60, -130)
bd["bdwidth"].setValue(380)
bd["bdheight"].setValue(260)
bd["label"].setValue("MeshSimplify 1.1 crop demo\n"
                     "Viewer 1: render  2: camera crop  3: box crop  4: input\n"
                     "Tab in the Viewer for 3D; scrub to see the crop cover the whole move")
bd["note_font_size"].setValue(14)

nuke.scriptSaveAs(out, overwrite=1)
print("SCENE " + out)
sys.stdout.flush()
