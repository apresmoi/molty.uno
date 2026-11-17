"""Overlay a real cat's side silhouette on ours, to check the rump, thigh and hock line.

The reference is one walking frame from Eadweard Muybridge, Animal Locomotion, Plate 716 (1887,
public domain; Boston Public Library scan on Wikimedia Commons). Pass the plate image and the frame's
crop box; the script thresholds the cat out of the photo, scales it to our cat by the tail root's
height over the floor, pins the two tail roots together and draws both outlines.

Run:  blender -b packages/cat/blender/cat.blend -P packages/cat/blender/overlay_ref.py -- PLATE.jpg X0 Y0 X1 Y1
Out:  packages/cat/blender/renders/overlay_<pose>.png
"""
import os
import sys

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
_argv = sys.argv[sys.argv.index('--') + 1:]
sys.argv = [sys.argv[0]]
from render_sheet import *  # noqa: E402,F401,F403  reuse the sheet's camera, passes and helpers

PLATE, (X0, Y0, X1, Y1) = _argv[0], map(int, _argv[1:5])
REF_TAIL = 25                           # column (in the crop) of the tail root
OURS, THEIRS = np.array((0.36, 0.30, 0.78)), np.array((0.93, 0.33, 0.18))


def dilate(m):
    e = m.copy()
    e[1:] |= m[:-1]; e[:-1] |= m[1:]; e[:, 1:] |= m[:, :-1]; e[:, :-1] |= m[:, 1:]
    return e


def fill_holes(m):
    outside = np.zeros_like(m)
    outside[0, :] = outside[-1, :] = outside[:, 0] = outside[:, -1] = True
    outside &= ~m
    while True:
        grown = dilate(outside) & ~m
        if (grown == outside).all():
            return ~outside
        outside = grown


def reference_mask():
    img = bpy.data.images.load(PLATE)
    w, h = img.size
    px = np.array(img.pixels[:], np.float32).reshape(h, w, 4)[::-1][Y0:Y1, X0:X1, :3]
    m = px.mean(-1) < 0.38
    m[:3] = False                       # the plate's frame border
    m = dilate(erode(m))                # drop the thin grid lines
    return fill_holes(m), px


def top_at(mask, col):
    rows = np.nonzero(mask[:, int(round(col))])[0]
    return rows.min()


def project(p):
    """World point -> pixel (x, y) in the 560 px side render."""
    from bpy_extras.object_utils import world_to_camera_view
    c = world_to_camera_view(scene, cam, p)
    return c.x * 560, (1 - c.y) * 560


def overlay(pose_name):
    pose = {n: p for n, p, _ in sheet_poses()}[pose_name]
    cat_poses.apply(arm, pose)
    place_camera('side', pose_center_f())
    set_ids(False)
    tmp = os.path.join(OUT, '_ov_ids.png')
    render(tmp)
    lab = np.rint(load_pixels(tmp)[..., 0] * 255 / 8)
    ours = np.isin(lab, [LABEL[g] for g in STRUCT + FEAT])
    os.remove(tmp)
    pb = arm.pose.bones
    tx, _ = project(pb['tail1'].head)
    ty = top_at(ours, tx)

    ref, photo = reference_mask()
    rty = top_at(ref, REF_TAIL)
    # our cat is stylized (a short, tall trunk under a big head), so the whole body cannot match a
    # real one; compare the hindquarters: one uniform scale from the tail-root height over the floor,
    # pinned at the tail root
    s = (np.nonzero(ours.any(1))[0].max() - ty) / (np.nonzero(ref.any(1))[0].max() - rty)
    # map every reference pixel into our image: pinned at the tail root
    H, W = ours.shape
    yy, xx = np.mgrid[0:H, 0:W]
    rx = np.rint((xx - tx) / s + REF_TAIL).astype(int)
    ry = np.rint((yy - ty) / s + rty).astype(int)
    ok = (rx >= 0) & (rx < ref.shape[1]) & (ry >= 0) & (ry < ref.shape[0])
    theirs = np.zeros_like(ours)
    theirs[ok] = ref[ry[ok], rx[ok]]

    img = np.ones((H, W, 3), np.float32) * 0.97
    img[ours] = (0.86, 0.83, 0.97)
    img[theirs & ~ours] = (0.99, 0.88, 0.84)
    for m, col in ((ours, OURS), (theirs, THEIRS)):
        e = m & ~erode(m)
        e = e | dilate(e) & m
        img[e] = col
    # the photo, scaled the same way, in the corner for reference
    ph = np.kron(photo, np.ones((2, 2, 1)))[: H // 3, : W // 2]  # the photo, inset
    img[H - ph.shape[0]:, :ph.shape[1]] = ph
    out = os.path.join(OUT, f'overlay_{pose_name}.png')
    save_rgb(out, img)
    print('[overlay]', out, 'scale', round(s, 2), 'overlap', round((ours & theirs).sum() / (ours | theirs).sum(), 3))


for name in ('standing', 'walk_passing'):
    overlay(name)
