# Headless functional tests for MeshSimplify (crop + simplify + object state).
#   set MS_PLUGIN_DIR=<repo>/build-<ver>/Release     (the fresh build, NDK_NOTES 50)
#   Nuke<ver>.exe -ti test/test_crop.py               (keep the path short, NDK_NOTES 53)
# Prints "PASS name" / "FAIL name: ..." and a SUMMARY line; exit code 1 on failure.
#
# Geometry is read back through WriteGeo (.obj, world space). The frustum
# tests use a 200x200 grid of 0.5-unit cells on a card at z = -10 and a camera
# at the origin with focal 50 / haperture 24.576, so the visible half-width is
# 10 * 24.576 / 100 = 2.4576 units and the expected cell counts are exact.
import os
import sys
import traceback

import nuke

plug = os.environ.get("MS_PLUGIN_DIR")
if plug:
    nuke.pluginAddPath(plug.replace("\\", "/"), addToSysPath=False)
nuke.load("MeshSimplify")

OUT = os.environ.get("MS_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))
OUT = OUT.replace("\\", "/")
if not os.path.isdir(OUT):
    os.makedirs(OUT)
TAG = os.environ.get("MS_TAG", "%d.%d" % (nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR))

results = []


def node_of(classes, **kw):
    for c in classes:
        try:
            return getattr(nuke.nodes, c)(**kw)
        except Exception:
            continue
    raise RuntimeError("none of %s exist" % (classes,))


def classic_camera(**kw):
    return node_of(["Camera2", "Camera3", "Camera"], **kw)


def classic_axis(**kw):
    return node_of(["Axis2", "Axis3", "Axis"], **kw)


_counter = [0]


def read_obj(node, frame=1):
    _counter[0] += 1
    path = "%s/t_%s_%d.obj" % (OUT, TAG, _counter[0])
    wg = nuke.nodes.WriteGeo(inputs=[node], file=path)
    try:
        nuke.execute(wg, frame, frame)
    finally:
        nuke.delete(wg)
    verts, faces = [], []
    read_obj.uvs = []
    if os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                if line.startswith("vt "):
                    read_obj.uvs.append(tuple(float(x) for x in line.split()[1:3]))
                elif line.startswith("v "):
                    verts.append(tuple(float(x) for x in line.split()[1:4]))
                elif line.startswith("f "):
                    faces.append([int(tok.split("/")[0]) for tok in line.split()[1:]])
        os.remove(path)
    return verts, faces


def bbox_of_faces(verts, faces):
    used = set(i for f in faces for i in f)
    if not used:
        return None
    pts = [verts[i - 1] for i in used]
    return [min(p[k] for p in pts) for k in range(3)] + [max(p[k] for p in pts) for k in range(3)]


def check(name, cond, msg=""):
    results.append((name, bool(cond)))
    print("%s %s %s" % ("PASS" if cond else "FAIL", name, "" if cond else msg))
    sys.stdout.flush()


def approx(a, b, eps=1e-3):
    return abs(a - b) <= eps


def run(name, fn):
    try:
        fn()
    except Exception:
        check(name, False, traceback.format_exc())


def grid_card(z=-10.0):
    # 1x1 card scaled by 100 with 200x200 cells -> 0.5-unit cells on [-50, 50].
    card = node_of(["Card2", "Card"], rows=200, columns=200)
    card["image_aspect"].setValue(False)
    card["uniform_scale"].setValue(100.0)
    card["translate"].setValue([0.0, 0.0, z])
    return card


def set_format(w, h, name):
    nuke.addFormat("%d %d 1 %s" % (w, h, name))
    nuke.root()["format"].setValue(name)


def new_ms(src, **kw):
    ms = nuke.nodes.MeshSimplify()
    ms.setInput(0, src)
    ms["target_ratio"].setValue(1.0)
    ms["crop_camera"].setValue(False)
    for k, v in kw.items():
        ms[k].setValue(v)
    return ms


# --------------------------------------------------------------------------
def t_card_calibration():
    card = grid_card()
    v, f = read_obj(card)
    bb = bbox_of_faces(v, f)
    check("card_calibration", len(f) == 40000 and approx(bb[0], -50) and approx(bb[3], 50)
          and approx(bb[2], -10), "faces=%d bbox=%s" % (len(f), bb))


def t_transform_preserved():
    cube = nuke.nodes.Cube(rows=8, columns=8)
    tg = nuke.nodes.TransformGeo(inputs=[cube])
    tg["translate"].setValue([10.0, 0.0, 0.0])
    ms = new_ms(tg, target_ratio=0.5)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    check("transform_preserved", bb and approx(bb[0], 9.5, 0.05) and approx(bb[3], 10.5, 0.05),
          "bbox=%s" % (bb,))


def t_uv_survives():
    # Cube uvs are per-vertex in some Nuke versions and per-POINT in 15.1-16.0;
    # both must come through (and must not read out of bounds).
    cube = nuke.nodes.Cube(rows=8, columns=8)
    tg = nuke.nodes.TransformGeo(inputs=[cube])
    for ratio, label in ((0.5, "simplified"), (1.0, "crop_only")):
        ms = new_ms(tg, target_ratio=ratio, crop_box_enable=(ratio == 1.0))
        ms["crop_box"].setValue([-10, -10, -10, 10, 10, 10])
        v, f = read_obj(ms)
        uvs = read_obj.uvs
        ok = len(f) > 0 and len(uvs) > 0 and all(-1e-4 <= a <= 1.0001 and -1e-4 <= b <= 1.0001 for a, b in uvs)
        spread = (max(a for a, b in uvs) - min(a for a, b in uvs)) if uvs else 0
        check("uv_survives_" + label, ok and spread > 0.5,
              "faces=%d uvs=%d spread=%.3f" % (len(f), len(uvs), spread))


def t_simplify_still_works():
    sph = nuke.nodes.Sphere(rows=60, columns=60)
    v0, f0 = read_obj(sph)
    ms = new_ms(sph, target_ratio=0.25)
    v, f = read_obj(ms)
    check("simplify_ratio", 0 < len(f) <= 0.3 * len(f0) * 1.05,
          "in=%d out=%d" % (len(f0), len(f)))


def t_box_world_keeps_quads():
    card = grid_card(z=0.0)
    ms = new_ms(card, crop_box_enable=True)
    ms["crop_box"].setValue([-2.4, -2.4, -1.0, 2.4, 2.4, 1.0])
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    sizes = set(len(x) for x in f)
    # touching: cells overlapping [-2.4, 2.4] are [-2.5, 2.5] -> 10 x 10
    check("box_world_touching", len(f) == 100 and sizes == {4} and approx(bb[0], -2.5)
          and approx(bb[4], 2.5), "faces=%d sizes=%s bbox=%s" % (len(f), sizes, bb))
    ms["crop_test"].setValue("inside")
    v, f = read_obj(ms)
    # inside: cells fully within [-2.4, 2.4] are [-2.0, 2.0] -> 8 x 8
    check("box_world_inside", len(f) == 64, "faces=%d" % len(f))


def t_box_from_cube():
    card = grid_card(z=0.0)
    cube = nuke.nodes.Cube()
    cube["translate"].setValue([10.0, 0.0, 0.0])
    cube["uniform_scale"].setValue(4.6)   # [7.7, 12.3] in x, [-2.3, 2.3] in y
    ms = new_ms(card, crop_box_enable=True)
    ms.setInput(2, cube)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # x cells [7.5, 12.5] -> 10, y cells [-2.5, 2.5] -> 10
    check("box_from_cube", ms.input(2) is not None and len(f) == 100 and approx(bb[0], 7.5)
          and approx(bb[3], 12.5), "faces=%d bbox=%s" % (len(f), bb))


def t_box_from_axis_rotated():
    card = grid_card(z=0.0)
    ax = classic_axis()
    ax["rotate"].setValue([0.0, 0.0, 90.0])
    ax["translate"].setValue([0.0, 20.0, 0.0])
    ms = new_ms(card, crop_box_enable=True)
    ms["crop_box"].setValue([-4.9, -0.9, -1.0, 4.9, 0.9, 1.0])   # long in local x
    ms.setInput(2, ax)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # rotated 90: world x span 1.8 -> cells [-1, 1] = 4, world y span [15.1, 24.9] -> [15, 25] = 20
    check("box_from_axis_rotated", ms.input(2) is not None and len(f) == 80 and bb and approx(bb[0], -1.0)
          and approx(bb[4], 25.0), "faces=%d bbox=%s" % (len(f), bb))


def t_box_removes_all():
    card = grid_card(z=0.0)
    ms = new_ms(card, crop_box_enable=True)
    ms["crop_box"].setValue([500, 500, 500, 501, 501, 501])
    v, f = read_obj(ms)
    check("box_removes_all_no_crash", len(f) == 0, "faces=%d" % len(f))


def cam_setup():
    cam = classic_camera()
    cam["focal"].setValue(50.0)
    cam["haperture"].setValue(24.576)
    cam["near"].setValue(0.1)
    cam["far"].setValue(10000.0)
    return cam


def t_frustum_square():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    check("frustum_square", ms.input(1) is not None and len(f) == 100 and bb and approx(bb[0], -2.5)
          and approx(bb[4], 2.5), "faces=%d bbox=%s" % (len(f), bb))
    # 100 px overscan each side on 1000 px: half-width 2.4576 * 1.2 = 2.949 -> [-3, 3]
    ms["overscan"].setValue(100.0)
    v, f = read_obj(ms)
    check("frustum_overscan", len(f) == 144, "faces=%d" % len(f))
    # disabling the camera crop keeps everything
    ms["crop_camera"].setValue(False)
    v, f = read_obj(ms)
    check("frustum_off", len(f) == 40000, "faces=%d" % len(f))


def t_frustum_hd():
    set_format(1920, 1080, "ms_hd")
    card = grid_card()
    cam = cam_setup()
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # vertical half 2.4576 * 1080 / 1920 = 1.3824 -> [-1.5, 1.5] = 6 rows x 10 cols
    check("frustum_hd_aspect", len(f) == 60 and bb and approx(bb[4], 1.5),
          "faces=%d bbox=%s" % (len(f), bb))


def t_frustum_window_translate():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    cam["win_translate"].setValue([0.9, 0.0])   # shifts the window by 0.9 half-widths
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # ndc x in [-0.1, 1.9] -> visible x = [-0.2458, 4.6694] -> cells [-0.5, 5] = 11 cols x 10 rows.
    # (The view axis is now near a side plane: guards the plane orientation.)
    check("frustum_win_translate", len(f) == 110 and bb and approx(bb[0], -0.5) and approx(bb[3], 5.0),
          "faces=%d bbox=%s" % (len(f), bb))


def t_frustum_window_offaxis():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    cam["win_translate"].setValue([1.5, 0.0])   # view axis now OUTSIDE the window
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # ndc x in [0.5, 2.5] -> visible x = [1.2288, 6.144] -> cells [1.0, 6.5] = 11 cols x 10 rows
    check("frustum_win_offaxis", len(f) == 110 and bb and approx(bb[0], 1.0) and approx(bb[3], 6.5),
          "faces=%d bbox=%s" % (len(f), bb))


def t_frustum_rotated_camera():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card(z=10.0)
    cam = cam_setup()
    cam["rotate"].setValue([0.0, 180.0, 0.0])   # now looking down +z at the card
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    check("frustum_rotated_camera", len(f) == 100 and bb and approx(bb[0], -2.5) and approx(bb[3], 2.5),
          "faces=%d bbox=%s" % (len(f), bb))


def t_frustum_win_scale_roll():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    cam["win_scale"].setValue([2.0, 2.0])      # screen = (ndc - T) / S: twice the view
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # visible half-width 4.9152 -> cells [-5, 5] = 20 x 20
    check("frustum_win_scale", len(f) == 400 and bb and approx(bb[3], 5.0), "faces=%d bbox=%s" % (len(f), bb))
    set_format(1920, 1080, "ms_hd")
    cam["win_scale"].setValue([1.0, 1.0])
    cam["winroll"].setValue(90.0)               # HD window turned on its side
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # x half 1.3824 -> [-1.5, 1.5] = 6 cols, y half 2.4576 -> [-2.5, 2.5] = 10 rows
    check("frustum_winroll", len(f) == 60 and bb and approx(bb[3], 1.5) and approx(bb[4], 2.5),
          "faces=%d bbox=%s" % (len(f), bb))


def t_unusable_camera_passes_through():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    cam["projection_mode"].setValue("orthographic")
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    check("ortho_camera_skips_crop", len(f) == 40000, "faces=%d" % len(f))


def t_multi_object():
    set_format(1000, 1000, "ms_sq1000")
    a = grid_card()
    ta = nuke.nodes.TransformGeo(inputs=[a])
    ta["translate"].setValue([0.25, 0.0, 0.0])   # moves the cell edges to .25 / .75
    b = grid_card()
    tb = nuke.nodes.TransformGeo(inputs=[b])
    tb["translate"].setValue([200.0, 0.0, 0.0])  # completely off screen
    mg = nuke.nodes.MergeGeo(inputs=[ta, tb])
    cam = cam_setup()
    ms = new_ms(mg, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # x edges at k*0.5 + 0.25: visible [-2.4576, 2.4576] -> [-2.75, 2.75] = 11 cols x 10 rows
    check("multi_object_offscreen_dropped", len(f) == 110 and bb and approx(bb[0], -2.75) and approx(bb[3], 2.75),
          "faces=%d bbox=%s" % (len(f), bb))


def t_frustum_near_far():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    cam["far"].setValue(5.0)          # card at distance 10 is beyond far
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    check("frustum_far_clips", len(f) == 0, "faces=%d" % len(f))
    ms["clip_near_far"].setValue(False)
    v, f = read_obj(ms)
    check("frustum_far_ignored", len(f) == 100, "faces=%d" % len(f))


def t_frustum_behind():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card(z=10.0)          # behind the camera
    cam = cam_setup()
    ms = new_ms(card, crop_camera=True, clip_near_far=False)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    check("frustum_behind_camera", len(f) == 0, "faces=%d" % len(f))


def t_frame_range_union():
    set_format(1000, 1000, "ms_sq1000")
    nuke.root()["first_frame"].setValue(1)
    nuke.root()["last_frame"].setValue(11)
    card = grid_card()
    cam = cam_setup()
    tr = cam["translate"]
    tr.setAnimated()
    tr.setValueAt(-5.0, 1, 0)
    tr.setValueAt(5.0, 11, 0)
    tr.setValueAt(0.0, 1, 1)
    tr.setValueAt(0.0, 11, 1)
    ms = new_ms(card, crop_camera=True)
    ms.setInput(1, cam)
    v1, f1 = read_obj(ms, frame=1)
    bb1 = bbox_of_faces(v1, f1)
    check("frame_current_follows_camera", len(f1) == 100 and bb1 and approx(bb1[0], -7.5),
          "faces=%d bbox=%s" % (len(f1), bb1))
    ms["crop_frames"].setValue("frame range")
    v, f = read_obj(ms, frame=1)
    bb = bbox_of_faces(v, f)
    # camera x from -5 to 5: visible x = [-7.4576, 7.4576] -> [-7.5, 7.5] = 30 cols x 10 rows
    check("frame_range_union", len(f) == 300 and bb and approx(bb[0], -7.5) and approx(bb[3], 7.5),
          "faces=%d bbox=%s" % (len(f), bb))
    v, f6 = read_obj(ms, frame=6)
    check("frame_range_same_every_frame", len(f6) == 300, "faces=%d" % len(f6))
    # Editing a key away from the current frame must re-cook (hash covers all frames).
    tr.setValueAt(10.0, 11, 0)
    v, f = read_obj(ms, frame=1)
    bb = bbox_of_faces(v, f)
    check("frame_range_hash_offframe_key", bb and approx(bb[3], 12.5), "faces=%d bbox=%s" % (len(f), bb))
    # explicit range + step
    ms["use_script_range"].setValue(False)
    ms["crop_first"].setValue(1)
    ms["crop_last"].setValue(1)
    v, f = read_obj(ms, frame=6)
    bb = bbox_of_faces(v, f)
    check("frame_range_explicit", bb and approx(bb[0], -7.5) and len(f) == 100,
          "faces=%d bbox=%s" % (len(f), bb))


def t_crop_then_simplify():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    ms = new_ms(card, crop_camera=True, target_ratio=0.5, lock_borders=True, overscan=100.0)
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    bb = bbox_of_faces(v, f)
    # cropped to 144 quads = 288 tris, simplified; flat grid with locked border keeps bbox
    check("crop_then_simplify", 0 < len(f) < 288 and bb and approx(bb[0], -3.0) and approx(bb[3], 3.0),
          "faces=%d bbox=%s" % (len(f), bb))


def t_box_and_camera():
    set_format(1000, 1000, "ms_sq1000")
    card = grid_card()
    cam = cam_setup()
    ms = new_ms(card, crop_camera=True, crop_box_enable=True)
    ms["crop_box"].setValue([0.1, -100, -100, 100, 100, 100])   # right half only
    ms.setInput(1, cam)
    v, f = read_obj(ms)
    # camera gives x in [-2.5, 2.5]; box keeps cells touching x >= 0.1 -> [0, 2.5] = 5 cols
    check("box_and_camera_intersect", len(f) == 50, "faces=%d" % len(f))


for name, fn in [
    ("card_calibration", t_card_calibration),
    ("transform_preserved", t_transform_preserved),
    ("simplify_ratio", t_simplify_still_works),
    ("uv_survives", t_uv_survives),
    ("box_world", t_box_world_keeps_quads),
    ("box_from_cube", t_box_from_cube),
    ("box_from_axis_rotated", t_box_from_axis_rotated),
    ("box_removes_all", t_box_removes_all),
    ("frustum_square", t_frustum_square),
    ("frustum_hd", t_frustum_hd),
    ("frustum_win_translate", t_frustum_window_translate),
    ("frustum_win_offaxis", t_frustum_window_offaxis),
    ("frustum_rotated", t_frustum_rotated_camera),
    ("frustum_win_scale_roll", t_frustum_win_scale_roll),
    ("ortho_camera", t_unusable_camera_passes_through),
    ("multi_object", t_multi_object),
    ("frustum_near_far", t_frustum_near_far),
    ("frustum_behind", t_frustum_behind),
    ("frame_range", t_frame_range_union),
    ("crop_then_simplify", t_crop_then_simplify),
    ("box_and_camera", t_box_and_camera),
]:
    run(name, fn)

npass = sum(1 for _, ok in results if ok)
print("SUMMARY %s %d/%d passed" % (TAG, npass, len(results)))
sys.stdout.flush()
if npass != len(results):
    sys.exit(1)
