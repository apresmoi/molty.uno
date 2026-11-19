"""Export the cat for the device: the rig, the body as rounded primitives per bone, every clip as
glTF-style animation channels, and the route map between pose families.

Run:  blender -b packages/cat/blender/cat.blend -P packages/cat/blender/export_molty.py [-- --cached [clip ...]]
      (--cached: reuse the dense samples cached in $CAT_SAMPLES, default <tmp>/molty-cat-samples.json, re-posing
      only the clips named; a full export poses every clip, about 30 minutes)
Out:  packages/cat/blender/cat-export.json  (packages/cat/build.mjs wraps it into packages/cat/cat.json),
      the dense samples cache (outside the repo)

Coordinates: the package's y is up and the cat faces +z (its left is +x): world_pkg = C . world_blender,
C = (x, z, -y). A bone's local frame is Blender's bone frame, so a node's local transform (relative to its
parent) is the same in both; only the root's is rotated by C.

Clips (glTF 2.0 animation layout): per node a channel for `t` (translation), `r` (rotation, unit quaternion
x y z w) and `s` (scale), each a sampler: `in` the key frames (at the clip's fps), `out` the values, and the
interpolation (STEP for a held value, else LINEAR or CUBICSPLINE, whichever encodes smaller: CUBICSPLINE stores per
key in-tangent, value, out-tangent, the tangents per frame). Outputs are int16 (translation and scale at 1/4096,
rotations normalised) or, for the soft nodes' rotations (tail, ears, face), int8 normalised (glTF's
KHR_mesh_quantization). Samples are taken from the posed rig (IK solved, so the keys are plain FK) at the fps, then
keys are dropped greedily while the curve stays within TOL of every sample. A channel equal to the rest pose all
through is left out.
"""
import base64
import json
import math
import os
import struct
import sys
import tempfile

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
ARGS = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
ONLY = set(a for a in ARGS if not a.startswith('--'))
sys.argv = [sys.argv[0]]

import cat_model as M  # noqa: E402
import cat_clips  # noqa: E402
import cat_poses  # noqa: E402

FPS = cat_clips.FPS
C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))       # blender -> package
TOL = {'t': 0.003, 'r': 0.008, 's': 0.02}         # about 0.15 dot at the body's size (a dot is 0.018)
TOL_FACE_S = 0.05                                 # eyelids, mouth, tongue: a scale off by this is not a dot
TOL_SOFT_R = 0.02                                 # tail, ears, face: their rotations as int8, looser
TOL_LEG_R = 0.0045                                # the chains from the root to the paws: a planted paw is their sum
LEGS = ('pelvis', 'spine', 'spine2', 'chest', 'scapula', 'upperarm', 'forearm', 'hand', 'thigh', 'shin', 'foot', 'toe')
QT, QR, QS = 4096.0, 32767.0, 4096.0              # int16: t and s at 1/4096, r normalised (glTF's KHR_mesh_quantization)
QR8 = 127.0                                       # int8 normalised quaternions for the soft nodes
SOFT = ('tail', 'ear', 'eye', 'mouth', 'tongue', 'neck', 'head')
VIEW_YAW = {'side': -90.0, 'front': 0.0, 'three_quarter': -40.0}
arm = bpy.data.objects['CatRig']
PROPS = {'bowl': ['bowl', 'milk'], 'yarn': ['yarn', 'yarnline']}


def r6(v):
    return float(f'{v:.6g}')


# ------------------------------------------------------------------ the rig
def nodes():
    """The nodes the device poses: the root, every deforming bone (IK controls are baked into the keys),
    then the props (each its own root-level node)."""
    bones = [b for b in arm.data.bones if b.use_deform or b.name == 'root']
    names = [b.name for b in bones]
    return names + list(PROPS)


NAMES = nodes()
INDEX = {n: i for i, n in enumerate(NAMES)}


def parent_of(name):
    if name in PROPS:
        return -1
    b = arm.data.bones[name]
    p = b.parent
    while p is not None and p.name not in INDEX:
        p = p.parent
    return INDEX[p.name] if p is not None else -1


def world_matrix(name, posed=True):
    if name in PROPS:
        ob = bpy.data.objects[name]
        return C @ ob.matrix_world
    if posed:
        return C @ arm.matrix_world @ arm.pose.bones[name].matrix
    return C @ arm.matrix_world @ arm.data.bones[name].matrix_local


def local_matrix(name, posed=True):
    p = parent_of(name)
    w = world_matrix(name, posed)
    return w if p < 0 else world_matrix(NAMES[p], posed).inverted() @ w


