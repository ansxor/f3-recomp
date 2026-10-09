"""Self-contained HTML call graph: routines as nodes, every edge kind from the static model,
uncertain control flow routed through the ?dispatch node. No network or external libraries."""
from __future__ import annotations

import json
from pathlib import Path

from .cli import annotate, insn_text, kinds_text, memory_summary
from .game import DISPATCH, VIDEO_REGIONS, Game, h


def routine_listing(g: Game, entry: int) -> str:
    states = g.states(entry)
    lines = []
    previous_end = None
    for pc in g.routines[entry].pcs:
        insn = g.code[pc]
        if previous_end is not None and pc != previous_end:
            lines.append("          …")
        notes = annotate(g, pc, insn, states.get(pc))
        lines.append(f"{h(pc)} {g.evidence(pc):>8}  {insn_text(insn):<36}" + ("  ; " + "; ".join(notes) if notes else ""))
        previous_end = pc + insn.size
    return "\n".join(lines)


def build_model(g: Game, with_disassembly: bool) -> dict:
    nodes = []
    for entry in g.entry_list:
        routine = g.routines[entry]
        stores, _ = memory_summary(g, entry)
        hits = max((g.hits.get(g.block_of.get(pc, pc), 0) for pc in routine.pcs), default=0)
        node = {
            "id": h(entry), "name": routine.name, "kinds": kinds_text(routine.kinds),
            "root": any(k.startswith("vector") or k in ("entry", "seed") for k in routine.kinds),
            "uncertain": g.dispatch_hint.get(entry, ""), "n": len(routine.pcs), "hits": hits,
            "writes": {region: count for region, count in stores.items()},
            "video": sorted(r for r in stores if r in VIDEO_REGIONS),
        }
        if with_disassembly:
            node["dis"] = routine_listing(g, entry)
        nodes.append(node)
    unresolved = [e for e in g.edges if e.dst == DISPATCH]
    nodes.append({"id": "?dispatch", "name": "?dispatch", "kinds": "pseudo", "root": True,
                  "uncertain": "runtime f3_dispatch: unresolved computed jmp/jsr sites lead here; "
                               "profile-hit entries with no static predecessor leave from here",
                  "n": 0, "hits": 0, "writes": {}, "video": [],
                  "dis": "\n".join(f"{h(e.site)}  in {g.label(e.src):<14} {insn_text(g.code[e.site])}"
                                   for e in sorted(unresolved, key=lambda e: e.site))})
    edges = [[("?dispatch" if e.src == DISPATCH else h(e.src)), ("?dispatch" if e.dst == DISPATCH else h(e.dst)),
              e.kind, h(e.site) if e.site >= 0 else ""] for e in g.edges]
    return {"game": g.id, "a5": h(g.a5) if g.a5 is not None else None,
            "profile": g.profile_path.name if g.hits else None, "nodes": nodes, "edges": edges}


def write_graph(g: Game, path: Path, with_disassembly: bool = True) -> None:
    model = build_model(g, with_disassembly)
    data = json.dumps(model, separators=(",", ":")).replace("</", "<\\/")
    path.write_text(TEMPLATE.replace("/*DATA*/null", data).replace("@GAME@", g.id))


