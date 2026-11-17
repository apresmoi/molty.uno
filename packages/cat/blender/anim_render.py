"""Bake the cat's clips onto the rig and render them for the preview page (animations.html).

Run:  blender -b packages/cat/blender/cat.blend -P packages/cat/blender/anim_render.py -- [clip ...]
      (no clip names = every clip in cat_clips.CLIPS)
      --data re-derives only the braille data (dots, silhouettes, floor rows) and keeps the frames
Out:  actions named after each clip, saved into cat.blend (rig + props keyed on every frame)
      anim/<clip>/NNNN.webp   clean frames (flat palette, outline, floor ticks under a walking cat)
      anim/<clip>.js          braille frames as dot data for the page's canvas, plus the foot-slip check
      anim/index.js           the clip list the page loads
Foot slip: every planted paw's contact point (front toe tip, hind ball) may not move more than half a
braille dot while it is planted; the worst case per clip is printed and shown on the page.
"""
import base64
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
_argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
sys.argv = [sys.argv[0]]

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from bpy_extras.object_utils import world_to_camera_view  # noqa: E402
from mathutils import Vector  # noqa: E402

import render_sheet as RS  # noqa: E402
import cat_clips  # noqa: E402
import cat_poses  # noqa: E402

RS.CLEAN_PX = 320
OUT = os.path.join(HERE, 'anim')
os.makedirs(OUT, exist_ok=True)
DOT = RS.VIEW_SIZE / RS.DW                     # one braille dot in world units
SLIP_MAX = 0.5 * DOT
arm, scene = RS.arm, RS.scene
PROPS = {'bowl': ['bowl', 'milk'], 'yarn': ['yarn', 'yarnline']}
CONTACT = {'F.L': ('hand.L', 'tail'), 'F.R': ('hand.R', 'tail'), 'H.L': ('toe.L', 'head'), 'H.R': ('toe.R', 'head')}


def show_props(props):
    for name, objs in PROPS.items():
        on = name in props
        for o in objs:
            bpy.data.objects[o].hide_render = not on
        if on:
            loc, rot = props[name]
            ob = bpy.data.objects[name]
            ob.location, ob.rotation_euler = loc, rot
    bpy.context.view_layer.update()


def contacts():
    out = {}
    for leg, (bone, end) in CONTACT.items():
        pb = arm.pose.bones[bone]
        out[leg] = np.array(arm.matrix_world @ (pb.tail if end == 'tail' else pb.head))
    return out


def floor_ticks(path, view):
    """Ticks on the floor every 0.1 so a planted paw can be seen not to slide (side views)."""
    img = RS.load_pixels(path + '_clean.png')[..., :3]
    H = img.shape[0]
    if view == 'side':
        cam = RS.cam
        c = world_to_camera_view(scene, cam, cat_poses.F(0, 0))
        y = int(round((1 - c.y) * H))
        img[min(H - 1, y + 1):min(H, y + 2), :] = (0.78, 0.76, 0.84)
        f0 = -cam.location.y
        for k in range(int((f0 - 1) * 10), int((f0 + 1) * 10) + 1):
            c = world_to_camera_view(scene, cam, cat_poses.F(k / 10, 0))
            x = int(round(c.x * H))
            if 1 <= x < H - 1:
                img[y + 1:min(H, y + 6), x - 1:x + 1] = (0.6, 0.58, 0.7)
    return img


def save_webp(path, rgb):
    h, w = rgb.shape[:2]
    im = bpy.data.images.new('frame', w, h, alpha=False)
    im.colorspace_settings.name = 'Non-Color'
    a = np.ones((h, w, 4), np.float32)
    a[..., :3] = rgb
    im.pixels.foreach_set(a[::-1].ravel())
    s = scene.render.image_settings
    s.file_format, s.quality, s.color_mode = 'WEBP', 82, 'RGB'
    scene.view_settings.view_transform = 'Standard'
    im.save_render(path, scene=scene)
    bpy.data.images.remove(im)


