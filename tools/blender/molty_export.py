"""The Blender exporter: a .blend into a Molty's body file (docs/CREATURE.md, "How we make ours").

Run:  blender -b <file>.blend -P tools/blender/molty_export.py -- --out <Molty folder>/<name>.glb [--actions idle,blink]
Out:  one .glb: the skeleton, the shapes that are drawn, and every action sampled once a frame.

This is one way to make a body file, the one we use for the cat. The Molty tools (packages/tools) never run it and
never need Blender: they read the .glb, whatever wrote it. Needs Blender 5.3.

What it reads from the .blend:
  the skeleton   the one armature: every deforming bone, and any bone with one below it (controls such as IK
                 targets are left out: the poses they make are what gets sampled)
  props          an object that is not parented and has shapes as children (a bowl): a bone of its own after the
                 others, hidden (scaled to nothing) while no action shows it
  shapes         every Empty with a custom property `shape` (blob, ellipsoid, capsule, cone, cylinder, patch), the
                 child of a bone or of a prop, in name order. Its location is its centre (or first end) in the bone's
                 own frame; a blob, an ellipsoid and a patch take their turn and their three radii from its rotation
                 and scale. Custom properties say the rest: color, outline, and to / radius / flat / flat_axis /
                 stiffness where the shape has them
  feet           a bone with the custom property foot = "tip" touches the floor at its tip
  actions        every action, by name, in the order of its custom property `order` (then by name). Its manual
                 frame range is its length (the end may fall between two frames); Cyclic Animation marks a loop,
                 written with its first pose again at the end. Custom properties view (front, side, three_quarter),
                 follow and stride go out with it

Coordinates: the file's y is up and the Molty faces +z (its left is +x), glTF's own: world_file = C . world_blender,
C = (x, z, -y). A bone's local frame is Blender's bone frame, so a node's transform relative to its parent is the same
in both; only a root's is turned by C.
"""
import json
import math
import struct
import sys

import bpy
import numpy as np
from mathutils import Matrix

NEEDS = (5, 3)
ARGS = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))       # blender -> file


def fail(message):
    print('molty_export: ' + message, file=sys.stderr, flush=True)
    sys.exit(1)


def opt(name, default=None):
    return ARGS[ARGS.index(name) + 1] if name in ARGS and ARGS.index(name) + 1 < len(ARGS) else default


if bpy.app.version[:2] != NEEDS:
    fail('needs Blender %d.%d; this is %s. Poses are sampled from the rig as that version solves it, so another one may give another file' % (*NEEDS, bpy.app.version_string))
OUT, ONLY = opt('--out'), opt('--actions')
if not OUT:
    fail('say where the body file goes: -- --out <Molty folder>/<name>.glb')
arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
if len(arms) != 1:
    fail('the file has %d armatures: a Molty has one skeleton' % len(arms))
arm = arms[0]
scene = bpy.context.scene
FPS = scene.render.fps


def f32(x):
    """A single-precision number as its shortest text: 0.43, not 0.4300000071525574."""
    return float(str(np.float32(x)))


def r6(v):
    return float(f'{v:.6g}')


# ------------------------------------------------------------------ the skeleton, the props, the shapes
def carries(bone):
    return bone.use_deform or any(carries(c) for c in bone.children)


SHAPES = sorted((o for o in bpy.data.objects if 'shape' in o.keys()), key=lambda o: o.name)
PROPS = sorted({o.parent for o in SHAPES if o.parent is not None and o.parent != arm}, key=lambda o: o.name)
for p in PROPS:
    if p.parent is not None:
        fail('prop %s is parented to %s: a prop stands by itself' % (p.name, p.parent.name))
NAMES = [b.name for b in arm.data.bones if carries(b)] + [p.name for p in PROPS]
INDEX = {n: i for i, n in enumerate(NAMES)}
PROP = {p.name: p for p in PROPS}
# a prop's rest is how the file was saved: hidden, for ours
PROP_REST = {p.name: (tuple(p.location), tuple(p.rotation_euler), tuple(p.scale)) for p in PROPS}


