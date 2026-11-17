"""The cat's animation clips, as poses over time for the rig in cat.blend (posed by cat_poses.apply).

A clip is a function of time t (seconds) returning (pose, props, planted):
  pose     a cat_poses pose dict in world coordinates (the root bone carries root motion)
  props    {name: (location, rotation_euler)} for the props shown in this clip (bowl, yarn)
  planted  the paws on the floor at t ('F.L', 'F.R', 'H.L', 'H.R'), for the foot-slip check
Walking uses a footstep planner: each paw stays exactly where it landed until it lifts, and lands
where the body will be at mid-stance, so the body moves only by its steps (lateral sequence walk:
left hind, left fore, right hind, right fore, a quarter cycle apart, as in Muybridge's plates).
"""
import math

from mathutils import Matrix, Vector

import cat_poses as P
from cat_poses import F, sag

FPS = 24
TAU = 2 * math.pi


# ------------------------------------------------------------------ easing and keys
def clamp(x, a=0.0, b=1.0):
    return a if x < a else b if x > b else x


def smooth(x):
    x = clamp(x)
    return x * x * (3 - 2 * x)


def ramp(t, t0, t1):
    """0 before t0, 1 after t1, eased in and out between."""
    return smooth((t - t0) / (t1 - t0)) if t1 > t0 else float(t >= t1)


def bump(t, t0, t1):
    """0 -> 1 -> 0 over [t0, t1], eased (a smooth hill)."""
    if t <= t0 or t >= t1:
        return 0.0
    return math.sin(math.pi * (t - t0) / (t1 - t0)) ** 2


def lerp(a, b, w):
    if isinstance(a, dict):
        return {k: lerp(a[k], b[k], w) if k in a and k in b else (a.get(k, b.get(k))) for k in set(a) | set(b)}
    if isinstance(a, Vector):
        return a.lerp(b, w)
    if isinstance(a, (list, tuple)):
        out = [lerp(x, y, w) for x, y in zip(a, b)]
        return tuple(out) if isinstance(a, tuple) else out
    return a + (b - a) * w


def keys(t, ks):
    """Piecewise eased interpolation through [(time, value), ...] (holds outside)."""
    if t <= ks[0][0]:
        return ks[0][1]
    for (t0, v0), (t1, v1) in zip(ks, ks[1:]):
        if t <= t1:
            return lerp(v0, v1, smooth((t - t0) / (t1 - t0)))
    return ks[-1][1]


def blink(t, times, close=0.07, hold=0.04, open_=0.14):
    v = 0.0
    for b in times:
        if b <= t < b + close:
            v = max(v, smooth((t - b) / close))
        elif b + close <= t < b + close + hold:
            v = 1.0
        elif b + close + hold <= t < b + close + hold + open_:
            v = max(v, 1 - smooth((t - b - close - hold) / open_))
    return v


def flick(t, times, dur=0.25):
    """A quick out-and-back (ear twitch, tail tip flick): a decaying wiggle after each time."""
    v = 0.0
    for b in times:
        if b <= t < b + dur:
            u = (t - b) / dur
            v += math.sin(u * TAU * 1.5) * (1 - u)
    return v


# ------------------------------------------------------------------ the tail: shapes plus waves
def rot_z(v, deg):
    return Matrix.Rotation(math.radians(deg), 3, 'Z') @ v


def rot_side(v, deg):
    """Raise (deg > 0) a direction in the vertical plane that contains it."""
    h = Vector((v.x, v.y, 0))
    if h.length < 1e-6:
        return v
    axis = h.normalized().cross(Vector((0, 0, 1)))
    return Matrix.Rotation(math.radians(deg), 3, axis) @ v


def tail_wave(dirs, t, lat=0.0, lat_f=0.5, up=0.0, up_f=0.5, lag=0.55, tip=0.0):
    """Overlapping action: a wave travels root to tip (each segment later and bigger); `tip` adds a curl
    at the end. lat/up are amplitudes in degrees per segment at the tip."""
    n = len(dirs)
    out = []
    for i, d in enumerate(dirs):
        w = (i / (n - 1)) ** 1.3
        d = rot_z(d, lat * w * math.sin(TAU * lat_f * t - lag * i))
        d = rot_side(d, up * w * math.sin(TAU * up_f * t - lag * i) + tip * max(0, (i - n + 4)) / 3)
        out.append(d)
    return out


# ------------------------------------------------------------------ base poses
SIT = P.sitting()
STAND = P.standing()
STAND['scapula'] = -61.4                       # the rest angle, made explicit so poses can blend
STAND['knee'], STAND['elbow'] = (0.02, 0.26), (0.05, 0.24)
SIT_BODY = ('pelvis', 'pitch', 'spine', 'spine2', 'chest', 'neck', 'head', 'scapula', 'tail', 'knee', 'elbow')
HAND_OFF = Vector((-0.05, 0.055))               # wrist relative to the toe tip, paw flat (f, z)
MET = 0.122                                    # metatarsus length (hock to ball)
SIT_LEAN = 84.0                                # sitting: the metatarsus lies flat, the hock just behind on the floor


def shifted(p, dx):
    """The same pose moved forward by dx (body keys and poles; paws are placed separately)."""
    q = dict(p)
    q['root'] = p.get('root', 0.0) + dx
    q['pelvis'] = (p['pelvis'][0] + dx, p['pelvis'][1])
    for k in ('knee', 'elbow'):
        if k in p:
            q[k] = (p[k][0] + dx, p[k][1])
    return q


def hand(toe_f, toe_x, pitch=0.0, lift=0.0):
    """Wrist target for a front paw whose toe tip is at toe_f; the paw rolls about the toe (pitch < 0:
    heel up), so a planted toe never slides."""
    a = math.radians(pitch)
    of = HAND_OFF.x * math.cos(a) - HAND_OFF.y * math.sin(a)
    oz = HAND_OFF.x * math.sin(a) + HAND_OFF.y * math.cos(a)
    return (toe_f + of, 0.015 + lift + oz, toe_x, pitch)


def foot(ball_f, ball_x, lean=9.5, lift=0.0, toe=0.0):
    """Hind paw: ball on the floor (or lifted), the metatarsus leaning `lean` degrees back from vertical."""
    a = math.radians(lean)
    z = 0.03 + lift
    return ((ball_f, z, ball_x), (ball_f - MET * math.sin(a), z + MET * math.cos(a)), toe)


def toe_of(p, s):
    """Front toe tip from a pose's wrist target (paw flat)."""
    return p['hands'][s][0] - HAND_OFF.x


# neutral contacts relative to the standing body (near = right, a little ahead of the far paws)
NEUTRAL = {'F.R': (toe_of(STAND, 'R'), -0.07), 'F.L': (toe_of(STAND, 'L'), 0.07),
           'H.R': (STAND['feet']['R'][0][0], -0.105), 'H.L': (STAND['feet']['L'][0][0], 0.105)}
STAND['feet'] = {s: foot(NEUTRAL['H.' + s][0], NEUTRAL['H.' + s][1]) for s in 'LR'}
SIT['feet'] = {s: foot(SIT['feet'][s][0][0], -0.105 if s == 'R' else 0.105, SIT_LEAN, 0.005) for s in 'LR'}


# ------------------------------------------------------------------ walking: gait + footstep planner
T_CYCLE, DUTY, STRIDE = 1.0, 0.62, 0.22
V_WALK = STRIDE / (DUTY * T_CYCLE)
OFFSET = {'H.L': 0.0, 'F.L': 0.25, 'H.R': 0.5, 'F.R': 0.75}
LIFT = {'F': 0.075, 'H': 0.06}


class Steps:
    """One paw's footfalls: planted at `start` until its first lift, then (t_lift, t_land, target) steps."""

    def __init__(self, start):
        self.start, self.steps = start, []

    def add(self, t_lift, t_land, target, lift=None):
        self.steps.append((t_lift, t_land, target, lift))

    def at(self, t):
        """(position, swing fraction or None, lift height override)"""
        pos = self.start
        for t0, t1, target, lift in self.steps:
            if t < t0:
                return pos, None, None
            if t < t1:
                return pos + (target - pos) * smooth((t - t0) / (t1 - t0)), (t - t0) / (t1 - t0), lift
            pos = target
        return pos, None, None


def plan(x, t_go, t_end, phase0=0.0):
    """Footfalls for a body moving by x(t): each paw lifts on the gait schedule after t_go and lands
    where the body will be at mid-stance; steps that would not move the paw are skipped."""
    steps = {}
    for leg, (f0, _) in NEUTRAL.items():
        st = Steps(f0 + x(min(t_go, 0.0) if t_go > -1 else -50))
        k = math.floor((t_go - 1) / T_CYCLE) - 2
        while True:
            t_lift = t_go + (k + DUTY + OFFSET[leg] - phase0) * T_CYCLE
            k += 1
            if t_lift > t_end:
                break
            if t_lift < t_go:
                continue
            t_land = t_lift + (1 - DUTY) * T_CYCLE
            target = f0 + x(t_land + DUTY * T_CYCLE / 2)
            prev = st.steps[-1][2] if st.steps else st.start
            if abs(target - prev) > 0.012:
                st.add(t_lift, t_land, target)
        steps[leg] = st
    return steps


