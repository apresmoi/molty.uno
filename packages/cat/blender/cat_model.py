"""Molty's cat as a rigged Blender model, built from scratch.

Run:  blender -b -P packages/cat/blender/cat_model.py
Out:  packages/cat/blender/cat.blend

Conventions
- Units match the .molty creature format: floor at z=0, the sitting head centre near 0.64.
- The cat faces -Y (Blender's front view). Its left (.L) is +X.
- The rest pose is standing.
- The body (chest, waist, rump, neck, upper arms and both hind legs) is ONE smooth merged shape: a
  metaball family whose elements each ride a bone, so the back flows into the thigh and down to the
  hock with no seam in any pose.
- The head, front forearms and paws, tail and features are smooth primitives (ellipsoid, capsule,
  cone) rigidly weighted to one bone each, grouped into a few mesh objects.
"""
import math
import os
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PALETTE = {  # creature.body.palette from packages/cat/cat.json
    'eye': '#0d0a12', 'shine': '#ffffff', 'body': '#a99be8', 'accent': '#7566c4', 'belly': '#f4f0ff',
    'ear': '#ffb7c8', 'nose': '#ff8fab', 'iris': '#9be564', 'whisker': '#6e7590', 'maw': '#160810',
    'bowl': '#e8434f', 'yarnline': '#8f1f2b',
}


def F(f, z, x=0.0):
    """Point from (forward, up, side). Forward is -Y."""
    return Vector((x, -f, z))


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hex_rgba(h):
    return [srgb_to_linear(int(h[i:i + 2], 16) / 255) for i in (1, 3, 5)] + [1.0]


# ------------------------------------------------------------------ skeleton (rest = standing)
# name: (head, tail, parent, connected, deform). Proportions follow a real cat seen from the side
# (Juan's walking tabby): a long, deep, nearly level body (rear to chest ~1.6 x the withers height,
# depth ~0.55 x), legs that leave ~1/3 of the height under the belly, the hind leg growing out of
# the body's lower rear corner (the thigh IS the rear end), the front leg out of the chest under the
# shoulder. The head stays the old cat's big cartoon head. The back is two bones (lumbar, thoracic)
# so it can round when the cat sits or crouches.
LEG_X = 0.075
HIND_X = 0.085
BONES = {
    'root': (F(0, 0), F(0.3, 0), None, False, False),
    'pelvis': (F(-0.36, 0.43), F(-0.22, 0.43), 'root', False, True),
    'spine': (F(-0.22, 0.43), F(-0.06, 0.43), 'pelvis', True, True),
    'spine2': (F(-0.06, 0.43), F(0.10, 0.43), 'spine', True, True),
    'chest': (F(0.10, 0.43), F(0.26, 0.44), 'spine2', True, True),
    'neck': (F(0.27, 0.475), F(0.36, 0.595), 'chest', False, True),
    'head': (F(0.38, 0.615), F(0.38, 0.815), 'neck', False, True),
}
TAIL_N, TAIL_LEN = 12, 0.062
for i in range(TAIL_N):
    a = F(-0.37 - i * TAIL_LEN, 0.50)
    b = F(-0.37 - (i + 1) * TAIL_LEN, 0.50)
    BONES[f'tail{i + 1}'] = (a, b, 'pelvis' if i == 0 else f'tail{i}', i > 0, True)

HEAD_C, HEAD_R = F(0.45, 0.715), (0.21, 0.18, 0.18)  # the old cat's head: E('head', [0.21, 0.18, 0.18])