def parent_of(name):
    if name in PROP:
        return -1
    p = arm.data.bones[name].parent
    while p is not None and p.name not in INDEX:
        p = p.parent
    return INDEX[p.name] if p is not None else -1


def world_matrix(name, posed=True):
    if name in PROP:
        return C @ PROP[name].matrix_world
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


def to_rest():
    """No action on anything: the bones in their rest pose, the props as the file has them."""
    for ob in [arm] + PROPS:
        if ob.animation_data:
            ob.animation_data.action = None
    for pb in arm.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    for p in PROPS:
        p.location, p.rotation_euler, p.scale = PROP_REST[p.name]
    bpy.context.view_layer.update()


def shape_node(ob):
    """A shape as a glTF node under its bone: where it is, and in `extras` what it is."""
    if ob.parent == arm and (ob.parent_type != 'BONE' or ob.parent_bone not in INDEX):
        fail('shape %s must be the child of a bone that is exported (or of a prop)' % ob.name)
    kind = ob['shape']
    extras = {'shape': kind, 'color': ob.get('color'), 'outline': ob.get('outline')}
    node = {'name': ob.name, 'translation': [f32(x) for x in ob.location]}
    if kind in ('blob', 'ellipsoid', 'patch'):
        q = ob.rotation_quaternion
        node['rotation'] = [f32(q.x), f32(q.y), f32(q.z), f32(q.w)]
        node['scale'] = [f32(x) for x in ob.scale]
    for key in ('stiffness', 'to', 'radius', 'flat', 'flat_axis'):
        if key in ob.keys():
            v = ob[key]
            extras[key] = list(v) if hasattr(v, '__len__') else v
    node['extras'] = extras
    return (INDEX[ob.parent_bone] if ob.parent == arm else INDEX[ob.parent.name]), node


# ------------------------------------------------------------------ the actions
def slot_for(action, ob):
    return next((s for s in action.slots if s.identifier == 'OB' + ob.name), None)


def frames_of(action):
    """The frames an action is sampled at: one a frame from its start, and its end when that falls between two; a
    loop stops one short of its end (its first pose again)."""
    a, b = (action.frame_start, action.frame_end) if action.use_frame_range else action.frame_range
    whole = [a + i for i in range(int(math.floor(b - a + 1e-6)) + 1)]
    if action.use_cyclic:
        return [f for f in whole if f < b - 1e-6], a, b
    return whole + ([b] if b - whole[-1] > 1e-6 else []), a, b


def sample(action):
    """Per node t, r (one side of the quaternion sphere all through), s at every frame, and the frames' times."""
    to_rest()
    for ob in [arm] + PROPS:
        s = slot_for(action, ob)
        if s is not None:
            ob.animation_data_create()
            ob.animation_data.action = action
            ob.animation_data.action_slot = s
    frames, start, end = frames_of(action)
    per = {nm: {'t': [], 'r': [], 's': []} for nm in NAMES}
    for f in frames:
        whole = int(math.floor(f))
        scene.frame_set(whole, subframe=f - whole)
        for nm in NAMES:
            t, r, s = trs(local_matrix(nm))
            if per[nm]['r']:
                q0 = per[nm]['r'][-1]
                if sum(a * b for a, b in zip(q0, r)) < 0:
                    r = [-x for x in r]
            per[nm]['t'].append(t)
            per[nm]['r'].append(r)
            per[nm]['s'].append(s)
    times = [(f - start) / FPS for f in frames]
    if action.use_cyclic:                                        # a loop ends on the pose it starts with
        times.append((end - start) / FPS)
        for nm in NAMES:
            for k in 'trs':
                per[nm][k].append(per[nm][k][0])
    return per, times


