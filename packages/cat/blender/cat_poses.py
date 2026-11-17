"""Posing for the cat rig (cat.blend): the base standing and sitting poses and apply(), which poses
the rig from a pose dict. The clips (cat_clips.py) build on these.

A pose is a dict:
  pelvis: (forward, up) position of the pelvis head, plus 'pitch' (deg, nose-up positive)
  spine/spine2/chest/neck/head: pitch of each bone's direction (deg from forward-horizontal; 90 = up)
  turn: optional {bone: (yaw, roll)}; ears, lids, jaw, tongue, fluff: optional face and tail controls
  scapula (optional): pitch of both shoulder blades, which slide to set the shoulder joint
  tail: one armature-space direction per tail segment (sag() for side-plane curls, V3() for 3D)
  hands/feet: per side, paw IK targets on the floor. hands: (forward, up, side, paw_pitch);
        feet: ball (forward, up, side), hock (forward, up), paw_pitch
"""
import math

from mathutils import Matrix, Vector


def F(f, z, x=0.0):
    return Vector((x, -f, z))


def sag(deg):
    """Direction in the sagittal plane, degrees from forward-horizontal."""
    a = math.radians(deg)
    return Vector((0, -math.cos(a), math.sin(a)))


def V3(x, f, z):
    """Direction from (side, forward, up); side -1 is the cat's right."""
    return Vector((x, -f, z)).normalized()


def head3(heading, elev):
    h, e = math.radians(heading), math.radians(elev)
    return Vector((-math.sin(h) * math.cos(e), -math.cos(h) * math.cos(e), math.sin(e)))


# ------------------------------------------------------------------ posing helpers
def _update():
    import bpy
    bpy.context.view_layer.update()


def reset(arm):
    for pb in arm.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    _update()


def aim(arm, name, direction, head=None):
    """FK: point bone `name` along `direction` (armature space), keeping its side axis level."""
    pb = arm.pose.bones[name]
    rest = arm.data.bones[name].matrix_local
    y = direction.normalized()
    rx, rz = rest.col[0].xyz, rest.col[2].xyz
    if abs(rx.dot(y)) < 0.7:  # keep the side axis level (sagittal bones)
        x = (rx - y * rx.dot(y)).normalized()
        z = x.cross(y)
    else:  # the bone turns sideways (a tail sweeping round): keep its up axis instead
        z = (rz - y * rz.dot(y)).normalized()
        x = y.cross(z)
    m = Matrix((x, y, z)).transposed().to_4x4()
    m.translation = head if head is not None else pb.head
    pb.matrix = m
    _update()


def place(arm, name, pos, pitch=0.0):
    """Put a control bone at `pos`, its rest orientation pitched nose-up by `pitch` degrees."""
    pb = arm.pose.bones[name]
    rest = arm.data.bones[name].matrix_local
    m = Matrix.Rotation(math.radians(-pitch), 4, 'X') @ rest.to_3x3().to_4x4()
    m.translation = pos
    pb.matrix = m
    _update()


def orient(arm, name, pitch, yaw=0.0, roll=0.0, head=None):
    """FK for the body chain and the head: pitch the bone in the side plane (as aim does), then roll
    it about the forward axis and turn it about the vertical (yaw > 0 turns to the cat's left)."""
    pb = arm.pose.bones[name]
    rest = arm.data.bones[name].matrix_local
    ry = rest.col[1].xyz
    a0 = math.degrees(math.atan2(ry.z, -ry.y))
    rot = (Matrix.Rotation(math.radians(yaw), 4, 'Z') @ Matrix.Rotation(math.radians(roll), 4, 'Y')
           @ Matrix.Rotation(math.radians(-(pitch - a0)), 4, 'X') @ rest.to_3x3().to_4x4())
    rot.translation = head if head is not None else pb.head
    pb.matrix = rot
    _update()


def apply(arm, p):
    """Pose the rig from a pose dict (armature space = world; the root bone carries root motion)."""
    reset(arm)
    place(arm, 'root', F(p.get('root', 0.0), 0))
    turn = p.get('turn', {})
    pf, pz = p['pelvis']
    orient(arm, 'pelvis', p['pitch'], *turn.get('pelvis', (0, 0)), head=F(pf, pz))
    for name in ('spine', 'spine2', 'chest', 'neck', 'head'):  # the neck keeps its rest offset on the chest
        orient(arm, name, p.get(name, p['spine']), *turn.get(name, (0, 0)))
    if 'scapula' in p:  # the shoulder blade slides: aim it to set the shoulder joint
        for s in ('L', 'R'):
            aim(arm, f'scapula.{s}', sag(p['scapula']))
    for i, d in enumerate(p['tail']):
        aim(arm, f'tail{i + 1}', d)
    for s in ('L', 'R'):
        hf, hz, hx, hp = p['hands'][s]
        place(arm, f'ik_hand.{s}', F(hf, hz, hx), hp)
        (bf, bz, bx), (kf, kz), fp = p['feet'][s]
        place(arm, f'ik_foot.{s}', F(bf, bz, bx), fp)
        place(arm, f'hock_target.{s}', F(kf, kz, bx))
        if 'elbow' in p:
            place(arm, f'pole_elbow.{s}', F(*p['elbow'], hx))
        if 'knee' in p:
            place(arm, f'pole_knee.{s}', F(*p['knee'], bx))
    face(arm, p)