for s, d in (('L', 1), ('R', -1)):
    x, xh = d * LEG_X, d * HIND_X
    # ears: pivot on the skull, tipped outward like the old cat's (rot z = -0.2 per side)
    ep = HEAD_C + Vector((d * 0.115, 0.01, 0.12))
    BONES[f'ear.{s}'] = (ep, ep + Vector((d * math.sin(0.2), 0, math.cos(0.2))) * 0.17, 'head', False, True)
    # front leg: scapula, humerus (elbow points back, level with the belly), radius, paw
    BONES[f'scapula.{s}'] = (F(0.18, 0.53, x), F(0.27, 0.365, x), 'chest', False, True)
    BONES[f'upperarm.{s}'] = (F(0.27, 0.365, x), F(0.22, 0.24, x), f'scapula.{s}', True, True)
    BONES[f'forearm.{s}'] = (F(0.22, 0.24, x), F(0.24, 0.07, x), f'upperarm.{s}', True, True)
    BONES[f'hand.{s}'] = (F(0.24, 0.07, x), F(0.29, 0.015, x), f'forearm.{s}', True, True)
    # hind leg: femur (the stifle sits at the flank fold), tibia, metatarsus (the hock angles back)
    BONES[f'thigh.{s}'] = (F(-0.30, 0.40, xh), F(-0.22, 0.25, xh), 'pelvis', False, True)
    BONES[f'shin.{s}'] = (F(-0.22, 0.25, xh), F(-0.36, 0.15, xh), f'thigh.{s}', True, True)
    BONES[f'foot.{s}'] = (F(-0.36, 0.15, xh), F(-0.34, 0.03, xh), f'shin.{s}', True, True)
    BONES[f'toe.{s}'] = (F(-0.34, 0.03, xh), F(-0.285, 0.015, xh), f'foot.{s}', True, True)
    # controls (not deforming): paw IK targets on the floor, the hock target rides the hind paw,
    # elbow and knee poles ride the body
    BONES[f'ik_hand.{s}'] = (*BONES[f'hand.{s}'][:2], 'root', False, False)
    BONES[f'ik_foot.{s}'] = (*BONES[f'toe.{s}'][:2], 'root', False, False)
    BONES[f'hock_target.{s}'] = (F(-0.36, 0.15, xh), F(-0.36, 0.20, xh), f'ik_foot.{s}', False, False)
    BONES[f'pole_elbow.{s}'] = (F(0.05, 0.24, x), F(0.05, 0.29, x), 'chest', False, False)
    BONES[f'pole_knee.{s}'] = (F(0.02, 0.26, xh), F(0.02, 0.31, xh), 'pelvis', False, False)


# ------------------------------------------------------------------ parts
# Each part: (group, colour, bone, kind, args). Groups become mesh objects.
PARTS = []


def ellipsoid(group, color, bone, center, radii, pitch=0.0, normal=None):
    PARTS.append((group, color, bone, 'ellipsoid', dict(center=center, radii=radii, pitch=pitch, normal=normal)))


def capsule(group, color, bone, p0, p1, r0, r1):
    PARTS.append((group, color, bone, 'capsule', dict(p0=p0, p1=p1, r0=r0, r1=r1)))


def cone(group, color, bone, base, tip, r, flat=1.0):
    PARTS.append((group, color, bone, 'cone', dict(base=base, tip=tip, r=r, flat=flat)))


# The body's metaball elements: (bone, centre, surface radii (side, forward, up), pitch, stiffness).
# Neighbours merge into one smooth skin. A lone element's surface sits where its field
# stiffness * (1 - (r/R)^2)^3 meets the threshold; a stiffer element blends over a shorter reach, which
# keeps a crease where the haunch folds against the body.
META = []
THRESHOLD = 0.6


def surface_fraction(stiffness):
    return math.sqrt(1 - (THRESHOLD / stiffness) ** (1 / 3))


def blob(bone, center, radii, pitch=0.0, stiffness=2.0):
    META.append((bone, center, radii, pitch, stiffness))


def chain(bone, p0, p1, radii):
    """A tapering limb segment: round elements spaced along the bone, merging into one tube."""
    n = len(radii)
    ax = p1 - p0
    ang = math.degrees(math.atan2(ax.z, -ax.y))
    for i, r in enumerate(radii):
        blob(bone, p0.lerp(p1, i / (n - 1)), (r, r * 1.05, r), pitch=ang)