class Data:
    """The .glb's one buffer: floats, an accessor for each run."""

    def __init__(self):
        self.bytes = bytearray()
        self.accessors = []

    def put(self, rows, kind):
        flat = [x for row in rows for x in row] if kind != 'SCALAR' else list(rows)
        at = len(self.bytes)
        self.bytes += struct.pack('<%df' % len(flat), *flat)
        acc = {'bufferView': 0, 'byteOffset': at, 'componentType': 5126, 'count': len(rows), 'type': kind}
        if kind == 'SCALAR':                                     # glTF wants the range of an animation's times
            acc['min'], acc['max'] = [f32(min(flat))], [f32(max(flat))]
        self.accessors.append(acc)
        return len(self.accessors) - 1


def main():
    to_rest()
    scene.frame_set(0)
    rest = {nm: trs(local_matrix(nm)) for nm in NAMES}
    nodes = []
    for nm in NAMES:
        t, r, s = rest[nm]
        nodes.append({'name': nm, 'translation': [f32(x) for x in t], 'rotation': [f32(x) for x in r], 'scale': [f32(x) for x in s]})
        if nm not in PROP and arm.data.bones[nm].get('foot') == 'tip':
            nodes[-1]['extras'] = {'foot': [0, r6(arm.data.bones[nm].length), 0]}
    roots = []
    for i, nm in enumerate(NAMES):
        p = parent_of(nm)
        if p < 0:
            roots.append(i)
        else:
            nodes[p].setdefault('children', []).append(i)
    for ob in SHAPES:
        on, node = shape_node(ob)
        nodes[on].setdefault('children', []).append(len(nodes))
        nodes.append(node)

    actions = sorted(bpy.data.actions, key=lambda a: (a.get('order', 1 << 30), a.name))
    if ONLY:
        missing = [n for n in ONLY.split(',') if n not in bpy.data.actions]
        if missing:
            fail('no action named %s (actions: %s)' % (', '.join(missing), ', '.join(a.name for a in actions)))
        actions = [a for a in actions if a.name in ONLY.split(',')]
    data, animations = Data(), []
    for action in actions:
        if slot_for(action, arm) is None:
            continue                                             # an action for something else
        per, times = sample(action)
        all_times, first_time = data.put(times, 'SCALAR'), None
        channels, samplers = [], []
        for nm in NAMES:
            for path, key, kind in (('translation', 't', 'VEC3'), ('rotation', 'r', 'VEC4'), ('scale', 's', 'VEC3')):
                rows = per[nm][key]
                if all(row == rows[0] for row in rows):          # it never moves: one key, or none when that is its rest
                    if rows[0] == rest[nm]['trs'.index(key)]:
                        continue
                    if first_time is None:
                        first_time = data.put(times[:1], 'SCALAR')
                    samplers.append({'input': first_time, 'output': data.put(rows[:1], kind), 'interpolation': 'STEP'})
                else:
                    samplers.append({'input': all_times, 'output': data.put(rows, kind), 'interpolation': 'LINEAR'})
                channels.append({'sampler': len(samplers) - 1, 'target': {'node': INDEX[nm], 'path': path}})
        extras = {k: action[k] for k in ('view', 'follow', 'stride') if k in action.keys()}
        animations.append({'name': action.name, 'channels': channels, 'samplers': samplers, **({'extras': extras} if extras else {})})
        print('[molty_export] sampled', action.name, len(times), 'frames', flush=True)
    to_rest()

    doc = {'asset': {'version': '2.0', 'generator': 'molty_export.py (Blender %d.%d)' % NEEDS}, 'scene': 0, 'scenes': [{'nodes': roots}],
           'nodes': nodes, 'animations': animations, 'accessors': data.accessors,
           'bufferViews': [{'buffer': 0, 'byteLength': len(data.bytes)}], 'buffers': [{'byteLength': len(data.bytes)}]}
    text = json.dumps(doc, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    body = bytes(data.bytes) + b'\0' * (-len(data.bytes) % 4)
    with open(OUT, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(body)))
        f.write(struct.pack('<II', len(text), 0x4E4F534A) + text)
        f.write(struct.pack('<II', len(body), 0x004E4942) + body)
    print('[molty_export] wrote', OUT, 12 + 16 + len(text) + len(body), 'bytes:', len(NAMES), 'bones,', len(SHAPES), 'shapes,', len(animations), 'actions')


main()
