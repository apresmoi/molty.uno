"""Render the cat's model sheet: every pose as a clean flat render plus a braille preview at Molty's
resolution (52x32 cells of 2x4 dots on the round panel), then write model-sheet.html.

Run:  blender -b packages/cat/blender/cat.blend -P packages/cat/blender/render_sheet.py
Out:  packages/cat/blender/renders/*.png, packages/cat/blender/model-sheet.html  (local only)
"""
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import cat_poses  # noqa: E402
import cat_clips  # noqa: E402


def sheet_poses():
    """The sheet's poses come from the clips: sitting, standing, and the four walk keys (timed on the near hind)."""
    keys = [('contact', 0.5), ('down', 0.625), ('passing', 0.75), ('up', 0.875)]
    return ([('sitting', cat_clips.SIT, ['side', 'front', 'three_quarter']), ('standing', cat_clips.STAND, ['side'])]
            + [(f'walk_{k}', cat_clips.walk_loop(t)[0], ['side']) for k, t in keys])

OUT = os.path.join(HERE, 'renders')
os.makedirs(OUT, exist_ok=True)

PAL = {'eye': '#0d0a12', 'shine': '#ffffff', 'body': '#a99be8', 'accent': '#7566c4', 'belly': '#f4f0ff',
       'ear': '#ffb7c8', 'nose': '#ff8fab', 'iris': '#9be564', 'whisker': '#6e7590', 'bowl': '#e8434f',
       'yarnline': '#8f1f2b'}
COLS, ROWS, PANEL = 52, 32, 412          # the device grid and panel width in px
DW, DH = COLS * 2, ROWS * 4              # dot lattice: 104 x 128
SS = 8                                   # ID render pixels per dot (each axis)
CLEAN_PX = 560                           # clean render size
VIEW_SIZE = 1.9                          # world units across the panel (same scale for every pose)
FLOOR_AT = 0.80                          # floor height on the panel, fraction from the top
VIEWS = {  # camera looks along this direction (from the cat's right side / front)
    'side': (Vector((1, 0, 0)), 0.0),
    'front': (Vector((0, 1, 0)), 6.0),
    'three_quarter': (Vector((math.sin(math.radians(40)), math.cos(math.radians(40)), 0)), 6.0),
}
STRUCT = ['body', 'head', 'legF.L', 'legF.R', 'tail']  # 'body' is the merged torso + hind legs
FEAT = ['chest', 'earIn', 'iris', 'pupil', 'shine', 'nose', 'maw', 'tongue']
OWNER = {'chest': 'body', 'earIn': 'head', 'iris': 'head', 'pupil': 'head', 'shine': 'head', 'nose': 'head',
         'maw': 'head', 'tongue': 'head'}
LOOSE = ['whisker']                          # drawn as fine lines, never part of the outline
PROPS = ['bowl', 'milk', 'yarn', 'yarnline']  # outlined in their own colours
DEPTH_JUMP = 0.03                        # world units: a depth step this big between neighbours is an occluding contour
CREASE, CREASE_R = 0.1, 0.023            # a shading valley this deep (over this reach, world units) reads as a crease line
SHADOW = 0.55                            # clean render: |normal . view| below this takes the shadow tone
INK = np.array((0.09, 0.07, 0.16))
BG = np.array((0.94, 0.93, 0.96))       # line colour on the clean render
GROUPS = STRUCT + FEAT + LOOSE + PROPS
LABEL = {g: i + 1 for i, g in enumerate(GROUPS)}

scene = bpy.context.scene
arm = bpy.data.objects['CatRig']
meshes = [o for o in scene.objects if 'molty_group' in o]  # mesh groups + the metaball body


def hexrgb(h):
    return np.array([int(h[i:i + 2], 16) for i in (1, 3, 5)], float) / 255


