import json, sys
from statistics import mean
def summ(path, label):
    rows=[json.loads(l) for l in open(path)]
    def block(sel, tag):
        n=len(sel)
        if n==0:
            print(f"{label} [{tag}] no frames"); return
        def st(k): 
            v=[r[k] for r in sel]; return mean(v), max(v)
        dm,dx=st('drawn'); bm,bx=st('blocks'); gm,gx=st('blocks_ge2'); sm,sx=st('singletons')
        tot_drawn=sum(r['drawn'] for r in sel); tot_multi=sum(r['members_in_multi'] for r in sel)
        h={k:sum(r['hist'][k] for r in sel) for k in ['1','2-4','5-16','17+']}
        print(f"{label} [{tag}] frames={n} ({sel[0]['frame']}..{sel[-1]['frame']})")
        print(f"  drawn mean={dm:.1f} max={dx} | blocks mean={bm:.1f} max={bx} | blocks>=2 mean={gm:.1f} max={gx} | singletons mean={sm:.1f} max={sx}")
        print(f"  hist(all frames summed, blocks) 1={h['1']} 2-4={h['2-4']} 5-16={h['5-16']} 17+={h['17+']}")
        print(f"  share of drawn in multi-member blocks = {tot_multi}/{tot_drawn} = {100*tot_multi/max(tot_drawn,1):.1f}%")
        print(f"  frames with implicit (no-origin) drawn blocks = {sum(1 for r in sel if r['implicit_blocks_with_drawn'])}")
        print(f"  alt-origin (!multi_prev only) count mean={mean(r['alt_origin_count_ignoring_ctrl'] for r in sel):.1f}; bank switches total={sum(r['bank_switches'] for r in sel)}")
    block(rows, 'all frames')
    block([r for r in rows if r['drawn']>0], 'frames with drawn>0')
summ(sys.argv[1], sys.argv[2])