def on_head(x, f, z, out=0.0):
    """A point on the head surface along (x, f, z) from the centre, pushed `out` along the normal."""
    d = Vector((x, -f, z))
    a, b, c = HEAD_R
    t = 1 / math.sqrt((d.x / a) ** 2 + (d.y / b) ** 2 + (d.z / c) ** 2)
    p = d * t
    n = Vector((p.x / a ** 2, p.y / b ** 2, p.z / c ** 2)).normalized()
    return HEAD_C + p + n * out, n


# face rig: eyes (scale Y to blink), the mouth opening and the tongue (scale up from nothing)
MOUTH = HEAD_C + Vector((0, -0.172, -0.098))
for s, d in (('L', 1), ('R', -1)):
    ec, _ = on_head(d * 0.085, 0.155, 0.01, 0.0)
    BONES[f'eye.{s}'] = (ec, ec + Vector((0, 0, 0.05)), 'head', False, True)
BONES['mouth'] = (MOUTH, MOUTH + Vector((0, 0, 0.04)), 'head', False, True)
BONES['tongue'] = (MOUTH + Vector((0, 0.01, 0.004)), MOUTH + Vector((0, -0.05, -0.02)), 'head', False, True)


def build_parts():
    PARTS.clear()
    META.clear()
    # the body is ONE smooth merged shape (a metaball family): chest, waist, rump, neck, upper arms and
    # both hind legs, so the back flows into the thigh and down to the hock with no seam
    blob('chest', F(0.17, 0.395), (0.125, 0.17, 0.148), pitch=4)
    blob('spine2', F(0.03, 0.39), (0.12, 0.14, 0.138))
    blob('spine', F(-0.12, 0.39), (0.12, 0.14, 0.135))
    blob('pelvis', F(-0.27, 0.41), (0.12, 0.12, 0.12), pitch=-4)
    blob('neck', F(0.32, 0.52), (0.09, 0.09, 0.12), pitch=-35)
    # head: the old cat's head, a hint of muzzle for the side profile, pointy ears pink inside
    ellipsoid('head', 'body', 'head', HEAD_C, HEAD_R)
    ellipsoid('head', 'body', 'head', HEAD_C + Vector((0, -0.1, -0.055)), (0.09, 0.085, 0.07))
    nose, n = on_head(0, 0.2, -0.045, 0.0)
    ellipsoid('nose', 'nose', 'head', nose + Vector((0, -0.012, 0)), (0.022, 0.012, 0.014))
    # the mouth opening and the tongue are rigged but scaled to nothing until a clip opens them
    ellipsoid('maw', 'maw', 'mouth', MOUTH, (0.042, 0.016, 0.03), pitch=-20)
    ellipsoid('tongue', 'nose', 'tongue', MOUTH + Vector((0, -0.02, -0.006)), (0.024, 0.038, 0.01), pitch=-25)
    for s, d in (('L', 1), ('R', -1)):
        head, tail = BONES[f'ear.{s}'][:2]
        up = (tail - head).normalized()
        cone('head', 'body', f'ear.{s}', head - up * 0.005, head + up * 0.165, 0.078, flat=0.8)
        cone('earIn', 'ear', f'ear.{s}', head + up * 0.012 + Vector((0, -0.03, 0)), head + up * 0.14 + Vector((0, -0.016, 0)), 0.052, flat=0.4)
        # round eyes: a green iris, a round pupil, a white shine
        p, n = on_head(d * 0.085, 0.155, 0.01, 0.0)
        ellipsoid('iris', 'iris', f'eye.{s}', p, (0.05, 0.05, 0.012), normal=n)
        p2, _ = on_head(d * 0.085, 0.155, 0.01, 0.008)
        ellipsoid('pupil', 'eye', f'eye.{s}', p2, (0.024, 0.024, 0.01), normal=n)
        p3, _ = on_head(d * 0.07, 0.155, 0.03, 0.014)
        ellipsoid('shine', 'shine', f'eye.{s}', p3, (0.01, 0.01, 0.006), normal=n)
        # whiskers: three per side from the muzzle, fanned and drooping a little, riding the head
        root = HEAD_C + Vector((d * 0.05, -0.17, -0.07))
        for el in (16, 2, -12):
            e = math.radians(el - 8)  # fanned, with a slight droop
            dirn = Vector((d * math.cos(e), -0.25, math.sin(e))).normalized()
            capsule('whisker', 'whisker', 'head', root, root + dirn * 0.25, 0.0035, 0.002)
        # front leg: the upper arm melts into the chest, the forearm and paw read as the leg
        sh, el, wr, ht = *(BONES[f'{b}.{s}'][0] for b in ('upperarm', 'forearm', 'hand')), BONES[f'hand.{s}'][1]
        chain(f'upperarm.{s}', sh, el, (0.05, 0.046, 0.042, 0.038, 0.036))
        capsule(f'legF.{s}', 'body', f'forearm.{s}', el, wr, 0.035, 0.028)
        ellipsoid(f'legF.{s}', 'body', f'hand.{s}', (wr + ht) / 2 + Vector((0, 0, 0.004)), (0.034, 0.046, 0.026), pitch=-12)
        # hind leg: a big muscular thigh melting into the rump, narrowing below the knee to a slim
        # shin, a high hock, the long near-vertical metatarsus and a round paw
        hp, kn, hk, bl = (BONES[f'{b}.{s}'][0] for b in ('thigh', 'shin', 'foot', 'toe'))
        tt = BONES[f'toe.{s}'][1]
        ax = kn - hp
        ang = math.degrees(math.atan2(ax.z, -ax.y))
        # The thigh IS the rear end: there is no buttock behind the legs. The pelvis ends at the hip, the
        # tail root sits on top of the thigh, and the body flows forward into the thigh. Every hind mass
        # rides the thigh or shin bone, so the rear moves with the leg when it folds (sit, crouch, pounce).
        back = Vector((0, 1, 0))
        blob(f'thigh.{s}', hp.lerp(kn, 0.38) + back * 0.025, (0.09, 0.14, 0.11), pitch=ang, stiffness=3.0)
        blob(f'thigh.{s}', hp + back * 0.04 + Vector((0, 0, -0.03)), (0.088, 0.085, 0.09), stiffness=3.0)  # its round back
        blob(f'thigh.{s}', hp.lerp(kn, 0.85), (0.05, 0.055, 0.05), pitch=ang)
        sv = hk - kn
        blob(f'shin.{s}', kn.lerp(hk, 0.22) + back * 0.018, (0.042, 0.05, 0.05), pitch=math.degrees(math.atan2(sv.z, -sv.y)))  # calf
        chain(f'shin.{s}', kn, hk, (0.044, 0.038, 0.033, 0.029, 0.027, 0.026))
        chain(f'foot.{s}', hk, bl, (0.027, 0.025, 0.024, 0.023, 0.023, 0.024))
        blob(f'toe.{s}', (bl + tt) / 2 + Vector((0, 0, 0.002)), (0.033, 0.047, 0.024), pitch=-8)
    # tail: one capsule per segment, tapering to a round tip
    for i in range(TAIL_N):
        a, b = BONES[f'tail{i + 1}'][:2]
        r0, r1 = 0.034 - 0.0013 * i, 0.034 - 0.0013 * (i + 1)
        capsule('tail', 'accent', f'tail{i + 1}', a, b, r0, r1)