def face(arm, p):
    """Ears (flatten back, swivel), eyelids, the mouth and the tongue: local rotations and scales."""
    pb = arm.pose.bones
    for s, d in (('L', 1), ('R', -1)):
        flat, swivel = p.get('ears', {}).get(s, (0.0, 0.0))
        pb[f'ear.{s}'].rotation_euler = (math.radians(-flat * 0.8), math.radians(swivel * d), math.radians(-flat * 0.5 * d))
        lid = min(1.0, max(-0.4, p.get('lids', 0.0)))  # < 0 widens the eyes (a startle)
        k = 1 - min(0.0, lid) * 0.8
        pb[f'eye.{s}'].scale = (k, max(0.06, 1 - max(0.0, lid)) * k, k)
    jaw = max(0.0, p.get('jaw', 0.0))
    pb['mouth'].scale = (max(1e-3, jaw ** 0.7),) * 3
    tg = max(0.0, p.get('tongue', 0.0))
    pb['tongue'].scale = (max(1e-3, min(1.0, tg * 3)), max(1e-3, tg * 1.6), max(1e-3, min(1.0, tg * 3)))
    pb['tongue'].rotation_euler = (math.radians(-35 * p.get('tongue_curl', 0.0)), 0, 0)
    fluff = p.get('fluff', 0.0)  # a bottle-brush tail: the segments inherit the root's thickness
    if fluff:
        pb['tail1'].scale = (1 + fluff, 1.0, 1 + fluff)
    _update()


def reach_errors(arm):
    """How far each IK chain falls short of its target (0 = reached)."""
    pb = arm.pose.bones
    out = {}
    for s in ('L', 'R'):
        out['hand.' + s] = (pb[f'forearm.{s}'].tail - pb[f'ik_hand.{s}'].head).length
        out['hock.' + s] = (pb[f'shin.{s}'].tail - pb[f'hock_target.{s}'].head).length
        out['foot.' + s] = (pb[f'foot.{s}'].tail - pb[f'ik_foot.{s}'].head).length
    return out


def tail_sag(angles):
    return [sag(a) for a in angles]


# ------------------------------------------------------------------ the poses
def standing():
    return dict(
        pelvis=(-0.36, 0.43), pitch=0, spine=0, spine2=0, chest=3.6, neck=53, head=90,
        tail=tail_sag([152, 136, 123, 113, 105, 99, 95, 93, 95, 101, 112, 126]),  # rising from the top rear, curving up
        # the near (right) legs a little apart from the far ones so both of each pair show from the side
        hands={'R': (0.265, 0.07, -0.07, 0), 'L': (0.215, 0.07, 0.07, 0)},
        feet={'R': ((-0.32, 0.03, -0.085), (-0.34, 0.15), 0), 'L': ((-0.37, 0.03, 0.085), (-0.39, 0.15), 0)},
    )


def sitting():
    # the tail drops behind the rump, swings out to the right along the floor, then rises beside the
    # body and hooks outward at the tip (the old cat's "long tail curls up behind it")
    tail = [V3(*v) for v in ((0, -0.4, -1), (-0.6, -1, -0.15), (-1, -0.3, 0), (-1, 0, 0), (-0.8, -0.3, 0.6), (-0.3, -0.45, 1),
                             (-0.05, -0.3, 1), (0, -0.05, 1), (-0.35, 0.15, 0.9), (-0.85, 0.1, 0.5), (-1, 0, -0.15), (-0.6, 0, -0.9))]
    return dict(
        # the long back rounds (lumbar up steep, thorax nearly level) so the shoulders sit over the front paws
        pelvis=(-0.36, 0.13), pitch=70, spine=55, spine2=10, chest=14, scapula=-88, neck=74, head=92,
        knee=(0.0, 0.32), elbow=(-0.1, 0.25),
        tail=tail,
        hands={'R': (0.05, 0.07, -0.06, 0), 'L': (0.02, 0.07, 0.06, 0)},  # near paw a touch ahead
        # the WHOLE hind foot flat on the floor, hock at the back of the haunch, toes pointing forward
        feet={'R': ((-0.19, 0.035, -0.105), (-0.31, 0.045), 0), 'L': ((-0.22, 0.035, 0.105), (-0.34, 0.045), 0)},
    )