def encode(on, rgb, palette):
    """Dots as bytes: index (uint16, row-major over 104 x 128) + colour slot (uint8), base64."""
    ys, xs = np.nonzero(on)
    buf = bytearray()
    for y, x in zip(ys, xs):
        c = tuple(int(v) for v in np.clip(np.round(rgb[y, x] * 31), 0, 31))
        if c not in palette:
            palette[c] = len(palette)
        i = int(y) * RS.DW + int(x)
        buf += bytes((i & 255, i >> 8, palette[c]))
    return base64.b64encode(bytes(buf)).decode()


def encode_mask(m):
    """Runs of a dot mask, (row, start, length) uint8 triples, base64."""
    buf = bytearray()
    for y in range(m.shape[0]):
        row = np.concatenate(([0], m[y].astype(np.int8), [0]))
        d = np.diff(row)
        for a, b in zip(np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]):
            buf += bytes((y, int(a), int(b - a)))
    return base64.b64encode(bytes(buf)).decode()


def floor_row(view):
    """Dot row of the floor under the cat in this view (the floor contact line)."""
    c = world_to_camera_view(scene, RS.cam, RS.cam.location + RS.cam.matrix_world.to_3x3().col[2] * -6 - Vector((0, 0, (RS.FLOOR_AT - 0.5) * RS.VIEW_SIZE)))
    return round((1 - c.y) * RS.DH, 2)


def render_clip(name, data_only=False):
    """Render every view of a clip. The first view is the primary one (frames in anim/<clip>/), the
    others go to anim/<clip>/<view>/; the pose, the slip check and the bake are view-independent."""
    fn, dur, loop, views, follow = cat_clips.CLIPS[name]
    views = (views,) if isinstance(views, str) else tuple(views)
    n = int(round(dur * cat_clips.FPS))
    fdir = os.path.join(OUT, name)
    dirs = {v: fdir if k == 0 else os.path.join(fdir, v) for k, v in enumerate(views)}
    for d in dirs.values():
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            if f.endswith('.webp') and not data_only:
                os.remove(os.path.join(d, f))
    masks = {v: [] for v in views}
    cams, frows = [], {}
    frames = {v: [] for v in views}
    palette = {v: {} for v in views}
    baked, runs, short = [], {}, 0.0
    c0 = root0 = None
    for i in range(n):
        t = i / cat_clips.FPS
        pose, props, planted = fn(t)
        cam_jolt = props.pop('_cam', (0.0, 0.0)) if props else (0.0, 0.0)
        cat_poses.apply(arm, pose)
        show_props(props)
        short = max(short, max(cat_poses.reach_errors(arm).values()))
        if c0 is None:
            c0, root0 = RS.pose_center_f(), pose.get('root', 0.0)
        if isinstance(follow, bool):
            cf = c0 + (pose.get('root', 0.0) - root0 if follow else 0.0)
        else:
            cf = follow
        for view in views:
            RS.place_camera(view, cf)
            if cam_jolt != (0.0, 0.0):  # the world jolts: the camera shakes across the view
                right = RS.cam.matrix_world.to_3x3().col[0]
                RS.cam.location += right * cam_jolt[0] + Vector((0, 0, cam_jolt[1]))
                bpy.context.view_layer.update()
            base = os.path.join(dirs[view], f'{i:04d}')
            if not data_only:
                RS.set_clean()
                RS.render(base + '_clean.png')
                RS.clean_lines(base)
                save_webp(base + '.webp', floor_ticks(base, view))
                os.remove(base + '_clean.png')
            on, rgb = RS.braille(base)
            frames[view].append(encode(on, rgb, palette[view]))
            masks[view].append(encode_mask(RS.LAST_SIL))
            if view not in frows:
                frows[view] = floor_row(view)
        cams.append(round((cf - c0) / DOT, 2))   # how far the camera has travelled, in dots (the floor stays put)
        # foot slip: track every planted paw's contact point across its contact
        pts = contacts()
        for leg in cat_clips.OFFSET:
            if leg in planted:
                runs.setdefault(leg, [[]])[-1].append(pts[leg])
            elif runs.get(leg) and runs[leg][-1]:
                runs[leg].append([])
        baked.append(({pb.name: pb.matrix_basis.copy() for pb in arm.pose.bones},
                      {o: (bpy.data.objects[o].location.copy(), bpy.data.objects[o].rotation_euler.copy(),
                           bpy.data.objects[o].hide_render) for objs in PROPS.values() for o in objs}))
        print(f'[anim] {name} {i + 1}/{n}', flush=True)
    slip = {leg: max([float(np.max(np.linalg.norm(np.array(r) - r[0], axis=1))) for r in rs if len(r) > 1] or [0.0])
            for leg, rs in runs.items()}
    worst = max(slip.values() or [0.0])
    print(f'[anim] {name}: worst foot slip {worst:.4f} = {worst / DOT:.2f} dot (limit 0.5); IK short {short:.3f}')
    pal = {v: ['#%02x%02x%02x' % tuple(int(c * 255 / 31) for c in col) for col, _ in sorted(palette[v].items(), key=lambda kv: kv[1])]
           for v in views}
    data = dict(name=name, fps=cat_clips.FPS, n=n, loop=loop, view=views[0], views=list(views),
                palette=pal[views[0]], frames=frames[views[0]], masks=masks[views[0]], floor_row=frows[views[0]], cam=cams,
                by_view={v: dict(palette=pal[v], frames=frames[v], masks=masks[v], floor_row=frows[v]) for v in views[1:]},
                slip_dots=round(worst / DOT, 3), slip_ok=worst <= SLIP_MAX, ik_short=round(short, 4))
    with open(os.path.join(OUT, name + '.js'), 'w') as f:
        f.write('window.CLIPS = window.CLIPS || {};\nwindow.CLIPS[%s] = %s;\n' % (json.dumps(name), json.dumps(data)))
    if not data_only:
        bake(name, baked)
    return data


