"""One-off: make cat.blend the cat's whole source. Writes into it what the cat's Python scripts used to add at
export time (recorded by record.py from those scripts): every clip as an action, the shapes the device draws as
Empties on their bones, the feet, the props hidden at rest. After this the scripts are not needed to build the cat.

Run:  blender -b cat.blend -P record.py   (writes record.json beside it: about half an hour)
      blender -b cat.blend -P bake_blend.py -- record.json <out.blend>
"""
import json
import sys

import bpy
from mathutils import Matrix

REC, OUT = sys.argv[sys.argv.index('--') + 1:]
rec = json.load(open(REC))
FPS = rec['fps']
arm = bpy.data.objects['CatRig']
scene = bpy.context.scene
KIND = {'blob': 'blob', 'ell': 'ellipsoid', 'cap': 'capsule', 'cone': 'cone', 'cyl': 'cylinder', 'patch': 'patch'}

# 1. the actions baked for the render sheets (another frame count, props by visibility) go
for ob in bpy.data.objects:
    if ob.animation_data:
        ob.animation_data_clear()
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)
for pb in arm.pose.bones:
    pb.matrix_basis = Matrix.Identity(4)
scene.render.fps = FPS
scene.frame_start = 0
scene.frame_set(0)

# 2. the props rest hidden, as the exporter always had them outside their clip
props = [bpy.data.objects[p] for p in rec['prop_objects']]
for p in props:
    p.location, p.rotation_euler, p.scale = (0, 0, 0), (0, 0, 0), (0, 0, 0)
    p.hide_render = False

# 3. every clip an action: what posed the rig at each frame, keyed on that frame
IDENT = [0, 0, 0, 0, 0, 0, 1, 1, 1]
HIDDEN = [0, 0, 0, 0, 0, 0, 0, 0, 0]
PATHS = (('location', 0), ('rotation_euler', 3), ('scale', 6))


def curves(act, ob, prefix, rows, frames, rest):
    """An fcurve for each of the 9 numbers that ever leaves its rest value."""
    for path, at in PATHS:
        for i in range(3):
            vals = [row[at + i] for row in rows]
            if all(v == rest[at + i] for v in vals):
                continue
            fc = act.fcurve_ensure_for_datablock(ob, prefix + path, index=i)
            fc.keyframe_points.add(len(vals))
            fc.keyframe_points.foreach_set('co', [x for f, v in zip(frames, vals) for x in (f, v)])
            for k in fc.keyframe_points:
                k.interpolation = 'LINEAR'
            fc.update()


arm.animation_data_create()
for p in props:
    p.animation_data_create()
for order, (name, c) in enumerate(rec['clips'].items()):
    n, end = len(c['basis']), c['dur'] * FPS
    frames = [i if i <= end else end for i in range(n)]          # the last pose of a clip that ends between two frames sits at its end
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    arm.animation_data.action = act
    for b, bone in enumerate(rec['pose_bones']):
        curves(act, arm, 'pose.bones["%s"].' % bone, [frame[b] for frame in c['basis']], frames, IDENT)
    arm.animation_data.action = None
    for k, p in enumerate(props):
        rows = [frame[k] for frame in c['props']]
        if any(row != HIDDEN for row in rows):
            p.animation_data.action = act
            curves(act, p, '', rows, frames, HIDDEN)
            p.animation_data.action = None
    act.use_frame_range = True
    act.frame_start, act.frame_end = 0, end
    act.use_cyclic = c['loop']
    act['order'] = order
    act['view'] = c['view']
    if c['follow']:
        act['follow'] = True
    if name in ('walk',):
        act['stride'] = 1.0                                      # a stride of the walk: it is joined and left on one
    print('[bake] action', name, n, 'frames', flush=True)
for pb in arm.pose.bones:
    pb.matrix_basis = Matrix.Identity(4)

# 4. the shapes the device draws: an Empty each, on its bone (its own frame, from the bone's head) or on its prop
shapes = bpy.data.collections.new('shapes (exported)')
scene.collection.children.link(shapes)
for k, s in enumerate(rec['shapes']):
    e = bpy.data.objects.new('shape.%03d.%s' % (k, s['g']), None)
    shapes.objects.link(e)
    on = rec['names'][s['b']]
    if on in rec['prop_objects']:
        e.parent = bpy.data.objects[on]
    else:
        e.parent, e.parent_type, e.parent_bone = arm, 'BONE', on
        e.matrix_parent_inverse = Matrix.Translation((0, -arm.data.bones[on].length, 0))
    e.location = s['p']
    e['shape'], e['color'], e['outline'] = KIND[s['k']], s['c'], s['g']
    e.empty_display_type, e.empty_display_size = 'SPHERE', 1.0
    if s['k'] in ('blob', 'ell', 'patch'):
        e.rotation_mode = 'QUATERNION'
        e.rotation_quaternion = (s['q'][3], s['q'][0], s['q'][1], s['q'][2])
        e.scale = s['r']
    else:
        e.empty_display_size = max(s['r'][0], 0.01)
        e['to'] = s['p1']
    if s['k'] == 'blob':
        e['stiffness'] = s['st']
    if s['k'] in ('cap', 'cyl'):
        e['radius'] = s['r']
    if s['k'] == 'cone':
        e['radius'], e['flat'], e['flat_axis'] = s['r'][0], s['r'][1], s['fa']

# 5. the feet: the front paws touch the floor at the hand bone's tip (the hind ones at the toe bone's start, the default)
for i, point in rec['contacts']:
    if any(point):
        arm.data.bones[rec['names'][i]]['foot'] = 'tip'

bpy.context.view_layer.update()
bpy.context.preferences.filepaths.save_version = 0
bpy.ops.wm.save_as_mainfile(filepath=OUT, compress=True)
print('[bake] wrote', OUT)