# ------------------------------------------------------------------ scene setup
cam_data = bpy.data.cameras.new('SheetCam')
cam_data.type = 'ORTHO'
cam_data.ortho_scale = VIEW_SIZE
cam = bpy.data.objects.new('SheetCam', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
scene.render.engine = 'BLENDER_WORKBENCH'
if scene.world is None:
    scene.world = bpy.data.worlds.new('World')
sh = scene.display.shading
sh.light = 'FLAT'
scene.view_settings.view_transform = 'Standard'
scene.render.image_settings.file_format = 'PNG'
arm.hide_render = True

# the depth pass: EEVEE with every surface emitting (camera depth, |normal . view|), written as float EXR
depth_mat = bpy.data.materials.new('sheet.depth')
_nt = depth_mat.node_tree
_nt.nodes.clear()
_N = lambda t: _nt.nodes.new(t)
_cd, _geo, _dot, _abs, _xyz, _em, _out = (_N(t) for t in ('ShaderNodeCameraData', 'ShaderNodeNewGeometry', 'ShaderNodeVectorMath',
                                                           'ShaderNodeMath', 'ShaderNodeCombineXYZ', 'ShaderNodeEmission', 'ShaderNodeOutputMaterial'))
_dot.operation, _abs.operation = 'DOT_PRODUCT', 'ABSOLUTE'
_nt.links.new(_geo.outputs['Normal'], _dot.inputs[0])
_nt.links.new(_geo.outputs['Incoming'], _dot.inputs[1])
_nt.links.new(_dot.outputs['Value'], _abs.inputs[0])
_nt.links.new(_cd.outputs['View Z Depth'], _xyz.inputs['X'])
_nt.links.new(_abs.outputs[0], _xyz.inputs['Y'])
_nt.links.new(_xyz.outputs[0], _em.inputs['Color'])
_nt.links.new(_em.outputs[0], _out.inputs[0])

# the id pass: every surface emits its object's id colour; the body's chest patch emits the chest id
ids_mat = bpy.data.materials.new('sheet.ids')
_it = ids_mat.node_tree
_it.nodes.clear()
_I = lambda t: _it.nodes.new(t)
_oi, _cmp, _mul, _mix, _iem, _iout = (_I(t) for t in ('ShaderNodeObjectInfo', 'ShaderNodeMath', 'ShaderNodeMath', 'ShaderNodeMix',
                                                    'ShaderNodeEmission', 'ShaderNodeOutputMaterial'))
_cmp.operation, _mul.operation = 'COMPARE', 'MULTIPLY'
_cmp.inputs[1].default_value, _cmp.inputs[2].default_value = 1.0, 0.1
_mix.data_type = 'RGBA'
_mix.inputs['B'].default_value = (LABEL['chest'] * 8 / 255, 0, 0, 1)
_it.links.new(_oi.outputs['Object Index'], _cmp.inputs[0])
_it.links.new(_cmp.outputs[0], _mul.inputs[0])
_tc = _I('ShaderNodeTexCoord')
_tc.object = bpy.data.objects.get('chest_patch')
_ln, _lt = _I('ShaderNodeVectorMath'), _I('ShaderNodeMath')
_ln.operation, _lt.operation = 'LENGTH', 'LESS_THAN'
_lt.inputs[1].default_value = 1.0
_it.links.new(_tc.outputs['Object'], _ln.inputs[0])
_it.links.new(_ln.outputs['Value'], _lt.inputs[0])
_it.links.new(_lt.outputs[0], _mul.inputs[1])
_it.links.new(_mul.outputs[0], _mix.inputs['Factor'])
_it.links.new(_oi.outputs['Color'], _mix.inputs['A'])
_it.links.new(_iem.outputs[0], _iout.inputs[0])
_it.links.new(_mix.outputs['Result'], _iem.inputs['Color'])


def set_clean(size=None):
    """Flat palette colours (the materials are pure emission), anti-aliased, on a transparent film."""
    scene.render.engine = 'BLENDER_EEVEE'
    bpy.context.view_layer.material_override = None
    scene.view_settings.view_transform = 'Standard'
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.filter_size = 1.5
    scene.eevee.taa_render_samples = 16
    scene.render.resolution_x = scene.render.resolution_y = size or CLEAN_PX
    scene.render.pixel_aspect_x = scene.render.pixel_aspect_y = 1
    scene.render.film_transparent = True


def set_size(dots):
    if dots:  # the dot lattice is 104 x 128 over a square panel: stretch the pixels so the image stays square
        scene.render.resolution_x, scene.render.resolution_y = DW * SS, DH * SS
        scene.render.pixel_aspect_x, scene.render.pixel_aspect_y = DH / DW, 1
    else:
        scene.render.resolution_x = scene.render.resolution_y = CLEAN_PX
        scene.render.pixel_aspect_x = scene.render.pixel_aspect_y = 1


def set_ids(dots):
    """Part ids as exact colours: each object's id, and the chest patch painted onto the body."""
    scene.render.engine = 'BLENDER_EEVEE'
    bpy.context.view_layer.material_override = ids_mat
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.view_settings.view_transform = 'Raw'
    scene.render.filter_size = 0.0
    scene.eevee.taa_render_samples = 1
    scene.render.film_transparent = True
    set_size(dots)
    scene.render.dither_intensity = 0
    for o in scene.objects:  # every element of the metaball family carries the body's id
        g = o.get('molty_group') or ('body' if o.type == 'META' else None)
        if g:
            o.color = (LABEL[g] * 8 / 255, 0, 0, 1)


def set_depth(dots):
    scene.render.engine = 'BLENDER_EEVEE'
    bpy.context.view_layer.material_override = depth_mat
    scene.view_settings.view_transform = 'Raw'
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.filter_size = 0.0
    scene.eevee.taa_render_samples = 1
    set_size(dots)


def place_camera(view, center_f):
    d, elev = VIEWS[view]
    e = math.radians(elev)
    d = Vector((d.x * math.cos(e), d.y * math.cos(e), -math.sin(e))).normalized()
    zc = (FLOOR_AT - 0.5) * VIEW_SIZE  # floor sits FLOOR_AT down the panel
    target = Vector((0, -center_f, zc))
    cam.location = target - d * 6
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    bpy.context.view_layer.update()
    return d


def world_verts(o, dg):
    ev = o.evaluated_get(dg)
    me = ev.to_mesh()
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get('co', co)
    m = np.array(ev.matrix_world)
    ev.to_mesh_clear()
    return co.reshape(-1, 3) @ m[:3, :3].T + m[:3, 3]


def pose_center_f():
    dg = bpy.context.evaluated_depsgraph_get()
    f = np.concatenate([-world_verts(o, dg)[:, 1] for o in meshes if not o.hide_render])
    return (f.min() + f.max()) / 2


def render(path):
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def load_pixels(path):
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = 'Non-Color'  # raw values in, raw values out
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4)[::-1]  # top row first