def paws_from(steps, x, t, speed=1.0, extra=None):
    """Paw targets (hands, feet) and the planted set from the footstep plan at t."""
    hands, feet, planted = {}, {}, set()
    for leg, st in steps.items():
        if extra and leg in extra:
            continue
        side = leg[-1]
        f0, px = NEUTRAL[leg]
        pos, s, _ = st.at(t)
        rel = pos - (x(t) + f0)                          # + ahead of neutral (early stance)
        if leg[0] == 'F':
            if s is None:
                peel = smooth((-rel / STRIDE - 0.1) / 0.35)  # late stance: the heel of the paw peels up
                hands[side] = hand(pos, px, -45 * peel)
                planted.add(leg)
            else:  # swing: the wrist keeps folding as the paw leaves, then opens to reach for the floor
                pk = (-45 - 35 * smooth(s / 0.3)) * (1 - smooth((s - 0.55) / 0.45))
                hands[side] = hand(pos, px, pk, LIFT['F'] * math.sin(math.pi * s) ** 0.8)
        else:
            if s is None:
                feet[side] = foot(pos, px, clamp(9.5 + 75 * rel / STRIDE * 0.8, -25, 32))
                planted.add(leg)
            else:
                feet[side] = foot(pos, px, -22 + 60 * smooth(s), LIFT['H'] * math.sin(math.pi * s) ** 0.8, -45 * math.sin(math.pi * s))
    return hands, feet, planted


def gait_body(base, t, amount=1.0):
    """The body while walking: dips twice a cycle, the spine sways, the head stays level, the tail is
    carried low in a soft S and swings a little behind the hips (follow-through)."""
    ph = t / T_CYCLE
    p = dict(base)
    p['pelvis'] = (base['pelvis'][0], base['pelvis'][1] - amount * (0.02 + 0.012 * math.sin(TAU * 2 * ph)))  # a touch lower when walking
    p['turn'] = {'pelvis': (amount * 3 * math.sin(TAU * ph), amount * 2.5 * math.sin(TAU * ph + 1.2)),
                 'chest': (amount * -2 * math.sin(TAU * ph), 0), 'head': (amount * 1.5 * math.sin(TAU * ph), 0)}
    p['head'] = base['head'] + amount * 1.5 * math.sin(TAU * 2 * ph + 0.8)
    p['tail'] = tail_wave(WALK_TAIL if amount > 0.5 else base['tail'], t, lat=amount * 9, lat_f=1 / T_CYCLE, up=amount * 4, up_f=2 / T_CYCLE)
    return p


WALK_TAIL = P.tail_sag([142, 128, 117, 109, 103, 99, 96, 94, 94, 98, 108, 122])  # rising from the top rear, as walking cats carry it


def walk_loop(t):
    x = lambda tt: V_WALK * tt
    steps = plan(x, -20.0, 40.0)
    body = gait_body(shifted(STAND, x(t)), t)
    hands, feet, planted = paws_from(steps, x, t)
    body.update(hands=hands, feet=feet, ears={'L': (-0.1 * math.sin(TAU * t), 0), 'R': (0, 0)})
    body['lids'] = blink(t, [1.3])
    return body, {}, planted


# --- getting up from sitting and walking off; walking in and sitting down
D_RISE = SIT['feet']['R'][0][0] - STAND['feet']['R'][0][0]   # body shift that keeps the hind paws planted


def front_steps(order, t0, gap, dur, frm, to):
    """Front paws stepping from one pose's toes to another's, one after the other."""
    out = {}
    for i, s in enumerate(order):
        st = Steps(frm[s])
        st.add(t0 + i * gap, t0 + i * gap + dur, to[s], 0.06)
        out['F.' + s] = st
    return out


def walk_start(t):
    T_RISE0, T_RISE1, T_GO = 0.35, 1.2, 1.35
    acc = 0.7
    x_go = D_RISE

    def x(tt):  # body offset: still, then accelerating smoothly to walking speed
        if tt <= T_GO:
            return x_go
        u = tt - T_GO
        return x_go + (V_WALK * (u * u / (2 * acc)) if u < acc else V_WALK * (u - acc / 2))

    stand = shifted(STAND, x_go)
    w = ramp(t, T_RISE0, T_RISE1)
    # anticipation: the head dips and the chest settles before the rump lifts
    pre = bump(t, 0.1, 0.5)
    body = lerp({k: SIT[k] for k in SIT_BODY}, {k: stand[k] for k in SIT_BODY}, w)
    body['root'] = x_go * w
    if t > T_GO:
        g = ramp(t, T_GO, T_GO + 0.8)
        body = lerp(body, {k: gait_body(shifted(STAND, x(t)), t - T_GO)[k] for k in SIT_BODY}, g)
        body['turn'] = gait_body(shifted(STAND, x(t)), t - T_GO, g)['turn']
        body['root'] = x(t)
        body['pelvis'] = (STAND['pelvis'][0] + x(t), body['pelvis'][1])
    body['head'] = body['head'] - 8 * pre
    body['neck'] = body['neck'] - 4 * pre
    # front paws step out from the sitting spot to the standing spot as the body rises
    sit_toe = {s: toe_of(SIT, s) for s in 'LR'}
    stand_toe = {s: NEUTRAL['F.' + s][0] + x_go for s in 'LR'}
    fsteps = {}
    for s, t0 in (('R', 0.45), ('L', 0.6)):  # two short steps each, keeping the paws under the rising chest
        st = Steps(sit_toe[s])
        st.add(t0, t0 + 0.27, (sit_toe[s] + stand_toe[s]) / 2, 0.05)
        st.add(t0 + 0.47, t0 + 0.73, stand_toe[s], 0.06)
        fsteps['F.' + s] = st
    steps = plan(x, T_GO, 10.0, phase0=0.27)  # the right fore leads off, just after the body starts
    for leg in ('F.R', 'F.L'):  # the walk continues from where the getting-up steps left the paws
        fsteps[leg].steps += [s for s in steps[leg].steps]
    steps.update(fsteps)
    hands, feet, planted = paws_from(steps, x, t)
    # hind paws: planted where it sat; the hocks lift off the floor as the rump rises
    for s in 'LR':
        leg = 'H.' + s
        pos, sw, _ = steps[leg].at(t)
        if sw is None and not steps[leg].steps or t < (steps[leg].steps[0][0] if steps[leg].steps else 99):
            feet[s] = foot(pos, NEUTRAL[leg][1], SIT_LEAN + (9.5 - SIT_LEAN) * w, 0.005 * (1 - w))
    body.update(hands=hands, feet=feet)
    body['ears'] = {s: (-0.3 * ramp(t, 0.1, 0.4), 6 * flick(t, [0.15])) for s in 'LR'}
    body['tail'] = tail_wave(body['tail'], t, up=6 * (1 - w) * flick(t, [0.05]), lat=4)
    return body, {}, planted


def walk_stop(t):
    T_STOP0, DEC = 0.0, 1.1
    x_end = V_WALK * DEC / 2

    def x(tt):  # decelerating from walking speed to a stop
        if tt >= DEC:
            return x_end
        return V_WALK * (tt - tt * tt / (2 * DEC))

    T_SIT0, T_SIT1 = 1.9, 2.8
    steps = plan(x, -20.0, 2.0)
    stand = shifted(STAND, x_end)
    sit = shifted(SIT, x_end - D_RISE)
    g = 1 - ramp(t, 0.3, 1.5)
    walking = gait_body(shifted(STAND, x(t)), t, g)
    body = {k: walking[k] for k in SIT_BODY}
    body['turn'] = walking['turn']
    body['root'] = x(t)
    w = ramp(t, T_SIT0, T_SIT1)
    if w > 0:
        body = lerp(body, {k: sit[k] for k in SIT_BODY}, w)
        body['root'] = x_end + (x_end - D_RISE - x_end) * w
    # front paws walk back under the chest as it sits (one, then the other)
    stand_toe = {s: NEUTRAL['F.' + s][0] + x_end for s in 'LR'}
    sit_toe = {s: toe_of(SIT, s) + x_end - D_RISE for s in 'LR'}
    fsteps = {}
    for s, t0 in (('L', 1.95), ('R', 2.1)):  # two short steps back each, keeping the paws under the settling chest
        st = Steps(stand_toe[s])
        st.add(t0, t0 + 0.25, (stand_toe[s] + sit_toe[s]) / 2, 0.05)
        st.add(t0 + 0.3, t0 + 0.55, sit_toe[s], 0.04)
        fsteps['F.' + s] = st
    for leg in ('F.R', 'F.L'):
        last = steps[leg].steps
        fsteps[leg].start = steps[leg].start
        fsteps[leg].steps = last + fsteps[leg].steps
    steps.update(fsteps)
    hands, feet, planted = paws_from(steps, x, t)
    for s in 'LR':  # sitting down: the hind paws stay, the hocks settle to the floor behind them
        pos, sw, _ = steps['H.' + s].at(t)
        if sw is None and t > 1.6:
            feet[s] = foot(pos, NEUTRAL['H.' + s][1], 9.5 + (SIT_LEAN - 9.5) * w, 0.005 * w)
    body.update(hands=hands, feet=feet)
    body['tail'] = tail_wave(body['tail'], t, lat=6 * (1 - w), lat_f=0.8, up=5 * bump(t, 2.3, 3.2))
    body['lids'] = blink(t, [2.9])
    body['ears'] = {s: (0.0, 5 * flick(t, [3.0 if s == 'L' else 3.1])) for s in 'LR'}
    return body, {}, planted


# ------------------------------------------------------------------ eating from a bowl
BOWL_F = toe_of(SIT, 'R') + 0.19


def eat_pose():
    p = dict(SIT)
    p.update(pitch=60, spine=40, spine2=0, chest=-10, neck=-15, head=30, scapula=-55, elbow=(0.0, 0.38))
    return p