def bake(name, baked):
    """Key the clip on the rig (and its props) as Blender actions named after the clip."""
    ad = arm.animation_data_create()
    act = bpy.data.actions.get(name) or bpy.data.actions.new(name)
    act.use_fake_user = True
    ad.action = act
    for i, (bones, _) in enumerate(baked):
        for pb in arm.pose.bones:
            pb.matrix_basis = bones[pb.name]
            for path in ('location', 'rotation_euler', 'scale'):
                pb.keyframe_insert(path, frame=i + 1)
    ad.action = None
    for objs in PROPS.values():
        for o in objs:
            if all(b[1][o][2] for b in baked):
                continue
            ob = bpy.data.objects[o]
            oa = ob.animation_data_create()
            pa = bpy.data.actions.get(f'{name}.{o}') or bpy.data.actions.new(f'{name}.{o}')
            pa.use_fake_user = True
            oa.action = pa
            for i, (_, props) in enumerate(baked):
                ob.location, ob.rotation_euler, ob.hide_render = props[o]
                for path in ('location', 'rotation_euler', 'hide_render'):
                    ob.keyframe_insert(path, frame=i + 1)
            oa.action = None
            ob.hide_render = True


def write_index():
    names = [n for n in cat_clips.CLIPS if os.path.exists(os.path.join(OUT, n + '.js'))]
    with open(os.path.join(OUT, 'index.js'), 'w') as f:
        f.write('window.CLIP_LIST = %s;\n' % json.dumps(names))


def main():
    data_only = '--data' in _argv
    names = [a for a in _argv if not a.startswith('--')] or list(cat_clips.CLIPS)
    for n in names:
        render_clip(n, data_only)
    write_index()
    cat_poses.reset(arm)
    show_props({})
    for ob in (bpy.data.objects.get('SheetCam'),):
        if ob:
            bpy.data.objects.remove(ob)
    m = bpy.data.materials.get('sheet.depth')
    if m:
        bpy.data.materials.remove(m)
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, 'cat.blend'))
    print('[anim] baked', names, 'into cat.blend')


main()
