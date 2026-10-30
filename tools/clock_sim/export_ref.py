# Writes reference traces for tests/test_scheduler.cpp: python3 export_ref.py ../../tests/data
# One file per case. Line 1: "# target_fps kmax". Then one line per native frame:
#   now_ns kind flush n tick_0 t_0 tick_1 t_1 ...      (kind B = bypass, H = hold, P = pair, C = carrier only)
import sys, os
from clock_sim import run, hitch
from frame_sources import gen
out = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)
cases = {c: gen(c) for c in ['steady30', 'steady40', 'steady58', 'steady60', 'jit40', 'jit30', 'ramp', 'wobble', 'hover', 'hitch']}
cases['hitch60'] = hitch(100, 60)
cases['hitch30'] = hitch(100, 30)
for name, frames in cases.items():
    frames = [round(x) for x in frames]
    trace = []
    run(frames, 60, trace=trace, consume_zero=True)
    with open(os.path.join(out, f'ref_{name}.txt'), 'w') as f:
        f.write('# 60 3\n')
        for now, kind, flush, rec in trace:
            f.write(f"{now} {kind} {int(bool(flush))} {len(rec)}" + ''.join(f" {tk:.4f} {t:.6f}" for tk, t in rec) + "\n")
print('wrote', len(cases), 'traces to', out)