def eat(t):
    sit, eatp = {k: SIT[k] for k in SIT_BODY}, {k: eat_pose()[k] for k in SIT_BODY}
    down = ramp(t, 0.55, 1.3) * (1 - ramp(t, 3.9, 4.6))
    body = lerp(sit, eatp, down)
    look = ramp(t, 0.1, 0.5) * (1 - down)
    body['head'] = body['head'] - 18 * look
    lap = 0.0 if not (1.3 < t < 3.9) else max(0.0, math.sin(TAU * 3.2 * (t - 1.3)))
    body['head'] = body['head'] + 4 * lap * down
    body['neck'] = body['neck'] + 2 * lap * down
    body['tongue'] = 0.75 * lap + 0.9 * bump(t, 4.6, 5.0) + 0.8 * bump(t, 5.05, 5.4)
    body['tongue_curl'] = lap
    body['jaw'] = 0.35 * lap + 0.35 * bump(t, 4.55, 5.4)
    body['ears'] = {'L': (0.15 * down, 10 * flick(t, [2.1])), 'R': (0.15 * down, -8 * flick(t, [3.0]))}
    body['tail'] = tail_wave(body['tail'], t, lat=6, lat_f=0.4, tip=10 * down)
    body['lids'] = max(0.35 * down, blink(t, [0.3, 5.6]))
    body.update(hands=SIT['hands'], feet=SIT['feet'])
    props = {'bowl': (F(BOWL_F, 0.0, -0.035), (0, 0, 0))}
    return body, props, {'F.L', 'F.R', 'H.L', 'H.R'}


# ------------------------------------------------------------------ playing with a ball of yarn
YARN_R = 0.055
YARN_STOP = toe_of(SIT, 'R') + 0.3            # where the ball comes to rest, in front of the paws


def yarn_track(t):
    """The ball rolls in, stops in front of the cat, is pinned by the pounce, then batted away."""
    f_start, f_stop = YARN_STOP + 0.75, YARN_STOP
    if t < 1.1:
        f = f_start + (f_stop - f_start) * (1 - (1 - t / 1.1) ** 2)       # decelerating roll in
    elif t < 3.35:
        f = f_stop
    else:
        u = t - 3.35
        f = f_stop + 0.55 * (1 - math.exp(-u / 0.6))                         # batted: rolls away, slowing
    return f


def crouch():
    p = dict(SIT)
    p.update(pelvis=(SIT['pelvis'][0], 0.22), pitch=25, spine=0, spine2=0, chest=-6, neck=25, head=72, scapula=-85,
             knee=(0.0, 0.30), elbow=(-0.1, 0.35))
    return p


def yarn(t):
    # timeline: watch (0-1.3), crouch and wiggle (1.3-2.35), pounce (2.35-2.75), bat (3.0-3.4), watch it go
    T_CR0, T_CR1, T_P0, T_P1 = 1.25, 1.7, 2.35, 2.75
    jump = 0.2
    sit, cr = {k: SIT[k] for k in SIT_BODY}, {k: crouch()[k] for k in SIT_BODY}
    w = ramp(t, T_CR0, T_CR1)
    body = lerp(sit, cr, w)
    # the pounce: hind legs drive the body forward and a little up; it lands lower
    pj = ramp(t, T_P0, T_P1)
    air = bump(t, T_P0 + 0.05, T_P1)
    body = shifted(body, jump * pj)
    body['pelvis'] = (body['pelvis'][0], body['pelvis'][1] + 0.06 * air)
    body['root'] = jump * pj
    # bum wiggle before the pounce (anticipation), tail tip twitching
    wig = bump(t, 1.75, T_P0) * math.sin(TAU * 5 * t)
    body['turn'] = {'pelvis': (5 * wig, 4 * wig), 'spine': (2 * wig, 0)}
    yf = yarn_track(t)
    # the head tracks the ball (eyes on the prey), ears pricked forward
    body['head'] = body['head'] - 12 * ramp(t, 0.3, 1.0) * (1 - w) + 6 * bump(t, 3.4, 4.6)
    body['ears'] = {s: (-0.4 * ramp(t, 0.2, 0.6), 0) for s in 'LR'}
    body['tail'] = tail_wave(body['tail'], t, lat=10 * w, lat_f=1.6, up=8 * bump(t, T_P0, T_P1 + 0.3), tip=20 * bump(t, 1.6, 2.4))
    # paws: hind paws push off and land again; front paws reach for the ball; then the right paw bats it
    toes = {s: toe_of(SIT, s) for s in 'LR'}
    hands, feet, planted = {}, {}, set()
    for s in 'LR':
        st = Steps(toes[s])
        land = YARN_STOP - YARN_R - 0.05 - (0.04 if s == 'L' else 0.0)   # beside the ball where it stopped
        st.add(T_P0 + 0.08, T_P1, land, 0.1)
        if s == 'R':
            st.add(3.0, 3.4, land + 0.04, 0.09)    # the bat: up, forward through the ball, down
        pos, sw, lift = st.at(t)
        if sw is None:
            hands[s] = hand(pos, (-0.06 if s == 'R' else 0.06))
            planted.add('F.' + s)
        else:
            reach = -60 * math.sin(math.pi * sw) if s == 'L' or t < 2.9 else -80 * math.sin(math.pi * sw)
            hands[s] = hand(pos, (-0.06 if s == 'R' else 0.06), reach, (lift or 0.08) * math.sin(math.pi * sw))
        fs = Steps(SIT['feet'][s][0][0])
        fs.add(T_P0 + 0.2, T_P1 + 0.1, SIT['feet'][s][0][0] + jump, 0.05)
        pos, sw, _ = fs.at(t)
        ext = bump(t, T_P0 - 0.05, T_P0 + 0.35)   # the push: the hocks rise as the hind legs extend
        lean = SIT_LEAN - 40 * w - 30 * ext        # crouched: heels up off the floor, ready to spring
        feet[s] = foot(pos, SIT['feet'][s][0][2], lean, 0.005 + (0.04 * math.sin(math.pi * sw) if sw is not None else 0))
        if sw is None:
            planted.add('H.' + s)
    body.update(hands=hands, feet=feet)
    body['lids'] = blink(t, [0.8, 4.5])
    # the ball: rolls without slipping (turns by distance / radius); a little hop when batted
    roll = (yf - YARN_STOP - 0.75) / YARN_R  # moving toward the cat (+Y) the ball turns about -X
    hop = 0.03 * bump(t, 3.3, 3.55)
    props = {'yarn': (F(yf, YARN_R + hop, -0.02), (roll, 0, 0))}
    return body, props, planted


# ------------------------------------------------------------------ sitting gestures and moods
ALL4 = {'F.L', 'F.R', 'H.L', 'H.R'}


def sit_at(t, breath=1.0, sway=1.0):
    """The sitting cat, alive: it breathes (chest rising every 2 s), the tail tip sways."""
    p = dict(SIT)
    b = math.sin(TAU * 0.5 * t)
    p['spine'] = SIT['spine'] + 1.0 * breath * b
    p['chest'] = SIT['chest'] + 1.4 * breath * b
    p['head'] = SIT['head'] - 0.6 * breath * b
    p['tail'] = tail_wave(SIT['tail'], t, lat=5 * sway, lat_f=0.25, up=3 * sway, up_f=0.5)
    p['hands'], p['feet'] = SIT['hands'], SIT['feet']
    return p


def head_turn(p, yaw=0.0, roll=0.0, neck_share=0.35):
    """Turn the head; the neck takes a share so the motion is not a robot's."""
    t = dict(p.get('turn', {}))
    t['neck'] = (yaw * neck_share, roll * neck_share)
    t['head'] = (yaw, roll)
    p['turn'] = t
    return p


def ears(p, l=(0.0, 0.0), r=(0.0, 0.0)):
    p['ears'] = {'L': l, 'R': r}
    return p


def idle(t):
    p = sit_at(t)
    ears(p, (0, 22 * flick(t, [1.2], 0.3)), (0, -22 * flick(t, [3.1], 0.3)))
    p['lids'] = blink(t, [2.4])
    p['tail'] = tail_wave(p['tail'], t, tip=12 * bump(t, 0.5, 1.5))
    return p, {}, ALL4


def nod(t):
    p = sit_at(t)
    d = bump(t, 0.25, 1.6)
    p['head'] -= 22 * d
    p['neck'] -= 6 * d
    p['lids'] = max(smooth((t - 0.1) / 0.3) * (1 - smooth((t - 1.4) / 0.35)), 0)  # the slow blink of trust
    ears(p, (0.25 * d, 0), (0.25 * d, 0))
    return p, {}, ALL4


def shake_head(t):
    p = sit_at(t)
    env = bump(t, 0.1, 1.05) * (1 - 0.5 * ramp(t, 0.4, 1.0))
    r = 22 * env * math.sin(TAU * 6.5 * (t - 0.1))
    head_turn(p, 8 * env * math.sin(TAU * 6.5 * (t - 0.1) - 1.0), r, 0.25)
    lag = 0.7 * env * math.sin(TAU * 6.5 * (t - 0.18))      # ears flap a beat behind the head
    ears(p, (max(0, lag), 25 * flick(t, [1.15], 0.3)), (max(0, -lag), 0))
    p['lids'] = 0.6 * env
    return p, {}, ALL4


def glance(side):
    def fn(t):
        p = sit_at(t)
        e_ears = ramp(t, 0.05, 0.25) * (1 - ramp(t, 1.7, 2.0))   # the ears get there first
        e_head = ramp(t, 0.25, 0.65) * (1 - ramp(t, 1.6, 2.1))
        head_turn(p, side * 30 * e_head + side * 3 * bump(t, 0.65, 0.95), 0, 0.3)
        ears(p, (-0.2 * e_ears, side * 35 * e_ears), (-0.2 * e_ears, side * 35 * e_ears))
        p['tail'] = tail_wave(p['tail'], t, lat=-side * 14 * bump(t, 0.5, 1.4))   # the tail answers late
        p['lids'] = blink(t, [1.0])
        return p, {}, ALL4
    return fn