def save_rgb(path, rgb):
    h, w = rgb.shape[:2]
    img = bpy.data.images.new('tmp', w, h, alpha=True)
    img.colorspace_settings.name = 'Non-Color'
    a = np.ones((h, w, 4), np.float32)
    a[..., :3] = rgb
    img.pixels.foreach_set(a[::-1].ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


# ------------------------------------------------------------------ braille
def erode(m):
    e = m.copy()
    e[1:, :] &= m[:-1, :]
    e[:-1, :] &= m[1:, :]
    e[:, 1:] &= m[:, :-1]
    e[:, :-1] &= m[:, 1:]
    e[0, :] = e[-1, :] = e[:, 0] = e[:, -1] = False
    return e


def edge(m):
    return m & ~erode(m)


def shift(a, dy, dx, fill):
    out = np.full_like(a, fill)
    H, W = a.shape
    out[max(0, -dy):H - max(0, dy), max(0, -dx):W - max(0, dx)] = a[max(0, dy):H - max(0, -dy), max(0, dx):W - max(0, -dx)]
    return out


def analyse(base, dots):
    """Render ids + depth for the current view; return per-pixel labels, depth and contour pixels.
    A contour pixel is the NEARER side of an occluding edge: a depth step bigger than DEPTH_JUMP
    (the merged body folding over itself, a near leg over a far one), or a part boundary where a
    head, front leg or tail sits in front of something else."""
    set_ids(dots)
    render(base + '_ids.png')
    lab = np.rint(load_pixels(base + '_ids.png')[..., 0] * 255 / 8).astype(np.int32)
    set_depth(dots)
    render(base + '_depth.exr')
    px = load_pixels(base + '_depth.exr')
    dep, ndv = px[..., 0].astype(np.float64), px[..., 1]
    os.remove(base + '_ids.png')
    os.remove(base + '_depth.exr')
    whisker = lab == LABEL['whisker']
    lab, dep = see_through_whiskers(lab, dep)
    lab_all = lab
    cat = np.isin(lab, [LABEL[g] for g in STRUCT + FEAT])
    lab = np.where(cat, lab, 0)  # whiskers and props are drawn on their own, never as the cat's outline
    inside = lab > 0
    own = lab.copy()  # features count as the part they sit on
    for f in FEAT:
        own[lab == LABEL[f]] = LABEL[OWNER[f]]
    dep = np.where(inside, dep, np.inf)
    body = LABEL['body']
    line = np.zeros(lab.shape, bool)
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        nd, no = shift(dep, dy, dx, np.inf), shift(own, dy, dx, 0)
        behind = (no > 0) & (nd > dep)
        line |= inside & behind & (nd - dep > DEPTH_JUMP)
        line |= inside & behind & (no != own) & (own != body)
    # where the merged body creases without breaking (the round haunch folding against the flank, the
    # knee) draw that too: a dark valley in |normal . view| (a suggestive contour), away from the outline
    ring = inside.copy()
    for _ in range(3):
        ring = erode(ring)
    r = max(2, round(CREASE_R * lab.shape[0] / VIEW_SIZE))
    nd = np.where(inside, ndv, 0)
    avg = box(nd, r) / np.maximum(box(inside.astype(float), r), 1e-6)
    big = np.where(inside, nd, 9)
    floor_x = (big <= shift(big, 0, 1, 9)) & (big <= shift(big, 0, -1, 9))
    floor_y = (big <= shift(big, 1, 0, 9)) & (big <= shift(big, -1, 0, 9))
    crease = ring & (own == body) & (avg - nd > CREASE) & (nd < 0.7) & (floor_x | floor_y)  # the valley floor only
    return lab_all, dep, ndv, line, crease, whisker


def see_through_whiskers(lab, dep):
    """Whiskers are hairs: give their pixels the label and depth of what lies just above and below
    them (the head, or nothing), so they never cut the outline."""
    w = lab == LABEL['whisker']
    lab, dep = lab.copy(), dep.copy()
    up_l, up_d = np.zeros_like(lab), np.zeros_like(dep)
    dn_l, dn_d = np.zeros_like(lab), np.zeros_like(dep)
    got_up, got_dn = np.zeros_like(w), np.zeros_like(w)
    for k in range(1, 9):
        for sign, gl, gd, got in ((1, up_l, up_d, got_up), (-1, dn_l, dn_d, got_dn)):
            sl, sd = shift(lab, -sign * k, 0, 0), shift(dep, -sign * k, 0, np.inf)
            take = w & ~got & (sl != LABEL['whisker'])
            gl[take], gd[take] = sl[take], sd[take]
            got |= take
    both = (up_l > 0) & (dn_l > 0)
    lab[w] = np.where(both, up_l, 0)[w]
    dep[w] = np.where(both, (up_d + dn_d) / 2, dep)[w]
    return lab, dep


def box(a, r):
    """Box blur of radius r (separable, by cumulative sums)."""
    for ax in (0, 1):
        c = np.cumsum(np.pad(a, [(r + 1, r) if i == ax else (0, 0) for i in range(2)]), axis=ax)
        a = np.take(c, np.arange(2 * r + 1, c.shape[ax]), axis=ax) - np.take(c, np.arange(0, c.shape[ax] - 2 * r - 1), axis=ax)
    return a


def clean_lines(base):
    """Ink the clean render: the silhouette plus the occluding contours, ~2 px wide, and one flat
    shadow tone (the palette colour a step darker) where the body turns away, so the haunch reads."""
    lab, _, ndv, line, crease, _ = analyse(base, dots=False)
    inside = np.isin(lab, [LABEL[g] for g in STRUCT + FEAT])
    sil = inside & ~erode(inside)
    ink = sil | line
    for g in ('bowl', 'yarn'):
        m = (lab == LABEL[g]) | (lab == LABEL['milk' if g == 'bowl' else 'yarnline'])
        ink |= m & ~erode(m)
    ink = ink | (shift(ink, 0, 1, False) | shift(ink, 1, 0, False)) & inside
    px = load_pixels(base + '_clean.png')
    img = px[..., :3] * px[..., 3:] + BG * (1 - px[..., 3:])
    shadow = (lab == LABEL['body']) & (ndv < SHADOW)
    img[shadow] *= 0.84
    img[ink] = INK
    save_rgb(base + '_clean.png', img)


def braille(base):
    lab, dep, _, line, crease, whisker = analyse(base, dots=True)
    line = line | crease
    cells = lambda a: a.reshape(DH, SS, DW, SS).transpose(0, 2, 1, 3).reshape(DH, DW, SS * SS)
    lc = cells(lab)
    cov = {g: (lc == LABEL[g]).mean(-1) for g in GROUPS}
    cov['whisker'] = cells(whisker).mean(-1)
    sil = sum(cov[g] for g in STRUCT + FEAT) >= 0.4
    scov = {g: cov[g] + sum(cov[f] for f in FEAT if OWNER[f] == g) for g in STRUCT}
    owner = np.where(sil, np.stack([scov[g] for g in STRUCT], -1).argmax(-1), -1)
    lines = sil & (cells(line).sum(-1) >= 3)
    d = np.where(sil, np.nanmin(np.where(np.isinf(cells(dep)), np.nan, cells(dep)), axis=-1), np.nan)
    lo, hi = np.nanpercentile(d, 3), np.nanpercentile(d, 97)
    near = np.clip(1 - (d - lo) / max(1e-6, hi - lo), 0, 1)  # 1 = nearest

    body = hexrgb(PAL['body'])
    rgb = np.zeros((DH, DW, 3))
    on = np.zeros((DH, DW), bool)

    def shade(mask):
        t = near[mask][:, None]
        return np.clip(body * (0.5 + 0.5 * t) + (1 - body) * 0.45 * t ** 2, 0, 1)

    def put(mask, col):
        rgb[mask] = col
        on[mask] = True

    put(lines, shade(lines))
    feat = {g: cov[g] >= 0.3 for g in FEAT}
    put(edge(feat['chest']) & ~edge(sil), hexrgb(PAL['belly']))
    put(edge(feat['earIn']), hexrgb(PAL['ear']))
    eye = (cov['iris'] + cov['pupil'] + cov['shine']) >= 0.3
    put(edge(eye), hexrgb(PAL['iris']))
    put((cov['pupil'] + cov['shine'] >= 0.35) & erode(eye), hexrgb(PAL['iris']) * 0.75 + 0.25)
    put(feat['shine'] & erode(eye), hexrgb(PAL['shine']))
    put(cov['nose'] >= 0.25, hexrgb(PAL['nose']))
    put(edge(cov['maw'] >= 0.3), hexrgb(PAL['nose']))
    put(cov['tongue'] >= 0.3, hexrgb(PAL['nose']))
    put(edge(sil), shade(edge(sil)))
    # props: their own outlines (the bowl red with a white rim of milk, the yarn red with darker wraps)
    for g, inner in (('bowl', 'milk'), ('yarn', 'yarnline')):
        whole = (cov[g] + cov[inner]) >= 0.4
        put(edge(whole), hexrgb(PAL['bowl']))
    put(edge(cov['milk'] >= 0.3) & ~on, hexrgb(PAL['belly']))
    put((cov['yarnline'] >= 0.2) & ~on, hexrgb(PAL['yarnline']) * 1.4)
    # whiskers last, only where nothing else is drawn: fine lines that move with the head
    put((cov['whisker'] >= 0.22) & ~on, hexrgb(PAL['whisker']) * 1.3)
    global LAST_SIL
    LAST_SIL = sil | on  # the cat's (and props') dot silhouette, for the preview's floor halo
    return on, rgb


def draw_panel(on, rgb, path, scale=2):
    D = PANEL * scale
    img = np.zeros((D, D, 3), np.float32)
    yy, xx = np.mgrid[0:D, 0:D]
    disc = (xx - D / 2) ** 2 + (yy - D / 2) ** 2 <= (D / 2) ** 2
    img[~disc] = (0.07, 0.07, 0.09)
    img[disc] = (0, 0, 0)
    px, py = D / DW, D / DH
    for j in range(DH):
        for i in range(DW):
            cx, cy = (i + 0.5) * px, (j + 0.5) * py
            if (cx - D / 2) ** 2 + (cy - D / 2) ** 2 > (D / 2 - 4) ** 2:
                continue
            if on[j, i]:
                r, col = 1.55 * scale, rgb[j, i]
            elif i % 2 == 0 and j % 4 == 0:
                r, col = 0.6 * scale, np.array((0.13, 0.12, 0.2))
            else:
                continue
            x0, x1, y0, y1 = int(cx - r - 1), int(cx + r + 2), int(cy - r - 1), int(cy + r + 2)
            sub = (xx[y0:y1, x0:x1] - cx) ** 2 + (yy[y0:y1, x0:x1] - cy) ** 2 <= r * r
            img[y0:y1, x0:x1][sub] = col
    save_rgb(path, img)


# ------------------------------------------------------------------ main
def main():
    cards = []
    for name, pose, views in sheet_poses():
        cat_poses.apply(arm, pose)
        short = {k: round(v, 3) for k, v in cat_poses.reach_errors(arm).items() if v > 0.004}
        if short:
            print('[sheet]', name, 'IK short of target:', short)
        dg = bpy.context.evaluated_depsgraph_get()
        low = min(world_verts(o, dg)[:, 2].min() for o in meshes if not o.hide_render)
        if low < -0.006:
            print('[sheet]', name, f'sinks {-low:.3f} below the floor:', [o.name for o in meshes if world_verts(o, dg)[:, 2].min() < -0.006])
        cf = pose_center_f()
        for view in views:
            place_camera(view, cf)
            base = f'{name}_{view}'
            path = os.path.join(OUT, base)
            set_clean()
            render(path + '_clean.png')
            clean_lines(path)
            on, rgb = braille(path)
            draw_panel(on, rgb, path + '_braille.png')
            cards.append((name, view, base))
            print('[sheet]', base, int(on.sum()), 'dots')
    write_html(cards)


TITLES = {'sitting': 'Sitting', 'standing': 'Standing', 'walk_contact': 'Walk 1: contact', 'walk_down': 'Walk 2: down',
          'walk_passing': 'Walk 3: passing', 'walk_up': 'Walk 4: up'}
VIEWN = {'side': 'side', 'front': 'front', 'three_quarter': 'three-quarter'}


def write_html(cards):
    items = '\n'.join(
        f'''<figure><figcaption>{TITLES[n]} <span>{VIEWN[v]}</span></figcaption>
<div class="pair"><img src="renders/{b}_clean.png" alt="{TITLES[n]} {VIEWN[v]}, render"><img src="renders/{b}_braille.png" alt="{TITLES[n]} {VIEWN[v]}, braille"></div></figure>'''
        for n, v, b in cards)
    ov = [n for n in ('standing', 'walk_passing') if os.path.exists(os.path.join(OUT, f'overlay_{n}.png'))]
    overlays = '' if not ov else (
        '<h2>Reference check: the hindquarters</h2>\n<p>Orange: a real cat walking (Eadweard Muybridge, Animal Locomotion, '
        'Plate 716, 1887, public domain), scaled to the height of our tail root and pinned there. Purple: ours. From the tail root, '
        'down the back of the thigh to the hock is one convex curve on both: the thigh is the rear end. The body now follows a real '
        'cat (long, level, the legs leaving a third of the height under the belly), so the whole trunk lines up; only the big '
        'cartoon head stays. Made by overlay_ref.py.</p>\n<div class="grid">' + ''.join(
            f'<figure><figcaption>{TITLES[n]} <span>vs reference</span></figcaption><img src="renders/overlay_{n}.png" '
            f'alt="{TITLES[n]} silhouette over a real cat" style="width:100%;border-radius:8px"></figure>' for n in ov) + '</div>')
    html = f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cat Model Sheet</title>
<style>
:root{{--bg:#f4f2f8;--fg:#1d1830;--mute:#6b6585;--card:#fff;--line:#dcd8e6}}
@media (prefers-color-scheme:dark){{:root{{--bg:#0e0c14;--fg:#ece8f8;--mute:#9a93b5;--card:#17141f;--line:#2a2538}}}}
body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}}
main{{max-width:1180px;margin:0 auto;padding:28px 16px 60px}}
h1{{font-size:24px;margin:0 0 4px}} h2{{font-size:18px;margin:34px 0 4px}} p{{margin:0 0 6px;color:var(--mute);max-width:70ch}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(520px,1fr));gap:18px;margin-top:22px}}
figure{{margin:0;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px}}
figcaption{{font-weight:600;margin:0 0 8px}} figcaption span{{color:var(--mute);font-weight:400;margin-left:6px}}
.pair{{display:grid;grid-template-columns:1fr 1fr;gap:10px}} .pair img{{width:100%;border-radius:8px;display:block}}
@media (max-width:600px){{.grid{{grid-template-columns:1fr}}}}
</style></head><body><main>
<h1>Cat model sheet</h1>
<p>Phase 1 of the cat rebuild: the model and rig only, for sign-off before any animation or device code.</p>
<p>Each pose twice: left, the Blender model in flat palette colours; right, the same pose as Molty would draw it
(52 x 32 cells of 2 x 4 dots, one outline, brighter on the near side).</p>
<p>The body and hind legs are one merged shape: a long, level body; the thigh forms the rear end with the tail root on top; a narrow shin, a high hock over a near-vertical metatarsus. The white chest is surface colour only. Sitting, the back rounds and the whole hind foot lies flat.</p>
<p>Walk keys come from a gait model: a lateral sequence walk (left hind, left fore, right hind, right fore, a quarter cycle apart), as in Muybridge's cat plates. Keys are timed on the near hind paw.</p>
<p>Rig: root on the floor; pelvis, spine, chest, neck, head; two ears; 12 tail bones; front legs scapula, upper arm, forearm, paw; hind legs thigh, shin, hock (metatarsus), paw. IK targets on all four paws, the hock rides the hind paw target, elbow and knee poles.</p>
<div class="grid">
{items}
</div>
{overlays}
</main></body></html>
'''
    with open(os.path.join(HERE, 'model-sheet.html'), 'w') as f:
        f.write(html)
    print('[sheet] wrote model-sheet.html')


if __name__ == '__main__':
    main()