TEMPLATE = r"""<!doctype html>
<html><head><meta charset="utf-8"><title>@GAME@ call graph (f3a)</title>
<style>
:root{--bg:#14161a;--panel:#1c1f25;--fg:#d7dae0;--muted:#8a909c;--border:#2c313a;--accent:#61afef;--warn:#e5c07b;--err:#e06c75;--ok:#98c379;--vio:#c678dd;--org:#d19a66;--cyan:#56b6c2}
*{box-sizing:border-box}body{margin:0;font:13px/1.4 ui-monospace,Menlo,monospace;background:var(--bg);color:var(--fg);display:grid;grid-template-columns:280px 1fr 520px;height:100vh;overflow:hidden}
#side,#detail{background:var(--panel);border-right:1px solid var(--border);overflow:auto;padding:10px}#detail{border-left:1px solid var(--border);border-right:0}
h1{font-size:14px;margin:0 0 8px}h2{font-size:12px;color:var(--muted);margin:14px 0 4px;text-transform:uppercase;letter-spacing:.05em}
input[type=text],select{width:100%;background:var(--bg);color:var(--fg);border:1px solid var(--border);padding:5px;font:inherit}
label{display:block;color:var(--muted)}label input{vertical-align:middle}
.res div,.list div{cursor:pointer;padding:1px 3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.res div:hover,.list div:hover{background:var(--border)}
#wrap{position:relative;overflow:hidden}canvas{display:block}
#bar{position:absolute;top:8px;left:8px;display:flex;gap:6px;align-items:center;background:#1c1f25e0;padding:4px 6px;border:1px solid var(--border)}
button{background:var(--bg);color:var(--fg);border:1px solid var(--border);padding:3px 8px;font:inherit;cursor:pointer}button.on{border-color:var(--accent);color:var(--accent)}
pre{white-space:pre;overflow:auto;font-size:11.5px;background:var(--bg);padding:6px;border:1px solid var(--border);max-height:55vh}
pre a,.list a{color:var(--accent);cursor:pointer;text-decoration:none}.k{color:var(--muted)}.warn{color:var(--warn)}.legend span{display:inline-block;margin-right:8px}
#tip{position:absolute;pointer-events:none;background:#000d;padding:3px 6px;border:1px solid var(--border);display:none;white-space:nowrap}
</style></head><body>
<div id="side">
<h1>@GAME@ call graph</h1><div class="k" id="meta"></div>
<h2>Find</h2><input id="q" type="text" placeholder="name, sub_xxxxxx or hex address"><div class="res" id="res"></div>
<h2>Highlight</h2><select id="hl"><option value="">— nothing —</option><option value="uncertain">uncertain origin (dispatch?)</option><option value="video">writes any video region</option></select>
<h2>Edges</h2><div id="kinds"></div>
<h2>Focus depth</h2><label>callers <input id="dc" type="number" min="0" max="6" value="2" style="width:48px"></label><label>callees <input id="dn" type="number" min="0" max="6" value="2" style="width:48px"></label>
<h2>Legend</h2><div class="legend k" id="legend"></div>
<h2>Roots</h2><div class="list" id="roots"></div>
</div>
<div id="wrap"><canvas id="c"></canvas><div id="bar"><button id="bFocus" class="on">focus</button><button id="bAll">overview</button><button id="bBack">◀ back</button><span class="k" id="status"></span></div><div id="tip"></div></div>
<div id="detail"><div class="k">Click a node, or search. Edges: caller → callee. Click addresses in the listing to jump.</div></div>
<script>
const M=/*DATA*/null;
const cssCache=new Map();const css=n=>{if(!cssCache.has(n))cssCache.set(n,getComputedStyle(document.documentElement).getPropertyValue(n).trim());return cssCache.get(n)};
const MAXCOL=60;let clipped=0;
const EK={call:['call','--muted',0],"table-call":['table-call (jump table jsr)','--accent',0],table:['table (jump table jmp)','--vio',0],tail:['tail (branch into routine)','--cyan',1],fall:['fall-through','--border',1],ref:['ref (code address taken)','--ok',2],regconst:['regconst (jsr (aN) const)','--org',0],regtable:['regtable (jsr via ROM pointer table)','--org',1],slot:['slot (candidate: code pointer stored at the same struct offset)','--warn',2],dispatch:['dispatch (unresolved)','--err',1]};
const byId=new Map(M.nodes.map(n=>[n.id,n]));const out=new Map(),inn=new Map();
for(const n of M.nodes){out.set(n.id,[]);inn.set(n.id,[])}
for(const e of M.edges){out.get(e[0])?.push(e);inn.get(e[1])?.push(e)}
const on=new Set(Object.keys(EK).filter(k=>k!=='fall'));
const $=id=>document.getElementById(id);
$('meta').textContent=`${M.nodes.length-1} routines · ${M.edges.length} edges · a5=${M.a5||'?'} · profile=${M.profile||'none'}`;
for(const [k,[label,c,dash]] of Object.entries(EK)){const l=document.createElement('label');l.innerHTML=`<input type=checkbox ${on.has(k)?'checked':''}> <span style="color:${css(c)}">■</span> ${label} (${M.edges.filter(e=>e[2]===k).length})`;l.querySelector('input').onchange=ev=>{ev.target.checked?on.add(k):on.delete(k);draw()};$('kinds').append(l)}
$('legend').innerHTML=`<span style="color:${css('--accent')}">● root (vector/entry/seed)</span><span style="color:${css('--warn')}">● uncertain origin</span><span style="color:${css('--err')}">● ?dispatch</span><span>● other</span><br>node size ∝ instructions; ring = highlighted`;
for(const n of M.nodes.filter(n=>n.root&&n.id!=='?dispatch').sort((a,b)=>a.id<b.id?-1:1)){const d=document.createElement('div');d.textContent=`${n.name} ${n.kinds}`;d.onclick=()=>focus(n.id);$('roots').append(d)}
{const d=document.createElement('div');d.innerHTML=`<span style="color:${css('--err')}">?dispatch</span> (uncertain flow)`;d.onclick=()=>focus('?dispatch');$('roots').prepend(d)}
const regions=[...new Set(M.nodes.flatMap(n=>Object.keys(n.writes)))].sort();for(const r of regions){const o=document.createElement('option');o.value='w:'+r;o.textContent='writes '+r;$('hl').append(o)}
$('hl').onchange=draw;$('dc').onchange=$('dn').onchange=()=>layout();
const cv=$('c'),ctx=cv.getContext('2d');let W,H,dpr=devicePixelRatio||1;
let mode='focus',cur=null,hist=[],pos=new Map(),vis=[],view={x:0,y:0,s:1},hover=null;
function resize(){const r=$('wrap').getBoundingClientRect();W=r.width;H=r.height;cv.width=W*dpr;cv.height=H*dpr;cv.style.width=W+'px';cv.style.height=H+'px';draw()}
addEventListener('resize',resize);
function radius(n){return n.id==='?dispatch'?9:Math.max(3,Math.min(12,2+Math.sqrt(n.n)/2))}
function color(n){if(n.id==='?dispatch')return css('--err');if(n.uncertain)return css('--warn');if(n.root)return css('--accent');return css('--fg')}
function highlighted(n){const v=$('hl').value;if(!v)return false;if(v==='uncertain')return !!n.uncertain||n.id==='?dispatch';if(v==='video')return n.video.length>0;return !!n.writes[v.slice(2)]}
function layout(){pos=new Map();if(mode==='all'){overview()}else if(cur){focusLayout()}fit();draw()}
let fcol=new Map();
function focusLayout(){const dc=+$('dc').value,dn=+$('dn').value;const col=new Map([[cur,0]]);
 // ?dispatch links hundreds of routines: show it as a node, but only expand through it when it is the focus.
 const through=id=>id===cur||id!=='?dispatch';
 let fr=[cur];for(let d=1;d<=dc;d++){const nx=[];for(const id of fr)if(through(id))for(const e of inn.get(id)||[])if(on.has(e[2])&&!col.has(e[0])){col.set(e[0],-d);nx.push(e[0])}fr=nx}
 fr=[cur];for(let d=1;d<=dn;d++){const nx=[];for(const id of fr)if(through(id))for(const e of out.get(id)||[])if(on.has(e[2])&&!col.has(e[1])){col.set(e[1],d);nx.push(e[1])}fr=nx}
 const cols=new Map();for(const [id,c] of col){if(!cols.has(c))cols.set(c,[]);cols.get(c).push(id)}clipped=0;
 for(const [c,ids] of cols){ids.sort((a,b)=>(byId.get(b).hits-byId.get(a).hits)||(a<b?-1:1));if(c!==0&&ids.length>MAXCOL){clipped+=ids.length-MAXCOL;for(const id of ids.slice(MAXCOL))col.delete(id);ids.length=MAXCOL}ids.sort();ids.forEach((id,i)=>pos.set(id,{x:c*300,y:(i-(ids.length-1)/2)*42}))}vis=[...pos.keys()];fcol=col}
let ov=null;function overview(){if(!ov){ov=new Map();const depth=new Map();const q=[];for(const n of M.nodes)if(n.root||!(inn.get(n.id)||[]).length){depth.set(n.id,0);q.push(n.id)}
 for(let i=0;i<q.length;i++){const id=q[i];for(const e of out.get(id)||[])if(e[2]!=='fall'&&!depth.has(e[1])){depth.set(e[1],depth.get(id)+1);q.push(e[1])}}
 for(const n of M.nodes)if(!depth.has(n.id))depth.set(n.id,0);const layers=new Map();for(const [id,d] of depth){if(!layers.has(d))layers.set(d,[]);layers.get(d).push(id)}
 const order=new Map();for(const d of [...layers.keys()].sort((a,b)=>a-b)){const ids=layers.get(d);const bc=id=>{const ps=(inn.get(id)||[]).map(e=>order.get(e[0])).filter(v=>v!==undefined);return ps.length?ps.reduce((a,b)=>a+b,0)/ps.length:1e9};ids.sort((a,b)=>bc(a)-bc(b)||(a<b?-1:1));ids.forEach((id,i)=>{order.set(id,i);ov.set(id,{x:d*300,y:i*22})})}}
 pos=new Map(ov);vis=[...pos.keys()]}
function fit(){if(!pos.size)return;let x0=1e9,y0=1e9,x1=-1e9,y1=-1e9;for(const p of pos.values()){x0=Math.min(x0,p.x);x1=Math.max(x1,p.x);y0=Math.min(y0,p.y);y1=Math.max(y1,p.y)}
 let s=Math.min(1.6,Math.min((W-120)/Math.max(1,x1-x0+200),(H-80)/Math.max(1,y1-y0+40)));if(mode==='focus')s=Math.min(1,Math.max(.55,s));view.s=Math.max(.02,s);view.x=W/2-(x0+x1)/2*view.s;view.y=H/2-(y0+y1)/2*view.s}
const T=p=>[p.x*view.s+view.x,p.y*view.s+view.y];
function draw(){if(!W)return;ctx.setTransform(dpr,0,0,dpr,0,0);ctx.fillStyle=css('--bg');ctx.fillRect(0,0,W,H);const set=new Set(vis);
 for(const e of M.edges){if(!on.has(e[2])||!set.has(e[0])||!set.has(e[1]))continue;if(mode==='focus'&&fcol.get(e[1])-fcol.get(e[0])!==1)continue;const [x0,y0]=T(pos.get(e[0])),[x1,y1]=T(pos.get(e[1]));const [,c,dash]=EK[e[2]];
  const focusEdge=cur&&(e[0]===cur||e[1]===cur);ctx.strokeStyle=css(c);ctx.globalAlpha=mode==='all'&&!focusEdge?.25:.85;ctx.lineWidth=focusEdge?1.6:1;ctx.setLineDash(dash===1?[5,4]:dash===2?[2,3]:[]);
  ctx.beginPath();ctx.moveTo(x0,y0);const mx=(x0+x1)/2;ctx.bezierCurveTo(mx,y0,mx,y1,x1,y1);ctx.stroke();
  if(mode==='focus'){const a=Math.atan2(y1-y0,x1-mx);ctx.setLineDash([]);ctx.beginPath();ctx.moveTo(x1-8,y1-3);ctx.lineTo(x1,y1);ctx.lineTo(x1-8,y1+3);ctx.stroke()}}
 ctx.globalAlpha=1;ctx.setLineDash([]);
 for(const id of vis){const n=byId.get(id),[x,y]=T(pos.get(id)),r=radius(n)*(mode==='all'?Math.max(.4,view.s):1);ctx.fillStyle=color(n);ctx.beginPath();ctx.arc(x,y,r,0,7);ctx.fill();
  if(highlighted(n)){ctx.strokeStyle=css('--ok');ctx.lineWidth=2.5;ctx.beginPath();ctx.arc(x,y,r+3,0,7);ctx.stroke()}
  if(id===cur){ctx.strokeStyle=css('--accent');ctx.lineWidth=2;ctx.beginPath();ctx.arc(x,y,r+6,0,7);ctx.stroke()}
  const label=n.name+(n.video.length?' ['+n.video.join(',')+']':'');ctx.font='12px ui-monospace,Menlo,monospace';ctx.lineWidth=4;ctx.strokeStyle=css('--bg');ctx.fillStyle=css('--fg');
  if(mode==='focus'){ctx.textAlign='center';ctx.strokeText(label,x,y+r+14);ctx.fillText(label,x,y+r+14);ctx.textAlign='left'}
  else if(view.s>=0.9||id===cur){ctx.strokeText(label,x+r+4,y+4);ctx.fillText(label,x+r+4,y+4)}}
 $('status').textContent=mode==='focus'&&cur?`${vis.length} nodes around ${byId.get(cur).name}`+(clipped?` (+${clipped} hidden: columns keep the ${MAXCOL} most-executed; see the detail list)`:''):`${vis.length} nodes`}
function nodeAt(mx,my){let best=null,bd=1e9;for(const id of vis){const [x,y]=T(pos.get(id));const d=(x-mx)**2+(y-my)**2;if(d<bd){bd=d;best=id}}return bd<14*14?best:null}
let drag=null;cv.onmousedown=e=>{drag={x:e.offsetX,y:e.offsetY,vx:view.x,vy:view.y,moved:false}};
cv.onmousemove=e=>{if(drag){const dx=e.offsetX-drag.x,dy=e.offsetY-drag.y;if(Math.abs(dx)+Math.abs(dy)>3)drag.moved=true;view.x=drag.vx+dx;view.y=drag.vy+dy;draw();return}
 const id=nodeAt(e.offsetX,e.offsetY),tip=$('tip');if(id){const n=byId.get(id);tip.style.display='block';tip.style.left=(e.offsetX+14)+'px';tip.style.top=(e.offsetY+10)+'px';tip.textContent=`${n.name} ${n.id} · ${n.kinds} · ${n.n} insns${n.hits?' · hits '+n.hits:''}`}else tip.style.display='none'};
cv.onmouseup=e=>{if(drag&&!drag.moved){const id=nodeAt(e.offsetX,e.offsetY);if(id)focus(id)}drag=null};
cv.onwheel=e=>{e.preventDefault();const k=Math.exp(-e.deltaY*.0015);view.x=e.offsetX-(e.offsetX-view.x)*k;view.y=e.offsetY-(e.offsetY-view.y)*k;view.s*=k;draw()};
function focus(id,push=true){if(!byId.has(id))return;if(push&&cur&&cur!==id)hist.push(cur);cur=id;if(mode==='all'){pos=new Map(ov);vis=[...pos.keys()];draw()}else layout();detail(id)}
$('bFocus').onclick=()=>{mode='focus';$('bFocus').classList.add('on');$('bAll').classList.remove('on');layout()};
$('bAll').onclick=()=>{mode='all';$('bAll').classList.add('on');$('bFocus').classList.remove('on');layout();if(cur){const p=pos.get(cur);if(p){view.s=1;view.x=W/2-p.x;view.y=H/2-p.y;draw()}}};
$('bBack').onclick=()=>{if(hist.length)focus(hist.pop(),false)};
const esc=s=>s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const nameToId=new Map(M.nodes.map(n=>[n.name,n.id]));
function linkify(text){return esc(text).replace(/\b(sub_[0-9a-f]{6}|\?dispatch|[A-Za-z_][\w]*)\b/g,(m)=>nameToId.has(m)?`<a data-id="${nameToId.get(m)}">${m}</a>`:m)}
function edgeList(list,side){if(!list.length)return '<div class="k">none</div>';return list.map(e=>{const id=side==='in'?e[0]:e[1];const n=byId.get(id);return `<div><a data-id="${id}">${n?n.name:id}</a> <span class="k">${e[2]}${e[3]?' @'+e[3]:''}</span></div>`}).join('')}
function detail(id){const n=byId.get(id);const ins=(inn.get(id)||[]),outs=(out.get(id)||[]);
 $('detail').innerHTML=`<h1>${esc(n.name)} <span class="k">${n.id}</span></h1><div class="k">${esc(n.kinds)} · ${n.n} instructions${n.hits?' · max block hits '+n.hits:''}</div>`+
 (n.uncertain?`<div class="warn">uncertain origin: ${esc(n.uncertain)}</div>`:'')+
 `<h2>Callers (${ins.length})</h2><div class="list">${edgeList(ins,'in')}</div><h2>Callees (${outs.length})</h2><div class="list">${edgeList(outs,'out')}</div>`+
 `<h2>Writes</h2><div class="k">${Object.entries(n.writes).map(([r,c])=>`${r}: ${c}`).join(' · ')||'none'}</div>`+
 (n.dis!==undefined?`<h2>${id==='?dispatch'?'Unresolved computed transfers':'Disassembly'}</h2><pre>${linkify(n.dis)}</pre>`:'');
 for(const a of $('detail').querySelectorAll('a[data-id]'))a.onclick=()=>focus(a.dataset.id)}
$('q').oninput=()=>{const q=$('q').value.trim().toLowerCase().replace(/^\$|^0x/,'');const r=$('res');r.innerHTML='';if(!q)return;
 const hex=/^[0-9a-f]+$/.test(q)?parseInt(q,16):null;let hits=M.nodes.filter(n=>n.name.toLowerCase().includes(q)||n.id.includes(q));
 if(hex!==null){const owner=M.nodes.filter(n=>n.dis&&n.dis.includes('0x'+hex.toString(16).padStart(6,'0')+' '));for(const o of owner)if(!hits.includes(o))hits.push(o)}
 for(const n of hits.slice(0,40)){const d=document.createElement('div');d.textContent=`${n.name} ${n.kinds}`;d.onclick=()=>focus(n.id);r.append(d)}};
resize();const start=M.nodes.find(n=>n.kinds.includes('vector:reset'))||M.nodes[0];focus(start.id);
</script></body></html>
"""