def blink_clip(t):
    p = sit_at(t)
    p['lids'] = blink(t, [0.15], close=0.08, hold=0.05, open_=0.16)
    ears(p, (0, 25 * flick(t, [0.2], 0.3)), (0, 0))
    p['head'] -= 3 * bump(t, 0.1, 0.5)
    return p, {}, ALL4


def look_up(t):
    p = sit_at(t)
    e = ramp(t, 0.15, 0.6) * (1 - ramp(t, 1.9, 2.4))
    p['chest'] += 6 * e
    p['neck'] += 18 * e
    p['head'] += 38 * e
    ears(p, (-0.5 * e, 0), (-0.5 * e, 0))
    chatter = bump(t, 0.8, 1.7)
    p['jaw'] = 0.18 * chatter * (0.5 + 0.5 * math.sin(TAU * 8 * t))     # the bird chatter
    p['tail'] = tail_wave(p['tail'], t, tip=25 * e, lat=8 * e, lat_f=2)
    return p, {}, ALL4


def yawn(t):
    p = sit_at(t)
    m = bump(t, 0.35, 2.1) ** 0.6
    p['head'] += 24 * m
    p['neck'] += 8 * m
    p['chest'] -= 4 * m
    p['jaw'] = 1.25 * m
    p['tongue'] = 0.55 * m + 0.7 * bump(t, 2.15, 2.55)          # then a lick
    p['tongue_curl'] = m
    p['lids'] = max(min(1.0, 1.4 * m), blink(t, [2.75]))
    ears(p, (0.8 * m, 0), (0.8 * m, 0))
    return p, {}, ALL4


def paddle(t):
    p = sit_at(t)
    hands, planted = {}, {'H.L', 'H.R'}
    swats = {'R': [(0.35, 0.75, 0.10), (1.05, 1.45, 0.12)], 'L': [(0.8, 1.2, 0.08)]}
    for s in 'LR':
        toe = toe_of(SIT, s)
        lift, reach = 0.0, 0.0
        for t0, t1, h in swats[s]:
            u = bump(t, t0, t1)
            lift = max(lift, h * u)
            reach = max(reach, 0.08 * math.sin(math.pi * clamp((t - t0) / (t1 - t0))) ** 2)
        hands[s] = hand(toe + reach, NEUTRAL['F.' + s][1] * 0.86, -70 * min(1, lift / 0.05), lift)
        if lift < 1e-4:
            planted.add('F.' + s)
    p['hands'] = hands
    track = bump(t, 0.2, 1.7)
    p['head'] -= 14 * track
    head_turn(p, -8 * track * math.sin(TAU * 1.2 * t), 0, 0.3)
    ears(p, (-0.3 * track, 0), (-0.3 * track, 0))
    p['tail'] = tail_wave(p['tail'], t, tip=18 * track, lat=10 * track, lat_f=1.5)
    return p, {}, planted


def wobble(t):
    p = sit_at(t)

    def wave(delay):
        u = t - 0.15 - delay
        return 0.0 if u < 0 else math.sin(TAU * 7 * u) * math.exp(-u / 0.35)
    head_turn(p, 0, 26 * wave(0.0), 0.2)                       # it starts at the head ...
    tn = dict(p['turn'])
    tn['chest'] = (0, 5 * wave(0.06))                           # ... runs down the chest ...
    tn['pelvis'] = (0, 4 * wave(0.12))                          # ... and the hips ...
    p['turn'] = tn
    p['tail'] = tail_wave(p['tail'], t - 0.2, lat=40 * max(0.0, 1 - (t - 0.2) / 1.2) if t > 0.2 else 0, lat_f=4)   # ... to the tail
    ears(p, (0.6 * abs(wave(0.03)), 0), (0.6 * abs(wave(0.03)), 0))
    p['fluff'] = 0.35 * bump(t, 0.1, 1.4)
    p['lids'] = 0.7 * bump(t, 0.1, 0.9)
    return p, {}, ALL4


def crouch_body(t, w, wig=0.0):
    sit, cr = {k: SIT[k] for k in SIT_BODY}, {k: crouch()[k] for k in SIT_BODY}
    p = lerp(sit, cr, w)
    p['turn'] = {'pelvis': (5 * wig, 4 * wig), 'spine': (2 * wig, 0)}
    return p


def crouch_feet(w, ext=0.0):
    return {s: foot(SIT['feet'][s][0][0], SIT['feet'][s][0][2], SIT_LEAN - 40 * w - 30 * ext, 0.005) for s in 'LR'}


def wiggle(t):
    w = ramp(t, 0.1, 0.6) * (1 - ramp(t, 1.9, 2.4))
    wig = bump(t, 0.55, 1.9) * math.sin(TAU * 3.4 * t)
    p = crouch_body(t, w, wig)
    p['tail'] = tail_wave(p['tail'], t, tip=20 * w, lat=12 * w, lat_f=3)
    ears(p, (-0.5 * w, 0), (-0.5 * w, 0))
    p['hands'], p['feet'] = SIT['hands'], crouch_feet(w)
    p['lids'] = 0.15 * w
    return p, {}, ALL4


def bounce(t):
    T0, T1, jump = 1.0, 1.35, 0.18
    w = ramp(t, 0.1, 0.5)
    wig = bump(t, 0.45, T0) * math.sin(TAU * 5 * t)
    p = crouch_body(t, w, wig)
    pj, air = ramp(t, T0, T1), bump(t, T0 + 0.03, T1)
    p = shifted(p, jump * pj)
    p['pelvis'] = (p['pelvis'][0], p['pelvis'][1] + 0.07 * air)
    p['tail'] = tail_wave(p['tail'], t, tip=20 * bump(t, 0.4, 1.0), up=10 * air, lat=8 * w, lat_f=2)
    ears(p, (-0.4 * w + 0.3 * bump(t, T1, T1 + 0.4), 0), (-0.4 * w + 0.3 * bump(t, T1, T1 + 0.4), 0))
    hands, feet, planted = {}, {}, set()
    for s in 'LR':
        st = Steps(toe_of(SIT, s))
        st.add(T0 + 0.05, T1, toe_of(SIT, s) + jump + 0.06, 0.1)        # front paws out, landing ahead
        pos, sw, lift = st.at(t)
        hands[s] = hand(pos, NEUTRAL['F.' + s][1] * 0.86, -55 * math.sin(math.pi * sw) if sw is not None else 0,
                        0.1 * math.sin(math.pi * sw) if sw is not None else 0)
        if sw is None:
            planted.add('F.' + s)
        fs = Steps(SIT['feet'][s][0][0])
        fs.add(T0 + 0.18, T1 + 0.08, SIT['feet'][s][0][0] + jump, 0.04)
        pos, sw, _ = fs.at(t)
        ext = bump(t, T0 - 0.08, T0 + 0.3)
        feet[s] = foot(pos, SIT['feet'][s][0][2], SIT_LEAN - 40 * w - 30 * ext, 0.005 + (0.04 * math.sin(math.pi * sw) if sw is not None else 0))
        if sw is None:
            planted.add('H.' + s)
    p.update(hands=hands, feet=feet)
    p['root'] = jump * pj
    return p, {}, planted


def peek(t):
    w = ramp(t, 0.1, 0.6) * (1 - ramp(t, 2.5, 3.0))
    p = crouch_body(t, 0.8 * w)
    p['neck'] += 30 * w
    p['head'] += 12 * w
    sweep = math.sin(TAU * 0.45 * (t - 0.6)) * bump(t, 0.6, 2.6)   # the eyes sweep slowly
    head_turn(p, 24 * sweep, 0, 0.25)
    ears(p, (-0.6 * w, 18 * sweep), (-0.6 * w, 18 * sweep))
    p['tail'] = tail_wave(p['tail'], t, tip=18 * w, lat=6 * w, lat_f=2.5)
    p['hands'], p['feet'] = SIT['hands'], crouch_feet(0.8 * w)
    return p, {}, ALL4


def stretch(t):
    """The downward dog: the front paws walk out, the chest sinks, the rump and tail go up."""
    out = 0.2
    w = ramp(t, 0.55, 1.2) * (1 - ramp(t, 2.3, 2.9))
    p = {k: SIT[k] for k in SIT_BODY}
    dog = dict(p)
    dog.update(pelvis=(SIT['pelvis'][0], 0.42), pitch=-28, spine=-12, spine2=-10, chest=-10, neck=22, head=72, scapula=-70,
               knee=(0.2, 0.32), elbow=(-0.05, 0.3))
    p = lerp(p, dog, w)
    p['tail'] = tail_wave(lerp(SIT['tail'], P.tail_sag([150, 130, 112, 100, 94, 92, 92, 96, 104, 116, 128, 140]), w), t, tip=10 * w)
    p['lids'] = 0.7 * bump(t, 0.9, 2.5)
    p['jaw'] = 0.35 * bump(t, 1.2, 2.0)
    ears(p, (0.35 * w, 0), (0.35 * w, 0))
    hands, planted = {}, {'H.L', 'H.R'}
    for i, s in enumerate('RL'):
        st = Steps(toe_of(SIT, s))
        st.add(0.62 + 0.2 * i, 0.92 + 0.2 * i, toe_of(SIT, s) + out, 0.05)   # each paw walks out as the chest sinks
        st.add(2.35 + 0.2 * i, 2.65 + 0.2 * i, toe_of(SIT, s), 0.05)
        pos, sw, _ = st.at(t)
        hands[s] = hand(pos, NEUTRAL['F.' + s][1] * 0.86, -50 * math.sin(math.pi * sw) if sw is not None else 0,
                        0.05 * math.sin(math.pi * sw) if sw is not None else 0)
        if sw is None:
            planted.add('F.' + s)
    feet = {s: foot(SIT['feet'][s][0][0], SIT['feet'][s][0][2], SIT_LEAN - 70 * w, 0.005) for s in 'LR'}
    p.update(hands=hands, feet=feet)
    return p, {}, planted