def trs(m):
    t, r, s = m.decompose()
    return [t.x, t.y, t.z], [r.x, r.y, r.z, r.w], [s.x, s.y, s.z]


# ------------------------------------------------------------------ the body as primitives
def bone_rest_inv(bone):
    return (C @ arm.data.bones[bone].matrix_local).inverted()


def to_local(bone, p):
    return list(bone_rest_inv(bone) @ (C @ Vector(p)))


def rot_local(bone, R3):
    q = (bone_rest_inv(bone).to_3x3() @ C.to_3x3() @ R3).to_quaternion()
    return [q.x, q.y, q.z, q.w]


def sub_group(group, bone):
    if group != 'body':
        return group
    for leg in ('thigh', 'shin', 'foot', 'toe'):
        if bone.startswith(leg + '.'):
            return 'hind' + bone[-1]
    return 'torso'


def shapes():
    """Every primitive on its bone, in the bone's rest frame: kind, group (what outlines it), colour, data."""
    M.build_parts()
    out = []
    for bone, center, radii, pitch, stiff in M.META:            # the merged body: smooth-blended ellipsoids
        u = Matrix.Rotation(math.radians(-pitch), 3, 'X')
        out.append({'k': 'blob', 'b': INDEX[bone], 'g': sub_group('body', bone), 'c': 'body',
                    'p': to_local(bone, center), 'q': rot_local(bone, u), 'r': list(radii), 'st': stiff})
    for group, color, bone, kind, a in M.PARTS:
        g = {'iris': 'eye', 'pupil': 'eye', 'shine': 'eye'}.get(group, group)
        base = {'b': INDEX[bone], 'g': g, 'c': color}
        if kind == 'ellipsoid':
            if a['normal'] is not None:
                u = a['normal'].to_track_quat('Z', 'Y').to_matrix()
            else:
                u = Matrix.Rotation(math.radians(-a['pitch']), 3, 'X')
            out.append({**base, 'k': 'ell', 'p': to_local(bone, a['center']), 'q': rot_local(bone, u), 'r': list(a['radii'])})
        elif kind == 'capsule':
            out.append({**base, 'k': 'cap', 'p': to_local(bone, a['p0']), 'p1': to_local(bone, a['p1']), 'r': [a['r0'], a['r1']]})
        elif kind == 'cone':
            # a cone flattened front to back: its base centre, tip, base radius and the flat axis
            flat_axis = list(bone_rest_inv(bone).to_3x3() @ (C.to_3x3() @ Vector((0, -1, 0))))
            out.append({**base, 'k': 'cone', 'p': to_local(bone, a['base']), 'p1': to_local(bone, a['tip']),
                        'r': [a['r'], a['flat']], 'fa': flat_axis})
    c, r, pitch = M.CHEST_PATCH                                  # the white chest: colour on the body, no shape
    out.append({'k': 'patch', 'b': INDEX['chest'], 'g': 'chest', 'c': 'belly', 'p': to_local('chest', c),
                'q': rot_local('chest', Matrix.Rotation(math.radians(-pitch), 3, 'X')), 'r': list(r)})
    # the props, in their own frames (Blender's: z up): the bowl a frustum on its local z, the milk a disc in it
    out.append({'k': 'cyl', 'b': INDEX['bowl'], 'g': 'bowl', 'c': 'bowl', 'p': [0, 0, 0.0], 'p1': [0, 0, 0.06], 'r': [0.08, 0.105]})
    out.append({'k': 'ell', 'b': INDEX['bowl'], 'g': 'milk', 'c': 'belly', 'p': [0, 0, 0.058], 'q': [0, 0, 0, 1], 'r': [0.092, 0.092, 0.006]})
    out.append({'k': 'ell', 'b': INDEX['yarn'], 'g': 'yarn', 'c': 'bowl', 'p': [0, 0, 0], 'q': [0, 0, 0, 1], 'r': [0.055, 0.055, 0.055]})
    for e in out:
        for k in ('p', 'p1', 'q', 'r', 'fa'):
            if k in e:
                e[k] = [r6(v) for v in e[k]]
        if 'st' in e:
            e['st'] = r6(e['st'])
    return out


# ------------------------------------------------------------------ clips
def hermite(p0, m0, p1, m1, u):
    u2, u3 = u * u, u * u * u
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1


