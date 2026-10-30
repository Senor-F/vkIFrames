import random, statistics as st, sys
MS=1e6
class Timing:
    def __init__(s,a=0.15): s.a=a; s.first=True; s.last=0; s.sm=16_666_667.0; s.raw=0
    def update(s,now):
        if s.first: s.last=now; s.first=False; return s.sm
        s.raw=now-s.last; s.last=now
        c=min(max(s.raw,4_166_666),100_000_000)
        s.sm=s.a*c+(1-s.a)*s.sm; return s.sm
class Gate:
    def __init__(s,enter,exit_,dwell): s.active=False; s.streak=0; s.e=enter; s.x=exit_; s.d=dwell
    def update(s,r):
        want = r>s.x if s.active else r>s.e
        s.streak = s.streak+1 if want!=s.active else 0
        if s.streak>=s.d: s.active=want; s.streak=0
        return s.active
def run(frames, target_fps, kmax=3, enter=1.08, exit_=1.02, dwell=5, fix_anchor=True, L_mode='smoothed', slew=8, alpha=0.15):
    target=1e9/target_fps
    tm=Timing(alpha); gate=Gate(enter,exit_,dwell)
    held=False; running=False; v_prev=v_cur=0; anchor=0; nxt=0
    out=[]  # (tick_time, tau, t, native?) 
    pairs=[]; flaps=0; last_active=False; late=0; zero=0; dropped=0
    for now in frames:
        sm=tm.update(now)
        stalled = tm.raw>100_000_000
        active = (not stalled) and gate.update(sm/target)
        if stalled: gate=Gate(enter,exit_,dwell)
        if active!=last_active: flaps+=1; last_active=active
        if not active:
            held=False; running=False; continue
        # OnNativeFrame
        if not running: v_cur=now
        else:
            v_prev=v_cur; v_cur+=sm; v_cur+=(now-v_cur)/slew
        if not held:
            held=True
            v_prev=v_cur-sm
            anchor = v_cur + (sm if fix_anchor else 0)
            nxt=0; running=True; continue
        L = sm if L_mode=='smoothed' else (v_cur-v_prev)
        emitted=0
        while True:
            tick=anchor+nxt*target
            tau=tick-L
            if tau>=v_cur: break
            nxt+=1
            if emitted>kmax: dropped+=1; continue
            t=min(max((tau-v_prev)/(v_cur-v_prev),0),1)
            if tick < now: late+=1       # tick already in the past when we emit it
            out.append((tick,tau,t)); emitted+=1
        if emitted==0: zero+=1
        pairs.append(emitted)
    return dict(out=out,pairs=pairs,flaps=flaps,late=late,zero=zero,dropped=dropped)

def gen(kind,n=900,seed=1):
    r=random.Random(seed); t=0; ts=[]
    for i in range(n):
        if kind.startswith('steady'): fps=float(kind[6:]); dt=1e9/fps
        elif kind.startswith('jit'):  fps=float(kind[3:]); dt=1e9/fps*(1+r.uniform(-.2,.2))
        elif kind=='ramp': fps=30+30*i/n; dt=1e9/fps*(1+r.uniform(-.05,.05))
        elif kind=='hover': fps=60/1.05+r.uniform(-1.5,1.5); dt=1e9/fps          # hovering near target
        elif kind=='hitch': dt=1e9/40*(1+r.uniform(-.05,.05)) + (80*MS if i%200==199 else 0)
        elif kind=='wobble': fps=40+8*__import__('math').sin(i/40); dt=1e9/fps*(1+r.uniform(-.05,.05))
        t+=dt; ts.append(t)
    return ts

def report(name,res,target_fps):
    o=res['out']; target=1e9/target_fps
    taus=[x[1] for x in o]
    if len(o)<5: print(f"{name:10s} no output  flaps={res['flaps']}"); return
    dt=[(taus[i+1]-taus[i])/target for i in range(len(taus)-1)]   # content advance per tick, ideal = native_rate/target ratio
    back=sum(1 for x in dt if x<=0)
    ts=[x[2] for x in o]
    # smoothness: content advance should be ~constant; report spread (std/mean) and worst deviation
    m=st.mean(dt); sd=st.pstdev(dt)
    p=res['pairs']
    print(f"{name:10s} ticks={len(o):4d} adv/tick mean={m:.3f} sd={sd:.3f} min={min(dt):.2f} max={max(dt):.2f} backwards={back} "
          f"pairs/ n=[{min(p)},{max(p)}] zero={res['zero']} dropped={res['dropped']} late={res['late']} flaps={res['flaps']}")