def listening(t):
    p = sit_at(t, sway=0.3)
    ears(p, (-0.7, -12), (-0.7, 12))
    head_turn(p, 0, 7 * math.sin(TAU * 0.25 * t), 0.3)
    p['tail'] = tail_wave(p['tail'], t, tip=14 * (flick(t, [1.0], 0.4) + flick(t, [2.6], 0.4)))
    p['lids'] = blink(t, [3.3])
    return p, {}, ALL4


def thinking(t):
    p = sit_at(t, sway=0.0)
    p['tail'] = tail_wave(SIT['tail'], t, lat=14, lat_f=0.25, lag=0.4)
    ears(p, (0, 10 * math.sin(TAU * 0.25 * t)), (0.35, 0))
    head_turn(p, 12 * bump(t, 1.8, 3.2), 6 * math.sin(TAU * 0.25 * t), 0.3)
    p['lids'] = 0.35 + 0.6 * blink(t, [0.9])
    return p, {}, ALL4


def speaking(t):
    p = sit_at(t)
    m = sum(bump(t, a, b) for a, b in ((0.25, 0.8), (1.5, 2.05), (2.8, 3.3)))
    p['head'] += 9 * m
    p['neck'] += 3 * m
    p['jaw'] = 0.6 * m
    ears(p, (0.2 * m, 0), (0.2 * m, 0))
    p['lids'] = 0.25 * m
    return p, {}, ALL4


def happy(t):
    p = sit_at(t, sway=0.0)
    up = P.tail_sag([150, 128, 108, 96, 92, 90, 90, 90, 90, 90, 88, 84])
    p['tail'] = tail_wave(up, t, lat=3 + 5 * (0.5 + 0.5 * math.sin(TAU * 0.5 * t)), lat_f=6, lag=0.9)   # the quivering tip
    p['lids'] = max(0.45, blink(t, [1.0, 3.0], close=0.25, hold=0.3, open_=0.35))                       # slow blinks
    ears(p, (-0.2, 0), (-0.2, 0))
    hands, planted = {}, {'H.L', 'H.R'}
    for s, phase in (('L', 0.0), ('R', 0.5)):                                                          # kneading
        u = (t + phase) % 1.0
        lift = 0.03 * bump(u, 0.0, 0.45)
        hands[s] = hand(toe_of(SIT, s), NEUTRAL['F.' + s][1] * 0.86, -30 * lift / 0.03, lift)
        if lift < 1e-4:
            planted.add('F.' + s)
    p['hands'] = hands
    head_turn(p, 0, 5 * math.sin(TAU * 0.25 * t), 0.3)
    return p, {}, planted


LOAF_X = 0.06                                  # the loaf lies this far ahead of where it sat (hind paws stay put)


def loaf():
    """The bread loaf: belly on the floor, back a low smooth dome, all four paws tucked out of sight
    (the hind feet folded flat under the haunches, the front paws curled back under the chest), the
    head low on its shoulders, the tail wrapped along the near side."""
    p = dict(SIT)
    p.update(pelvis=(SIT['pelvis'][0] + LOAF_X, 0.215), pitch=10, spine=3, spine2=-3, chest=-9, neck=30, head=86,
             scapula=-95, knee=(0.3, 0.3), elbow=(-0.2, 0.05))
    # tail: out of the rump, down to the floor, then forward along the near (right) flank
    p['tail'] = LOAF_TAIL
    shoulder_f = p['pelvis'][0] + 0.60
    p['hands'] = {s: (shoulder_f - 0.03 + (0.012 if s == 'R' else 0.0), 0.045, d * 0.055, -165) for s, d in (('R', -1), ('L', 1))}
    p['feet'] = {s: foot(SIT['feet'][s][0][0], SIT['feet'][s][0][2], 86, 0.0) for s in 'LR'}
    return p


LOAF_TAIL = [P.V3(*v) for v in ((0, -0.5, -1), (-0.3, -0.45, -1), (-0.65, -0.1, -1), (-1, 0.35, -0.55), (-0.6, 1, -0.15),
                                 (-0.2, 1, 0), (-0.05, 1, 0), (0.03, 1, 0), (0.08, 1, 0), (0.15, 1, 0), (0.3, 0.95, 0.05), (0.5, 0.8, 0.08))]


def sphinx():
    """Halfway down: the loaf's body with the front paws stretched out flat in front (lying, not yet tucked)."""
    p = loaf()
    p['scapula'] = -80
    p['elbow'] = (0.0, 0.2)
    p['hands'] = {s: hand(p['pelvis'][0] + 0.60 + 0.11 + (0.015 if s == 'R' else 0.0), d * 0.06) for s, d in (('R', -1), ('L', 1))}
    p['head'] += 4
    return p


LOAF_BODY = SIT_BODY + ('hands', 'feet')


def lie_down(t):
    """Sit -> loaf: the front paws walk out, the chest sinks into a sphinx, the rump settles, then each
    front paw curls back under the chest and the tail wraps along the side."""
    sit, sph, lf = SIT, sphinx(), loaf()
    w1 = ramp(t, 0.25, 1.3)        # down into the sphinx
    w2 = ramp(t, 1.55, 2.4)        # tuck
    body = lerp(lerp({k: sit[k] for k in SIT_BODY}, {k: sph[k] for k in SIT_BODY}, w1), {k: lf[k] for k in SIT_BODY}, w2)
    body['head'] -= 5 * bump(t, 0.05, 0.5)                      # anticipation: a look down first
    body['tail'] = tail_wave(body['tail'], t, lat=4, lat_f=0.3, tip=8 * bump(t, 2.2, 3.0))
    hands, planted = {}, {'H.L', 'H.R'}
    for i, (s, d) in enumerate((('R', -1), ('L', 1))):
        out = Steps(toe_of(SIT, s))
        out.add(0.3 + 0.18 * i, 0.62 + 0.18 * i, toe_of(SIT, s) + 0.06, 0.04)
        out.add(0.75 + 0.18 * i, 1.1 + 0.18 * i, sph['hands'][s][0] - HAND_OFF.x, 0.04)
        pos, sw, _ = out.at(t)
        hs = hand(pos, d * 0.06, -40 * math.sin(math.pi * sw) if sw is not None else 0,
                  0.04 * math.sin(math.pi * sw) if sw is not None else 0)
        tk = ramp(t, 1.6 + 0.25 * i, 2.15 + 0.25 * i)              # one paw, then the other, curls under
        if tk > 0:
            hs = lerp(hs, lf['hands'][s], tk)
            hs = (hs[0], hs[1] + 0.05 * math.sin(math.pi * tk) ** 0.5, hs[2], hs[3])   # off the floor at once, never dragged
        hands[s] = hs
        if sw is None and tk in (0.0, 1.0):
            planted.add('F.' + s)
    body['hands'] = hands
    body['feet'] = {s: foot(SIT['feet'][s][0][0], SIT['feet'][s][0][2], SIT_LEAN + (86 - SIT_LEAN) * w1, 0.005 * (1 - w1)) for s in 'LR'}
    body['lids'] = 0.3 * w2
    body['ears'] = {s: (0.1 * w2, 0) for s in 'LR'}
    return body, {}, planted


LIE = 3.0


def get_up(t):
    """Loaf -> sit: the lying down played back, with a beat of stillness and a stretch of the neck first."""
    body, props, planted = lie_down(LIE - t * (LIE / (LIE - 0.4)) if t > 0.4 else LIE)
    body = dict(body)
    body['head'] += 6 * bump(t, 0.0, 0.6)
    body['lids'] = 0.3 * (1 - ramp(t, 0.2, 0.6))
    return body, props, planted


def loaf_at(t, breath=1.0, period=4.0):
    p = loaf()
    b = math.sin(TAU * t / period)
    p['spine'] += 1.0 * breath * b
    p['chest'] += 1.6 * breath * b
    p['head'] -= 0.8 * breath * b
    return p


def sleepy(t):
    """Asleep in the loaf: slow breathing, eyes closed, the head drooping a little, an ear twitch now and then."""
    p = loaf_at(t, 1.2, 4.0)
    p['head'] -= 8
    p['neck'] -= 6
    p['tail'] = tail_wave(p['tail'], t, tip=6 * bump(t, 2.0, 3.0))
    p['lids'] = 1.0
    ears(p, (0.2, 24 * flick(t, [1.4], 0.35)), (0.2, -18 * flick(t, [3.2], 0.3)))
    return p, {}, ALL4


def loaf_idle(t):
    """Resting awake in the loaf: breathing, slow blinks, the ears swivelling, the tail tip curling."""
    p = loaf_at(t, 1.0, 3.0)
    p['lids'] = max(0.3, blink(t, [1.2, 3.4], close=0.2, hold=0.2, open_=0.3))
    ears(p, (0, 22 * flick(t, [0.8], 0.35)), (0, -22 * flick(t, [2.6], 0.35)))
    head_turn(p, 10 * bump(t, 1.6, 3.0), 0, 0.3)
    p['tail'] = tail_wave(p['tail'], t, tip=10 * bump(t, 0.3, 1.5) + 8 * bump(t, 2.8, 3.8))
    return p, {}, ALL4