# ------------------------------------------------------------------ geometry
def _sphere(bm, u=28, v=18):
    return bmesh.ops.create_uvsphere(bm, u_segments=u, v_segments=v, radius=1.0)['verts']


def _frame(axis):
    a = axis.normalized()
    ref = Vector((1, 0, 0)) if abs(a.x) < 0.9 else Vector((0, 0, 1))
    b = a.cross(ref).normalized()
    c = a.cross(b)
    return a, b, c


def add_part(bm, kind, args):
    if kind == 'ellipsoid':
        verts = _sphere(bm)
        rx, ry, rz = args['radii']
        if args['normal'] is not None:  # a disc lying on a surface: local z along the normal
            rot = args['normal'].to_track_quat('Z', 'Y').to_matrix().to_4x4()
            m = Matrix.Translation(args['center']) @ rot @ Matrix.Diagonal((rx, ry, rz, 1))
        else:  # radii are (side, forward, up), pitched about the side axis (nose up = positive)
            m = (Matrix.Translation(args['center']) @ Matrix.Rotation(math.radians(-args['pitch']), 4, 'X')
                 @ Matrix.Diagonal((rx, ry, rz, 1)))
        for vt in verts:
            vt.co = m @ vt.co
        return verts
    if kind == 'capsule':
        verts = _sphere(bm, 20, 14)
        p0, p1, r0, r1 = args['p0'], args['p1'], args['r0'], args['r1']
        a, b, c = _frame(p1 - p0)
        for vt in verts:
            x, y, z = vt.co
            vt.co = (p1 + (b * x + c * y + a * z) * r1) if z >= -1e-6 else (p0 + (b * x + c * y + a * z) * r0)
        return verts
    if kind == 'cone':
        res = bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=20, radius1=1.0, radius2=0.0, depth=1.0)
        verts = res['verts']
        base, tip, r, flat = args['base'], args['tip'], args['r'], args['flat']
        axis = tip - base
        h = axis.length
        z = axis.normalized()
        y = Vector((0, -1, 0))  # flatten front-to-back
        y = (y - z * y.dot(z)).normalized()
        x = y.cross(z)
        rot = Matrix((x, y, z)).transposed().to_4x4()
        m = Matrix.Translation(base + axis / 2) @ rot @ Matrix.Diagonal((r, r * flat, h, 1))
        for vt in verts:
            vt.co = m @ vt.co
        return verts
    raise ValueError(kind)