def reduce_keys(v, tol, loop):
    """CUBICSPLINE keys (frame indices) for samples v (n x c): a Hermite through them with finite-difference tangents
    stays within tol of every sample."""
    n = len(v)
    d = np.zeros_like(v)
    if n > 2:
        d[1:-1] = (v[2:] - v[:-2]) / 2
    d[0] = v[min(1, n - 1)] - v[0]
    d[-1] = v[-1] - v[max(0, n - 2)]
    if loop and n > 2:                                           # a loop's ends meet smoothly
        d[0] = d[-1] = (v[1] - v[-2]) / 2
    keys = [0, n - 1] if n > 1 else [0]
    while len(keys) > 1:
        worst, at = 0.0, -1
        for a, b in zip(keys, keys[1:]):
            L = b - a
            if L < 2:
                continue
            u = (np.arange(a + 1, b) - a)[:, None] / L
            e = np.max(np.abs(hermite(v[a][None], d[a][None] * L, v[b][None], d[b][None] * L, u) - v[a + 1:b]), axis=1)
            j = int(np.argmax(e))
            if e[j] > worst:
                worst, at = e[j], a + 1 + j
        if worst <= tol or at < 0:
            break
        keys = sorted(keys + [at])
    return keys, d


def reduce_linear(v, tol):
    """LINEAR keys: straight segments within tol of every sample."""
    n = len(v)
    keys = [0, n - 1] if n > 1 else [0]
    while len(keys) > 1:
        worst, at = 0.0, -1
        for a, b in zip(keys, keys[1:]):
            if b - a < 2:
                continue
            u = (np.arange(a + 1, b) - a)[:, None] / (b - a)
            e = np.max(np.abs(v[a][None] + (v[b] - v[a])[None] * u - v[a + 1:b]), axis=1)
            j = int(np.argmax(e))
            if e[j] > worst:
                worst, at = e[j], a + 1 + j
        if worst <= tol or at < 0:
            break
        keys = sorted(keys + [at])
    return keys


def pack(fmt, vals):
    return base64.b64encode(struct.pack('<' + fmt * len(vals), *vals)).decode()


def q16(x, k):
    return int(max(-32767, min(32767, round(x * k))))


def q8(x):
    return int(max(-127, min(127, round(x * QR8))))


def channel(node, path, v, loop):
    """One glTF channel + sampler from per-frame samples: STEP for a held value, else LINEAR or CUBICSPLINE, whichever
    is smaller; outputs int16 (t, s at 1/4096; r normalised), or int8 normalised for a soft node's rotation."""
    group = node.split('.')[0].rstrip('0123456789')
    soft = path == 'r' and group in SOFT
    tol = TOL_SOFT_R if soft else TOL_LEG_R if path == 'r' and group in LEGS else TOL_FACE_S if path == 's' and group in SOFT else TOL[path]
    kin = 'B' if FRAMES[0] < 256 else 'H'                       # key frames: one byte each in a clip under 256 frames
    k = {'t': QT, 'r': QR, 's': QS}[path]
    base = {'node': INDEX[node], 'path': path, 'ct': 'i8' if soft else 'i16'}
    enc = (lambda xs: pack('b', [q8(x) for x in xs])) if soft else (lambda xs: pack('h', [q16(x, k) for x in xs]))
    if np.max(np.abs(v - v[0])) <= tol * 0.5:                    # held: one STEP key
        return {**base, 'interp': 'STEP', 'in': pack(kin, [0]), 'out': enc(list(v[0]))}
    kl = reduce_linear(v, tol)
    lin = {**base, 'interp': 'LINEAR', 'in': pack(kin, kl), 'out': enc([x for i in kl for x in v[i]])}
    if soft:
        return lin
    kc, d = reduce_keys(v, tol, loop)
    cub = {**base, 'interp': 'CUBICSPLINE', 'in': pack(kin, kc),
           'out': enc([x for i in kc for x in list(d[i]) + list(v[i]) + list(d[i])])}  # in-tangent, value, out-tangent
    return cub if len(cub['out']) + len(cub['in']) < len(lin['out']) + len(lin['in']) else lin


def show_props(props):
    for name, objs in PROPS.items():
        ob = bpy.data.objects[name]
        if name in props:
            loc, rot = props[name]
            ob.location, ob.rotation_euler, ob.scale = loc, rot, (1, 1, 1)
        else:
            ob.location, ob.rotation_euler, ob.scale = (0, 0, 0), (0, 0, 0), (0, 0, 0)
    bpy.context.view_layer.update()