def surprised(t):
    p = {k: STAND[k] for k in SIT_BODY}
    p.update(pelvis=(STAND['pelvis'][0], 0.45), pitch=18, spine=12, spine2=0, chest=-30, neck=46, head=86, scapula=-80)
    p['tail'] = P.tail_sag([118, 98, 90, 88, 90, 96, 106, 120, 136, 152, 166, 176])  # up, then hooked over
    p['fluff'] = 0.7
    trem = 0.6 * math.sin(TAU * 6 * t) + 0.4 * math.sin(TAU * 9.5 * t)
    p['pelvis'] = (p['pelvis'][0] + 0.003 * trem, p['pelvis'][1])
    ears(p, (1.0, 0), (1.0, 0))
    p['jaw'] = 0.3 + 0.1 * math.sin(TAU * 2 * t)        # a hiss
    p['lids'] = 0.0
    p['hands'] = {s: hand(NEUTRAL['F.' + s][0], NEUTRAL['F.' + s][1]) for s in 'LR'}
    p['feet'] = {s: foot(NEUTRAL['H.' + s][0], NEUTRAL['H.' + s][1], 5) for s in 'LR'}
    return p, {}, ALL4


# ------------------------------------------------------------------ startles and the shake
def surprised_body(t):
    """The surprised loop's pose (arched back, bottle-brush tail, ears flat), body keys only."""
    p, _, _ = surprised(t)
    return p


def startle(t, start, start_hands, start_feet, back=0.06, t0=0.9):
    """Calm, then the startle: a dip (anticipation), eyes widen, ears flick flat, the fur puffs, a small
    jump back onto stiff legs, and it settles into the surprised loop. All four paws leave the floor
    for the jump; otherwise they stay planted."""
    T_A, T_J0, T_J1 = t0, t0 + 0.16, t0 + 0.42
    end = surprised_body(t - T_J1)
    shift = start['feet']['R'][0][0] - end['feet']['R'][0][0] - back
    endp = shifted({k: end[k] for k in SIT_BODY}, shift)
    w = ramp(t, T_J0, T_J1 + 0.1)
    body = lerp({k: start[k] for k in SIT_BODY}, endp, w)
    body['root'] = shift * w
    dip = bump(t, T_A, T_J0 + 0.05)
    body['head'] -= 6 * dip
    body['pelvis'] = (body['pelvis'][0], body['pelvis'][1] - 0.02 * dip + 0.05 * bump(t, T_J0, T_J1))
    calm = 1 - ramp(t, T_A, T_J0)
    body['tail'] = tail_wave(lerp(start['tail'], end['tail'], w), t, lat=5 * calm, lat_f=0.25,
                             up=14 * bump(t, T_J0, T_J1 + 0.4))
    body['fluff'] = 0.7 * ramp(t, T_J0, T_J0 + 0.25)
    body['ears'] = {s: (min(1.0, 1.2 * ramp(t, T_A + 0.05, T_J0 + 0.08)), 20 * flick(t, [T_A], 0.2)) for s in 'LR'}
    body['lids'] = blink(t, [0.35]) if t < T_A else -0.35 * bump(t, T_A, T_J1 + 1.2) ** 0.5
    body['jaw'] = 0.3 * ramp(t, T_J1, T_J1 + 0.3) + 0.1 * math.sin(TAU * 2 * t) * ramp(t, T_J1, T_J1 + 0.3)
    body['turn'] = {}
    if t < T_A:  # calm: breathing
        b = math.sin(TAU * 0.5 * t)
        body['chest'] += 1.2 * b
    hands, feet, planted = {}, {}, set()
    for s in 'LR':
        hs = Steps(start_hands[s])
        hs.add(T_J0, T_J1, end['hands'][s][0] - HAND_OFF.x + shift, 0.05)
        pos, sw, _ = hs.at(t)
        hands[s] = hand(pos, NEUTRAL['F.' + s][1], -40 * math.sin(math.pi * sw) if sw is not None else 0,
                        0.05 * math.sin(math.pi * sw) if sw is not None else 0)
        if sw is None:
            planted.add('F.' + s)
        fs = Steps(start_feet[s][0][0])
        fs.add(T_J0 + 0.02, T_J1, end['feet'][s][0][0] + shift, 0.04)
        pos, sw, _ = fs.at(t)
        lean = lerp(math.degrees(math.atan2(start_feet[s][0][0] - start_feet[s][1][0], start_feet[s][1][1] - start_feet[s][0][1])), 5.0, w)
        feet[s] = foot(pos, NEUTRAL['H.' + s][1], lean, 0.04 * math.sin(math.pi * sw) if sw is not None else 0)
        if sw is None:
            planted.add('H.' + s)
    body.update(hands=hands, feet=feet)
    return body, {}, planted


def sit_surprised(t):
    return startle(t, SIT, {s: toe_of(SIT, s) for s in 'LR'}, SIT['feet'])


def stand_surprised(t):
    return startle(t, STAND, {s: NEUTRAL['F.' + s][0] for s in 'LR'}, STAND['feet'])


def shaken(t):
    """The human shakes Molty (it means: I want to talk). The world jolts and the cat is jostled
    (it sways against the motion, braces, one quick catch-step, ears flat, eyes squeezed, the tail
    whipping behind), a moment of dizziness, then it shakes it off head to tail like a wet cat and
    ends looking straight at you, ears up, ready to listen."""
    p = sit_at(t, sway=0.0)
    J0, J1, D1, S0, S1 = 0.15, 0.95, 1.55, 1.6, 2.35
    u = t - J0
    jolt = 0.0 if u < 0 else math.sin(TAU * 4.5 * u) * math.exp(-u / 0.35)          # the world's shake, decaying
    lag = 0.0 if u < 0.06 else math.sin(TAU * 4.5 * (u - 0.06)) * math.exp(-(u - 0.06) / 0.35)
    turn = {'pelvis': (0, -4 * jolt), 'spine': (0, -5 * jolt), 'chest': (0, -5 * lag), 'neck': (4 * lag, -10 * lag),
            'head': (6 * lag, -10 * lag)}
    # dizzy: a slow circle of the head, half-shut eyes, ears askew
    dz = bump(t, J1 - 0.1, D1)
    turn['head'] = (turn['head'][0] + 9 * dz * math.sin(TAU * 1.4 * t), turn['head'][1] + 8 * dz * math.cos(TAU * 1.4 * t))
    # the shake-off: head first, then chest, hips, tail (like wobble)
    def wave(delay):
        v = t - S0 - delay
        return 0.0 if v < 0 or t > S1 + 0.3 else math.sin(TAU * 7 * v) * math.exp(-v / 0.3)
    turn['head'] = (turn['head'][0], turn['head'][1] + 24 * wave(0.0))
    turn['chest'] = (turn['chest'][0], turn['chest'][1] + 5 * wave(0.06))
    turn['pelvis'] = (0, turn['pelvis'][1] + 6 * wave(0.12))
    p['turn'] = turn
    p['fluff'] = 0.3 * bump(t, S0, S1 + 0.2)
    jostled = bump(t, J0, J1 + 0.1)
    p['tail'] = tail_wave(SIT['tail'], t - 0.1, lat=35 * jostled + 30 * bump(t, S0 + 0.15, S1 + 0.3), lat_f=4.5, lag=0.7)
    attentive = ramp(t, S1, S1 + 0.35)
    p['ears'] = {'L': (0.9 * jostled - 0.6 * attentive, 15 * dz * math.sin(TAU * 1.4 * t)),
                 'R': (0.9 * jostled - 0.6 * attentive + 0.4 * dz, 0)}
    p['lids'] = max(0.85 * jostled, 0.5 * dz, 0.7 * bump(t, S0, S1)) * (1 - attentive) + (-0.12 * attentive)
    p['head'] += 5 * attentive - 4 * jostled
    # a quick catch-step with the near front paw as the jolt peaks (deliberate, lifted)
    hands, planted = dict(SIT['hands']), {'F.L', 'H.L', 'H.R'}
    st = Steps(toe_of(SIT, 'R'))
    st.add(J0 + 0.2, J0 + 0.42, toe_of(SIT, 'R') + 0.025, 0.04)
    pos, sw, _ = st.at(t)
    hands['R'] = hand(pos, NEUTRAL['F.R'][1] * 0.86 - (0.015 if sw is None and t > J0 + 0.42 else 0.0),
                      -45 * math.sin(math.pi * sw) if sw is not None else 0, 0.04 * math.sin(math.pi * sw) if sw is not None else 0)
    if sw is None:
        planted.add('F.R')
    p['hands'] = hands
    props = {'_cam': (0.025 * jolt, 0.012 * math.sin(TAU * 6 * max(0, u)) * math.exp(-max(0, u) / 0.3))}  # the world jolts
    return p, props, planted