def build_props(scene):
    """Props for the clips, hidden until a clip shows them: a food bowl with milk, a ball of yarn."""
    def obj(name, group, color, make):
        bm = bmesh.new()
        make(bm)
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        for f in me.polygons:
            f.use_smooth = True
        ob = bpy.data.objects.new(name, me)
        scene.collection.objects.link(ob)
        ob.data.materials.append(material(color))
        ob['molty_group'], ob['molty_color'] = group, color
        ob.hide_render = True
        return ob

    def bowl(bm):
        bmesh.ops.create_cone(bm, cap_ends=True, segments=32, radius1=0.08, radius2=0.105, depth=0.06,
                              matrix=Matrix.Translation((0, 0, 0.03)))
    obj('bowl', 'bowl', 'bowl', bowl)
    milk = obj('milk', 'milk', 'belly', lambda bm: add_part(bm, 'ellipsoid', dict(
        center=Vector((0, 0, 0.058)), radii=(0.092, 0.092, 0.006), pitch=0, normal=None)))
    milk.parent = bpy.data.objects['bowl']
    yarn = obj('yarn', 'yarn', 'bowl', lambda bm: add_part(bm, 'ellipsoid', dict(
        center=Vector((0, 0, 0)), radii=(0.055, 0.055, 0.055), pitch=0, normal=None)))

    def wraps(bm):
        for rx, rz in ((0, 0), (math.radians(70), math.radians(30)), (math.radians(-50), math.radians(-60))):
            # a thin disc through the ball: only its rim shows, a wrap of yarn round the ball
            ring = bmesh.ops.create_uvsphere(bm, u_segments=40, v_segments=4, radius=1.0)['verts']
            m = Matrix.Rotation(rz, 4, 'Z') @ Matrix.Rotation(rx, 4, 'X') @ Matrix.Diagonal((0.0565, 0.0565, 0.006, 1))
            for v in ring:
                v.co = m @ v.co
    lines = obj('yarnline', 'yarnline', 'yarnline', wraps)
    lines.parent = yarn


CHEST_PATCH = (F(0.32, 0.40), (0.075, 0.08, 0.11), -25)  # centre, radii (side, forward, up), pitch