def sample_clip(name):
    """Dense samples of a clip, and what posed the rig at each frame: every pose bone's own location, rotation and
    scale, and each prop's."""
    fn, dur, loop, views, follow = cat_clips.CLIPS[name]
    n = int(round(dur * FPS)) + (0 if loop else 1)
    per = {nm: {'t': [], 'r': [], 's': []} for nm in NAMES}
    basis, props_at = [], []
    for i in range(n):
        pose, props, _ = fn(min(i / FPS, dur))
        props = dict(props or {})
        props.pop('_cam', None)
        cat_poses.apply(arm, pose)
        show_props(props)
        basis.append([list(pb.location) + list(pb.rotation_euler) + list(pb.scale) for pb in arm.pose.bones])
        props_at.append([list(bpy.data.objects[p].location) + list(bpy.data.objects[p].rotation_euler) + list(bpy.data.objects[p].scale) for p in PROPS])
        for nm in NAMES:
            t, r, s = trs(local_matrix(nm))
            if per[nm]['r']:
                q0 = per[nm]['r'][-1]
                if sum(a * b for a, b in zip(q0, r)) < 0:
                    r = [-x for x in r]
            per[nm]['t'].append(t)
            per[nm]['r'].append(r)
            per[nm]['s'].append(s)
    return {'samples': per, 'basis': basis, 'props': props_at}


FRAMES = [0]


def encode_clip(name, per):
    fn, dur, loop, views, follow = cat_clips.CLIPS[name]
    FRAMES[0] = len(per[NAMES[0]]['t'])
    chans = []
    for nm in NAMES:
        for path in ('t', 'r', 's'):
            v = np.array(per[nm][path], dtype=np.float64)
            rv = np.array(REST[nm][path])
            if path == 'r' and np.dot(v[0], rv) < 0:
                rv = -rv
            if np.max(np.abs(v - rv)) <= TOL[path] * 0.25:
                continue                                         # the rest pose all through: left out
            chans.append(channel(nm, path, v, loop))
    views = (views,) if isinstance(views, str) else tuple(views)
    a, b = cat_clips.CLIP_FAMILY[name]
    return {'dur': r6(dur), 'loop': bool(loop), 'from': a, 'to': b, 'view': VIEW_YAW[views[0]],
            'follow': bool(follow is True), 'frames': len(per[NAMES[0]]['t']), 'ch': chans}


def rest_pose():
    cat_poses.reset(arm)
    show_props({})
    out = {}
    for nm in NAMES:
        t, r, s = trs(local_matrix(nm))
        out[nm] = {'t': t, 'r': r, 's': s}
    return out


# ------------------------------------------------------------------ the route map
MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised']


def graph():
    edges = {f'{a}>{b}': c for (a, b), c in cat_clips.FAMILY_EDGES.items()}
    return {'families': ['sit', 'stand', 'loaf', 'crouch', 'walk'], 'rest': cat_clips.FAMILY_REST, 'edges': edges,
            'blend': 0.3, 'center': [INDEX['pelvis'], INDEX['head']]}


def contacts():
    """The paws' contact points, on their nodes: the front toe tips (the hand bone's tail) and the hind balls (the toe bone's head)."""
    out = []
    for s in 'LR':
        out.append([INDEX[f'hand.{s}'], [0, r6(arm.data.bones[f'hand.{s}'].length), 0]])
        out.append([INDEX[f'toe.{s}'], [0, 0, 0]])
    return out


REST = None


def main():
    global REST
    REST = rest_pose()
    rec = {'fps': FPS, 'names': NAMES, 'parents': [parent_of(n) for n in NAMES], 'rest': REST, 'pose_bones': [pb.name for pb in arm.pose.bones],
           'rotation_modes': [pb.rotation_mode for pb in arm.pose.bones], 'prop_objects': list(PROPS),
           'shapes': shapes(), 'contacts': contacts(), 'graph': graph(), 'palette': M.PALETTE,
           'bone_lengths': {b.name: b.length for b in arm.data.bones}, 'clips': {}}
    out = os.path.join(HERE, 'record.json')
    for name in cat_clips.CLIPS:
        fn, dur, loop, views, follow = cat_clips.CLIPS[name]
        views = (views,) if isinstance(views, str) else tuple(views)
        a, b = cat_clips.CLIP_FAMILY[name]
        rec['clips'][name] = {'dur': dur, 'loop': bool(loop), 'view': views[0], 'follow': bool(follow is True), 'from': a, 'to': b, **sample_clip(name)}
        print('[record] sampled', name, flush=True)
    with open(out, 'w') as f:
        json.dump(rec, f, separators=(',', ':'))
    print('[record] wrote', out, os.path.getsize(out), 'bytes')


main()
