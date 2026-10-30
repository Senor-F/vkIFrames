# Models the Stage 3 output clock (DESIGN §2.B-2.D): EMA, bypass gate, jitter buffer, per-tick t. Run: python3 clock_sim.py
# Defaults here are the tuned values used in STAGE3.md; frame_sources.py has the synthetic game frame-time patterns.
import random, statistics as st
from frame_sources import gen, Gate, MS
class Timing:
    def __init__(s,a=0.15,ja=0.05): s.a=a; s.ja=ja; s.n=0; s.last=0; s.sm=16_666_667.0; s.raw=0; s.jit=0.0
    def update(s,now):
        s.n+=1
        if s.n==1: s.last=now; return s.sm
        s.raw=now-s.last; s.last=now
        c=min(max(s.raw,4_166_666),100_000_000)
        if s.n==2: s.sm=c
        else:
            s.jit=s.ja*abs(c-s.sm)+(1-s.ja)*s.jit; s.sm=s.a*c+(1-s.a)*s.sm
        return s.sm
def run(frames,tf,kmax=3,enter=1.12,exit_=1.03,dwell=8,jm=2.0,ja=0.05,slew=0.02,mcap=0.5,consume_zero=False,trace=None):
    # consume_zero: a pair with no tick still presents the held frame, at the next tick time, and consumes that tick (the C++ layer does this)
    # trace: list that receives one record per native frame, used to generate the reference data for tests/test_scheduler.cpp
    target=1e9/tf; tm=Timing(0.15,ja); gate=Gate(enter,exit_,dwell)
    held=False; running=False; v_prev=v_cur=0; anchor=0; nxt=0; L=0
    out=[]; pairs=[]; flips=0; last=False; late=0; Ls=[]
    for now in frames:
        sm=tm.update(now); stalled=tm.raw>100_000_000
        active=(not stalled) and gate.update(sm/target)
        if stalled: gate=Gate(enter,exit_,dwell)
        if active!=last: flips+=1; last=active
        if not active:
            if trace is not None: trace.append((now,'B',held,[]))
            held=False; running=False; continue
        if not running: v_cur=now
        else: v_prev=v_cur; v_cur+=sm; v_cur+=(now-v_cur)/8
        want=sm+min(jm*tm.jit,mcap*sm)
        if not held:
            held=True; v_prev=v_cur-sm; L=want; anchor=v_cur+L; nxt=0; running=True
            if trace is not None: trace.append((now,'H',False,[]))
            continue
        L+=max(-slew*target,min(slew*target,want-L))      # rate-limited content delay
        em=0; rec=[]
        while True:
            tick=anchor+nxt*target; tau=tick-L
            if tau>=v_cur: break
            nxt+=1
            if em>kmax: continue
            t=min(max((tau-v_prev)/(v_cur-v_prev),0),1)
            if tick<now: late+=1
            out.append((tick,tau,t)); em+=1; rec.append((tick,t))
        kind='P'
        if em==0 and consume_zero:
            rec.append((anchor+nxt*target,0.0)); nxt+=1; kind='C'
        if trace is not None: trace.append((now,kind,False,rec))
        pairs.append(em); Ls.append(L/MS)
    return out,pairs,flips,late,Ls
def rep(name,r,tf=60):
    out,pairs,flips,late,Ls=r
    if len(out)<5: print(f"{name:9s} bypass-only flips={flips}"); return
    target=1e9/tf; taus=[x[1] for x in out]
    dt=[(taus[i+1]-taus[i])/target for i in range(len(taus)-1)]
    print(f"{name:9s} ticks={len(out):4d} late={100*late/len(out):4.1f}% adv sd={st.pstdev(dt):.3f} [{min(dt):.2f},{max(dt):.2f}] n=[{min(pairs)},{max(pairs)}] L={st.mean(Ls):5.1f}ms flips={flips}")
def hitch(period,ms,fps=40,seed=3):
    r=random.Random(seed); t=0; ts=[]
    for i in range(900):
        t+=1e9/fps*(1+r.uniform(-.05,.05))+(ms*MS if i%period==period-1 else 0); ts.append(t)
    return ts
if __name__=='__main__':
    cases=['steady30','steady40','jit40','jit30','ramp','wobble']
    for label,kw in [('jm=2 slew=0.05',dict()),('jm=2 slew=0.02',dict(slew=0.02)),('jm=3 slew=0.05',dict(jm=3)),('jm=2 no slew',dict(slew=10))]:
        print('\n==',label)
        for c in cases: rep(c,run(gen(c),60,**kw))
        for ms in (30,60): rep(f'hitch{ms}',run(hitch(100,ms),60,**kw))
    print('\n== gate flips (fps: on?/flips), jitter j, 900 frames; wobble crossing boundary')
    for en,ex,dw in [(1.08,1.02,5),(1.15,1.05,8),(1.12,1.03,8),(1.2,1.1,8)]:
        line=[]
        for j in (0.03,0.10,0.20):
            tot=0; ons=[]
            for fps in (48,50,52,54,56,58):
                r=random.Random(int(fps*j*100)); t=0; ts=[]
                for i in range(900): t+=1e9/fps*(1+r.uniform(-j,j)); ts.append(t)
                o=run(ts,60,enter=en,exit_=ex,dwell=dw); tot+=o[2]-1 if len(o[0])>5 else o[2]; ons.append('Y' if len(o[0])>5 else 'n')
            line.append(f"j={j:.2f} {''.join(ons)} extra-flips={tot}")
        print(f"enter={en} exit={ex} dwell={dw}: "+' | '.join(line))
    # sinusoidal wobble across the boundary 
    import math
    print('wobble across boundary (fps 50+-6 over 300 frames period):')
    for en,ex,dw in [(1.08,1.02,5),(1.15,1.05,8),(1.2,1.1,8)]:
        t=0; ts=[]
        for i in range(1800): fps=50+6*math.sin(i/48); t+=1e9/fps; ts.append(t)
        o=run(ts,60,enter=en,exit_=ex,dwell=dw); print(f"  enter={en} exit={ex} dwell={dw}: flips={o[2]}")