# ------------------------------------------------------------------ the clip table
CLIPS = {
    # name: (fn, seconds, loop, view or (views...), camera: True follows the root, False frames the first pose, a number centres there)
    'walk': (walk_loop, 2 * T_CYCLE, True, 'side', True),
    'walk_start': (walk_start, 2.9, False, 'side', True),
    'walk_stop': (walk_stop, 3.4, False, 'side', True),
    'eat': (eat, 6.0, False, 'three_quarter', False),
    'yarn': (yarn, 5.2, False, 'side', YARN_STOP - 0.2),
    'idle': (idle, 4.0, True, 'front', False),
    'wiggle': (wiggle, 2.5, False, 'three_quarter', False),
    'nod': (nod, 2.0, False, 'front', False),
    'shake_head': (shake_head, 1.6, False, 'front', False),
    'bounce': (bounce, 2.2, False, 'three_quarter', False),
    'peek': (peek, 3.1, False, 'three_quarter', False),
    'glance_left': (glance(1), 2.3, False, 'front', False),
    'glance_right': (glance(-1), 2.3, False, 'front', False),
    'blink': (blink_clip, 0.8, False, 'front', False),
    'wobble': (wobble, 1.8, False, 'front', False),
    'stretch': (stretch, 3.4, False, 'side', False),
    'look_up': (look_up, 2.5, False, 'three_quarter', False),
    'yawn': (yawn, 3.0, False, 'three_quarter', False),
    'paddle': (paddle, 2.0, False, 'three_quarter', False),
    'listening': (listening, 4.0, True, 'front', False),
    'thinking': (thinking, 4.0, True, 'front', False),
    'speaking': (speaking, 4.0, True, 'front', False),
    'happy': (happy, 4.0, True, 'front', False),
    'sleepy': (sleepy, 4.0, True, ('front', 'side'), False),
    'loaf_idle': (loaf_idle, 4.0, True, ('front', 'side'), False),
    'lie_down': (lie_down, LIE, False, ('front', 'side'), False),
    'get_up': (get_up, LIE + 0.4, False, ('front', 'side'), False),
    'surprised': (surprised, 2.0, True, ('side', 'front', 'three_quarter'), False),
    'sit_surprised': (sit_surprised, 3.0, False, ('front', 'side'), False),
    'stand_surprised': (stand_surprised, 3.0, False, ('front', 'side'), False),
    'shaken': (shaken, 3.4, False, ('front', 'side'), False),
}


# ================================================================== pose families and transitions
# Every clip starts and ends in a pose family: sit, stand, loaf, crouch (the play crouch) or walk.
# Between families there is an authored transition for every ordered pair (FAMILY_EDGES); inside a
# family the device crossfades. Transitions come from one generator with per-pair choreography:
# anticipation, the rear and the front moving with overlap (which end leads), the paws stepping to
# their new spots while the others stay planted, the tail and head following through.

def lean_of(fp):
    """The metatarsus lean (degrees back from vertical) of a hind foot tuple."""
    (bf, bz, _), (kf, kz), _ = fp
    return math.degrees(math.atan2(bf - kf, kz - bz))


def shift_pose(p, dx):
    """The whole pose (body, poles and paw targets) moved forward by dx."""
    q = shifted(p, dx)
    if 'hands' in p:
        q['hands'] = {s: (h[0] + dx, *h[1:]) for s, h in p['hands'].items()}
    if 'feet' in p:
        q['feet'] = {s: ((f[0][0] + dx, f[0][1], f[0][2]), (f[1][0] + dx, f[1][1]), f[2]) for s, f in p['feet'].items()}
    return q


def _crouch_pose():
    p = crouch()
    p['feet'] = crouch_feet(1.0)
    p['tail'] = SIT['tail']
    return p


FAMILY = {'sit': SIT, 'stand': STAND, 'loaf': loaf(), 'crouch': _crouch_pose()}
LOOK = ('lids', 'jaw', 'tongue', 'fluff')


def tucked(h):
    return h[3] < -120


def transition(a, b, dur, lead='rear', lag=0.35, hind_first=True, steps_front='RL', antic=1.0, body_t=(0.2, 0.85)):
    """A clip from family pose a to family pose b (dicts at the same anchor: the hind paws stay
    planted where they are). lead: which end moves first ('rear' or 'front'); lag: how far behind
    (fraction of the move) the other end follows; body_t: the move's span as fractions of dur."""
    m0, m1 = body_t[0] * dur, body_t[1] * dur
    span = m1 - m0
    lead_w = lambda t: ramp(t, m0, m0 + span * (1 - lag))
    follow_w = lambda t: ramp(t, m0 + span * lag, m1)
    rear_keys, front_keys = ('pelvis', 'pitch', 'spine', 'knee'), ('spine2', 'chest', 'scapula', 'neck', 'head', 'elbow')

    def fn(t):
        wr, wf = (lead_w(t), follow_w(t)) if lead == 'rear' else (follow_w(t), lead_w(t))
        body = {}
        for k in rear_keys:
            body[k] = lerp(a[k], b[k], wr)
        for k in front_keys:
            body[k] = lerp(a[k], b[k], wf)
        wt = ramp(t, m0 + 0.15, m1 + 0.25)                          # the tail follows through, later
        body['tail'] = tail_wave(lerp(a['tail'], b['tail'], wt), t, lat=6 * bump(t, m0, m1 + 0.6), lat_f=0.9,
                                 up=5 * bump(t, m1 - 0.1, m1 + 0.6))
        # anticipation: a small dip of the head and chest the other way, just before the move
        dip = antic * bump(t, max(0.0, m0 - 0.3), m0 + 0.12)
        body['head'] -= 7 * dip
        body['chest'] -= 3 * dip
        # follow-through: the head settles after the body arrives (a small damped overshoot)
        u = t - m1
        if u > 0:
            body['head'] += 4 * math.sin(TAU * 1.4 * u) * math.exp(-u / 0.25)
        body['root'] = lerp(a.get('root', 0.0), b.get('root', 0.0), wr)
        for k in LOOK:
            body[k] = lerp(a.get(k, 0.0), b.get(k, 0.0), ramp(t, m0, m1))
        body['lids'] = max(body['lids'], blink(t, [m1 + 0.05]))
        body['ears'] = {s: (0.25 * bump(t, m0, m1), 10 * flick(t, [m1 + 0.1 if s == 'L' else m1 + 0.2], 0.3)) for s in 'LR'}
        hands, feet, planted = {}, {}, set()
        # front paws: step from a's spots to b's one after the other; a tucked paw curls in or out
        for i, s in enumerate(steps_front):
            ha, hb = a['hands'][s], b['hands'][s]
            t0 = (m0 + span * (0.15 if lead == 'front' else 0.45)) + i * span * 0.22
            t1 = t0 + max(0.22, span * 0.3)
            if tucked(ha) or tucked(hb):
                k = ramp(t, t0, t1)
                h = lerp(ha, hb, k)
                hands[s] = (h[0], h[1] + 0.05 * math.sin(math.pi * k) ** 0.5, h[2], h[3])   # off the floor at once, never dragged
                if k in (0.0, 1.0):
                    planted.add('F.' + s)
            else:
                # the front paws keep up with the shoulders: as many short steps as the distance needs
                # (at most 0.1 each), each landing where the shoulders will be, the two paws alternating
                ta, tb = ha[0] - HAND_OFF.x, hb[0] - HAND_OFF.x
                st = Steps(ta)
                ns = max(1, math.ceil(abs(tb - ta) / 0.1)) if abs(tb - ta) > 0.004 else 0
                wfront = follow_w if lead == 'rear' else lead_w
                for j in range(ns):
                    s0 = m0 + span * (j + 0.25 + 0.5 * i) / (ns + 0.5)
                    s1 = s0 + 0.24
                    tgt = tb if j == ns - 1 else ta + (tb - ta) * min(1.0, wfront(s1 + 0.1))
                    st.add(s0, s1, tgt, 0.05)
                if ns:
                    t0 = st.steps[0][0]
                pos, sw, _ = st.at(t)
                if sw is None:
                    hands[s] = hand(pos, (ha if t < t0 else hb)[2], ha[3] if t < t0 else hb[3])
                    planted.add('F.' + s)
                else:
                    hands[s] = hand(pos, lerp(ha[2], hb[2], sw), -45 * math.sin(math.pi * sw), 0.05 * math.sin(math.pi * sw))
        # hind paws: the balls stay planted (a short step only if the spot changes); the heels lift or settle
        for i, s in enumerate('RL'):
            fa, fb = a['feet'][s], b['feet'][s]
            st = Steps(fa[0][0])
            t0 = m0 + span * (0.05 if hind_first else 0.5) + i * 0.18
            if abs(fb[0][0] - fa[0][0]) > 0.006:
                st.add(t0, t0 + 0.28, fb[0][0], 0.04)
            pos, sw, _ = st.at(t)
            lean = lerp(lean_of(fa), lean_of(fb), wr)
            lift = 0.04 * math.sin(math.pi * sw) if sw is not None else 0.0
            feet[s] = foot(pos, fa[0][2] if t < t0 else fb[0][2], lean, max(0.0, lerp(fa[0][1], fb[0][1], wr) - 0.03) + lift)
            if sw is None:
                planted.add('H.' + s)
        body.update(hands=hands, feet=feet)
        return body, {}, planted
    return fn


def anchored(src, dst):
    """dst's family pose moved so its near hind paw lands where src's is (the hind paws stay put)."""
    a, b = FAMILY[src], FAMILY[dst]
    return shift_pose(b, a['feet']['R'][0][0] - b['feet']['R'][0][0])