def material(key):
    """Flat palette colour: emission for the renders, diffuse colour for the viewport."""
    name = 'pal.' + key
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.diffuse_color = hex_rgba(PALETTE[key])
    nt = m.node_tree
    nt.nodes.clear()
    em, out = nt.nodes.new('ShaderNodeEmission'), nt.nodes.new('ShaderNodeOutputMaterial')
    em.inputs['Color'].default_value = hex_rgba(PALETTE[key])
    nt.links.new(em.outputs[0], out.inputs[0])
    return m


def chest_mask_nodes(nt, mask_obj):
    """Nodes giving 1 inside the chest patch volume (an empty scaled to the patch), 0 outside."""
    tc = nt.nodes.new('ShaderNodeTexCoord')
    tc.object = mask_obj
    ln = nt.nodes.new('ShaderNodeVectorMath')
    ln.operation = 'LENGTH'
    lt = nt.nodes.new('ShaderNodeMath')
    lt.operation = 'LESS_THAN'
    lt.inputs[1].default_value = 1.0
    nt.links.new(tc.outputs['Object'], ln.inputs[0])
    nt.links.new(ln.outputs['Value'], lt.inputs[0])
    return lt.outputs[0]


def body_material(mask_obj):
    """The body's one smooth surface, white on the chest: the patch is colour, never a shape."""
    m = material('body')
    nt = m.node_tree
    em = next(n for n in nt.nodes if n.type == 'EMISSION')
    mix = nt.nodes.new('ShaderNodeMix')
    mix.data_type = 'RGBA'
    mix.inputs['A'].default_value = hex_rgba(PALETTE['body'])
    mix.inputs['B'].default_value = hex_rgba(PALETTE['belly'])
    nt.links.new(chest_mask_nodes(nt, mask_obj), mix.inputs['Factor'])
    nt.links.new(mix.outputs['Result'], em.inputs['Color'])
    return m


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = 'METRIC'

    # armature
    arm_data = bpy.data.armatures.new('CatRig')
    arm = bpy.data.objects.new('CatRig', arm_data)
    scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    for name, (h, t, parent, conn, deform) in BONES.items():
        eb = arm_data.edit_bones.new(name)
        eb.head, eb.tail = h, t
        eb.use_deform = deform
    for name, (h, t, parent, conn, deform) in BONES.items():
        eb = arm_data.edit_bones[name]
        if parent:
            eb.parent = arm_data.edit_bones[parent]
            eb.use_connect = conn
    # roll: local Z toward world +Z-ish so local X stays the side axis for sagittal bones
    for eb in arm_data.edit_bones:
        d = (eb.tail - eb.head).normalized()
        eb.align_roll(Vector((0, 0, 1)) if abs(d.z) < 0.8 else Vector((0, -1, 0)))
    bpy.ops.object.mode_set(mode='POSE')

    # IK: front paw (2-bone, elbow pole), hind leg (thigh+shin reach the hock target, the
    # metatarsus aims at the paw), paws copy their control's rotation
    for s in ('L', 'R'):
        pb = arm.pose.bones
        c = pb[f'forearm.{s}'].constraints.new('IK')
        c.target, c.subtarget, c.chain_count = arm, f'ik_hand.{s}', 2
        c.pole_target, c.pole_subtarget, c.pole_angle = arm, f'pole_elbow.{s}', math.radians(-90)
        c = pb[f'hand.{s}'].constraints.new('COPY_ROTATION')
        c.target, c.subtarget = arm, f'ik_hand.{s}'
        c = pb[f'shin.{s}'].constraints.new('IK')
        c.target, c.subtarget, c.chain_count = arm, f'hock_target.{s}', 2
        c.pole_target, c.pole_subtarget, c.pole_angle = arm, f'pole_knee.{s}', math.radians(90)
        c = pb[f'foot.{s}'].constraints.new('DAMPED_TRACK')
        c.target, c.subtarget = arm, f'ik_foot.{s}'
        c = pb[f'toe.{s}'].constraints.new('COPY_ROTATION')
        c.target, c.subtarget = arm, f'ik_foot.{s}'
    for pb in arm.pose.bones:
        pb.rotation_mode = 'XYZ'
    bpy.ops.object.mode_set(mode='OBJECT')
    ctl = arm_data.collections.new('controls')
    dfm = arm_data.collections.new('deform')
    for b in arm_data.bones:
        (dfm if b.use_deform else ctl).assign(b)

    # meshes: one object per group, each part rigidly weighted to its bone
    build_parts()
    groups = {}
    for group, color, bone, kind, args in PARTS:
        groups.setdefault(group, []).append((color, bone, kind, args))
    for group, parts in groups.items():
        bm = bmesh.new()
        vg_of = []
        for color, bone, kind, args in parts:
            vs = add_part(bm, kind, args)
            vg_of.append((bone, [v for v in vs], color))
        bm.verts.index_update()
        idx = [(bone, [v.index for v in vs], color) for bone, vs, color in vg_of]
        me = bpy.data.meshes.new(group)
        bm.to_mesh(me)
        bm.free()
        for p in me.polygons:
            p.use_smooth = True
        ob = bpy.data.objects.new(group, me)
        scene.collection.objects.link(ob)
        ob.data.materials.append(material(parts[0][0]))
        for bone, ids, color in idx:
            vg = ob.vertex_groups.get(bone) or ob.vertex_groups.new(name=bone)
            vg.add(ids, 1.0, 'REPLACE')
        mod = ob.modifiers.new('rig', 'ARMATURE')
        mod.object = arm
        ob.parent = arm
        ob['molty_group'] = group
        ob['molty_color'] = parts[0][0]

    # the body: one metaball family ('Body', 'Body.001', ...), each element riding its bone
    bpy.context.view_layer.update()
    for i, (bone, center, radii, pitch, stiff) in enumerate(META):
        name = 'Body' if i == 0 else f'Body.{i:03d}'
        mb = bpy.data.metaballs.new(name)
        mb.resolution = mb.render_resolution = 0.006
        mb.threshold = THRESHOLD
        big = max(radii)
        e = mb.elements.new()
        e.type, e.radius, e.stiffness = 'ELLIPSOID', big / surface_fraction(stiff), stiff
        e.size_x, e.size_y, e.size_z = (r / big for r in radii)
        ob = bpy.data.objects.new(name, mb)
        scene.collection.objects.link(ob)
        ob.parent, ob.parent_type, ob.parent_bone = arm, 'BONE', bone
        bpy.context.view_layer.update()
        ob.matrix_world = Matrix.Translation(center) @ Matrix.Rotation(math.radians(-pitch), 4, 'X')
        ob.pass_index = 1  # the ID pass paints the chest patch on pass index 1
        if i == 0:
            mask = bpy.data.objects.new('chest_patch', None)
            scene.collection.objects.link(mask)
            mask.parent, mask.parent_type, mask.parent_bone = arm, 'BONE', 'chest'
            bpy.context.view_layer.update()
            c, r, pitch = CHEST_PATCH
            mask.matrix_world = Matrix.Translation(c) @ Matrix.Rotation(math.radians(-pitch), 4, 'X') @ Matrix.Diagonal((*r, 1))
            mb.materials.append(body_material(mask))
            ob['molty_group'], ob['molty_color'] = 'body', 'body'
    bpy.context.view_layer.update()

    build_props(scene)

    # check: the rest pose must survive the IK (no twist, no drift)
    bpy.context.view_layer.update()
    worst = 0.0
    for b in arm.data.bones:
        pb = arm.pose.bones[b.name]
        worst = max(worst, (pb.head - b.head_local).length, (pb.tail - b.tail_local).length)
    print(f'[cat] rest drift under IK: {worst:.5f}')
    out = os.path.join(HERE, 'cat.blend')
    bpy.context.preferences.filepaths.save_version = 0  # no .blend1 backups
    bpy.ops.wm.save_as_mainfile(filepath=out)
    print('[cat] saved', out, len(arm.data.bones), 'bones,', len(groups), 'groups')
    return worst


if __name__ == '__main__':
    build()