# the choreography of each pair: duration, which end leads, and the order things happen
TRANS = {
    # stand up from sitting: the front stays, the rear rises (hind legs extend), the front paws step back under the shoulders
    ('sit', 'stand'): dict(dur=1.6, lead='rear', lag=0.3, steps_front='RL'),
    # sit down: the rear folds first, the chest stays up, the front paws walk back a step
    ('stand', 'sit'): dict(dur=1.9, lead='rear', lag=0.35, steps_front='LR'),
    # the loaf to sitting: the paws come out from under the chest, the chest rises, then the rump tucks under
    ('loaf', 'sit'): dict(dur=2.0, lead='front', lag=0.35, steps_front='RL', body_t=(0.25, 0.85)),
    # standing to the loaf: the rear sinks first, then the chest; the paws curl under last
    ('stand', 'loaf'): dict(dur=2.6, lead='rear', lag=0.4, steps_front='RL', body_t=(0.15, 0.8)),
    # the loaf to standing: the paws come out, the chest lifts, the rear pushes up
    ('loaf', 'stand'): dict(dur=2.0, lead='front', lag=0.3, steps_front='LR', body_t=(0.2, 0.85)),
    # into the play crouch from sitting: the chest drops forward, the rear lifts a little, eyes on the prey
    ('sit', 'crouch'): dict(dur=1.0, lead='front', lag=0.25, steps_front='RL'),
    ('crouch', 'sit'): dict(dur=1.0, lead='rear', lag=0.25, steps_front='LR'),
    ('stand', 'crouch'): dict(dur=1.5, lead='front', lag=0.3, steps_front='RL'),
    ('crouch', 'stand'): dict(dur=1.4, lead='rear', lag=0.25, steps_front='LR'),
    ('loaf', 'crouch'): dict(dur=1.4, lead='front', lag=0.3, steps_front='RL', body_t=(0.2, 0.85)),
    ('crouch', 'loaf'): dict(dur=1.6, lead='rear', lag=0.35, steps_front='LR', body_t=(0.15, 0.8)),
}


def make_transition(src, dst):
    spec = dict(TRANS[(src, dst)])
    dur = spec.pop('dur')
    return transition(FAMILY[src], anchored(src, dst), dur, **spec), dur


def get_up_authored():
    fn, dur = make_transition('loaf', 'sit')
    return fn, dur


# ------------------------------------------------------------------ in and out of the walk
def walk_from_stand(t, x0=0.0):
    """From standing still: the weight shifts forward, the near front paw leads off and the body
    speeds up into the walk (the footstep planner keeps every planted paw where it landed)."""
    T_GO, acc = 0.25, 0.7

    def x(tt):
        if tt <= T_GO:
            return x0
        u = tt - T_GO
        return x0 + (V_WALK * (u * u / (2 * acc)) if u < acc else V_WALK * (u - acc / 2))
    stand = shift_pose(STAND, x0)
    g = ramp(t, T_GO, T_GO + 0.9)
    walking = gait_body(shift_pose(STAND, x(t)), t - T_GO, g)
    body = lerp({k: stand[k] for k in SIT_BODY}, {k: walking[k] for k in SIT_BODY}, g)
    body['turn'] = walking['turn']
    body['root'] = x(t)
    body['pelvis'] = (STAND['pelvis'][0] + x(t), body['pelvis'][1])
    lean = bump(t, 0.0, T_GO + 0.2)                                  # anticipation: the weight goes forward
    body['chest'] -= 2 * lean
    body['head'] -= 3 * lean
    steps = plan(x, T_GO, 10.0, phase0=0.27)
    hands, feet, planted = paws_from(steps, x, t)
    body.update(hands=hands, feet=feet)
    body['ears'] = {s: (-0.2 * g, 0) for s in 'LR'}
    return body, {}, planted


WALK_IN, WALK_OUT = 2.2, 2.2


def walk_to_stand(t):
    """Slowing to a stop: the steps shorten, the last paws land under the body, it stands."""
    DEC = 1.1
    x_end = V_WALK * DEC / 2

    def x(tt):
        return x_end if tt >= DEC else V_WALK * (tt - tt * tt / (2 * DEC))
    steps = plan(x, -20.0, 2.0)
    g = 1 - ramp(t, 0.3, 1.7)
    walking = gait_body(shift_pose(STAND, x(t)), t, g)
    stand = shift_pose(STAND, x(t))
    body = lerp({k: stand[k] for k in SIT_BODY}, {k: walking[k] for k in SIT_BODY}, g)
    body['turn'] = walking['turn']
    body['root'] = x(t)
    body['head'] += 3 * bump(t, 1.4, 2.0)                           # the head settles last
    hands, feet, planted = paws_from(steps, x, t)
    body.update(hands=hands, feet=feet)
    body['lids'] = blink(t, [1.9])
    return body, {}, planted


def walk_stop_end():
    return walk_to_stand(WALK_OUT)[0]['root']


def concat(f1, d1, f2):
    """f1 then f2; f2's poses are moved forward by where f1 ended (root motion carries)."""
    end = f1(d1)[0].get('root', 0.0)

    def fn(t):
        if t < d1:
            return f1(t)
        p, props, planted = f2(t - d1)
        return shift_pose(p, end), props, planted
    return fn


def family_to_walk(src):
    if src == 'stand':
        return walk_from_stand, WALK_IN
    tr, d = make_transition(src, 'stand')
    return concat(tr, d, walk_from_stand), d + WALK_IN


def walk_to_family(dst):
    if dst == 'stand':
        return walk_to_stand, WALK_OUT
    tr, d = make_transition('stand', dst)
    return concat(walk_to_stand, WALK_OUT, tr), WALK_OUT + d


def crouch_idle(t):
    """The play crouch held: eyes on the prey, the rump swaying a little, the tail tip twitching."""
    p = crouch_body(t, 1.0, 0.25 * math.sin(TAU * 0.6 * t))
    p['tail'] = tail_wave(p['tail'], t, tip=14 * (0.5 + 0.5 * math.sin(TAU * 1.3 * t)), lat=6, lat_f=0.7)
    ears(p, (-0.45, 6 * math.sin(TAU * 0.4 * t)), (-0.45, 0))
    p['hands'], p['feet'] = SIT['hands'], crouch_feet(1.0)
    p['lids'] = 0.15
    return p, {}, ALL4


def stand_idle(t):
    """Standing still, alive: breathing, an ear turning, the tail tip curling."""
    p = dict(STAND)
    b = math.sin(TAU * 0.5 * t)
    p['spine2'] = STAND.get('spine2', 0) + 0.8 * b
    p['chest'] = STAND['chest'] + 1.0 * b
    p['tail'] = tail_wave(STAND['tail'], t, lat=5, lat_f=0.25, up=3, up_f=0.5, tip=8 * bump(t, 0.8, 2.0))
    ears(p, (0, 20 * flick(t, [1.5], 0.3)), (0, 0))
    p['lids'] = blink(t, [2.6])
    return p, {}, ALL4


_get_up_fn, _get_up_dur = get_up_authored()
CLIPS['get_up'] = (_get_up_fn, _get_up_dur, False, ('front', 'side'), False)
CLIPS['crouch_idle'] = (crouch_idle, 3.0, True, ('side', 'front'), False)
CLIPS['stand_idle'] = (stand_idle, 4.0, True, ('side', 'front'), False)
for (_s, _d) in TRANS:
    _name = f'{_s}_to_{_d}'
    if _name in ('loaf_to_sit',):
        continue                                             # 'get_up'
    _fn, _dur = make_transition(_s, _d)
    CLIPS[_name] = (_fn, _dur, False, ('side', 'front'), False)
# into and out of the walk: from standing (stand_to_walk, walk_to_stand) and from sitting (walk_start, walk_stop) as
# their own clips; from the loaf and the crouch the route stands up first (two authored hops: loaf_to_stand, stand_to_walk)
for _f in ('stand',):
    _fn, _dur = family_to_walk(_f)
    CLIPS[f'{_f}_to_walk'] = (_fn, _dur, False, 'side', True)
    _fn, _dur = walk_to_family(_f)
    CLIPS[f'walk_to_{_f}'] = (_fn, _dur, False, 'side', True)

# each clip's start and end family; the authored edges between families
CLIP_FAMILY = {
    'walk': ('walk', 'walk'), 'walk_start': ('sit', 'walk'), 'walk_stop': ('walk', 'sit'),
    'eat': ('sit', 'sit'), 'yarn': ('sit', 'crouch'), 'bounce': ('sit', 'crouch'),
    'sleepy': ('loaf', 'loaf'), 'loaf_idle': ('loaf', 'loaf'), 'lie_down': ('sit', 'loaf'), 'get_up': ('loaf', 'sit'),
    'surprised': ('stand', 'stand'), 'sit_surprised': ('sit', 'stand'), 'stand_surprised': ('stand', 'stand'),
    'crouch_idle': ('crouch', 'crouch'), 'stand_idle': ('stand', 'stand'),
}
for _n in CLIPS:
    if _n in CLIP_FAMILY:
        continue
    if '_to_' in _n:
        _a, _b = _n.split('_to_')
        CLIP_FAMILY[_n] = (_a, _b)
    else:
        CLIP_FAMILY[_n] = ('sit', 'sit')
FAMILY_EDGES = {}  # (from, to) -> the authored transition that goes there (never a gesture that happens to end elsewhere)
for _n, (_a, _b) in CLIP_FAMILY.items():
    if _a != _b and _n == f'{_a}_to_{_b}':
        FAMILY_EDGES[(_a, _b)] = _n
FAMILY_EDGES[('sit', 'walk')] = 'walk_start'
FAMILY_EDGES[('walk', 'sit')] = 'walk_stop'
FAMILY_EDGES[('loaf', 'sit')] = 'get_up'
FAMILY_EDGES[('sit', 'loaf')] = 'lie_down'
# the families' resting loops (where a route waits)
FAMILY_REST = {'sit': 'idle', 'stand': 'stand_idle', 'loaf': 'loaf_idle', 'crouch': 'crouch_idle', 'walk': 'walk'}
