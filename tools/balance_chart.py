#!/usr/bin/env python3
"""balance_chart.py — render the balance lab's CSV into one self-contained HTML page.

Usage:  python3 tools/balance_chart.py balance.csv -o balance.html
Input:  the CSV written by the "balance report" doctest case (BALANCE_REPORT=...).
Output: static HTML, no external assets. Pure stdlib so it runs anywhere the repo does.

Page: a posture selector (Tanky / Moderate / Glass Cannon) over six SVG line charts
(X = effective floor 1-150 everywhere; difficulty seams dashed at 50.5/100.5), each
with the three damage-archetype series (Magic/Melee/Ranged) as fixed-color 2px lines,
p10-p90 bands where the metric has percentiles, boss markers on the TTK chart, a
crosshair tooltip, and a "worst offenders" table of the biggest floor-over-floor
ttkTrash jumps. Colors follow the project dataviz rules (one unit per axis, color
follows the entity, text wears ink colors) and were validated for CVD separation.
"""
import argparse
import csv
import json
import math
import sys

# Fixed category order — color follows the entity (col index), never rank.
COLS = ["Magic", "Melee", "Ranged"]
ROWS = ["Tanky", "Moderate", "Glass Cannon"]

CHARTS = [  # (title, y-label, metric key, band keys or None, log-y)
    ("Time-to-kill trash",  "seconds", "ttkTrash",     None,                 False),
    ("Hits to die",         "hits",    "hitsToDie",    None,                 False),
    ("Seconds to die",      "seconds", "secondsToDie", None,                 False),
    ("Player total DPS",    "dmg/s",   "tDps50",       ("tDps10", "tDps90"), True),
    ("Player effective HP", "HP",      "ehp50",        ("ehp10", "ehp90"),   True),
    ("Enemy HP (median)",   "HP",      "enHpMed",      None,                 True),
]


def _metric_keys():
    """Every CSV column the charts consume (medians + percentile bands)."""
    keys = set()
    for _title, _unit, key, band, _log in CHARTS:
        keys.add(key)
        if band:
            keys.update(band)
    return keys


def load(path):
    """Parse the CSV into row dicts: every field float except bossName (str).

    bossName is RFC-4180 double-quoted in the file (names contain commas) —
    csv.DictReader handles that natively. Non-finite numerics are clamped to 0
    so the JSON we later emit can never contain NaN/Infinity tokens.
    """
    needed = _metric_keys() | {"difficulty", "floor", "effFloor", "cell",
                               "row", "col", "bossName", "bossHp", "bossHit"}
    rows = []
    try:
        f = open(path, newline="", encoding="utf-8")
    except OSError as e:
        sys.exit(f"balance_chart: cannot open {path}: {e.strerror or e}")
    with f:
        rd = csv.DictReader(f)
        missing = needed - set(rd.fieldnames or [])
        if missing:
            sys.exit("balance_chart: CSV is missing columns: " + ", ".join(sorted(missing)))
        for rec in rd:
            row = {}
            for k, v in rec.items():
                if k == "bossName":
                    row[k] = (v or "").strip()
                    continue
                try:
                    x = float(v)
                except (TypeError, ValueError):
                    x = 0.0
                if not math.isfinite(x):  # belt-and-braces: JSON must stay finite
                    x = 0.0
                row[k] = x
            rows.append(row)
    if not rows:  # header-only CSV would otherwise render a silently blank page
        sys.exit(f"balance_chart: no data rows in {path}")
    return rows


def build_series(rows):
    """Pivot rows into chart series + the deduped boss list.

    Returns (out, bosses) where out[postureRow][metricKey][col] is a list of
    [effFloor, value] pairs sorted by floor, and bosses is one record per boss
    FLOOR (deduped across the 9 build cells that all repeat it) sorted by
    effFloor — 10 distinct bosses x 3 difficulties.
    """
    keys = _metric_keys()
    out = {p: {k: {c: [] for c in range(3)} for k in keys} for p in range(3)}
    bosses = []
    seen_boss_floor = set()
    for r in rows:
        p, c, eff = int(r["row"]), int(r["col"]), int(r["effFloor"])
        for k in keys:
            out[p][k][c].append([eff, round(r[k], 3)])
        if r["bossName"] and eff not in seen_boss_floor:
            seen_boss_floor.add(eff)
            bosses.append({"eff": eff, "floor": int(r["floor"]),
                           "diff": int(r["difficulty"]), "name": r["bossName"],
                           "hp": round(r["bossHp"], 1), "hit": round(r["bossHit"], 1)})
    for p in out:
        for k in out[p]:
            for c in out[p][k]:
                out[p][k][c].sort(key=lambda d: d[0])
    bosses.sort(key=lambda b: b["eff"])
    return out, bosses


def worst_offenders(rows, top_n=10):
    """Top-N floor-over-floor ttkTrash jumps across all posture/column series.

    Ratio is max(after/before, before/after) so a cliff DOWN ranks as high as a
    spike UP. Difficulty seams (eff 50->51, 100->101) are consecutive on the
    effective-floor axis, so a harsh difficulty step surfaces here by design.
    """
    series = {}
    for r in rows:
        series.setdefault((int(r["row"]), int(r["col"])), []).append(
            (int(r["effFloor"]), r["ttkTrash"]))
    out = []
    for (p, c), pts in series.items():
        pts.sort()
        for (f0, v0), (f1, v1) in zip(pts, pts[1:]):
            if v0 <= 0 or v1 <= 0:  # a zero TTK would make the ratio meaningless
                continue
            ratio = max(v1 / v0, v0 / v1)
            out.append({"from": f0, "to": f1, "posture": ROWS[p], "col": COLS[c],
                        "before": round(v0, 3), "after": round(v1, 3),
                        "ratio": round(ratio, 2)})
    out.sort(key=lambda d: (-d["ratio"], d["from"]))
    return out[:top_n]


# One <figure> per chart is emitted statically (JS fills the <svg> innerHTML on
# draw) so the file structurally contains its six charts even before scripting.
FIG_TMPL = ('<figure class="chart"><h3>{title} <span class="unit">{unit}</span></h3>'
            '<svg id="svg{i}" viewBox="0 0 460 240" role="img" aria-label="{title}"></svg>'
            '</figure>\n')

TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Balance Lab Report</title>
<style>
/* Surfaces + ink per the project dataviz rules; series colors are CSS vars so the
   dark-mode swap is pure CSS and every SVG stroke/fill follows automatically. */
:root{
  --bg:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e;
  --grid:#e6e5e1; --panel:#ffffff; --border:#e0dfda;
  --c0:#2a78d6; --c1:#eb6834; --c2:#1baf7a;   /* Magic / Melee / Ranged */
}
@media (prefers-color-scheme: dark){
  :root{
    --bg:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7;
    --grid:#32322f; --panel:#222221; --border:#343430;
    --c0:#3987e5; --c1:#d95926; --c2:#199e70;
  }
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
  font:14px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;padding:20px 22px 40px}
main{max-width:1420px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:16px;margin:28px 0 8px}
p.lede{color:var(--ink2);margin:0 0 14px;font-size:13px}
.chip{white-space:nowrap;color:var(--ink)}
.chip i{display:inline-block;width:10px;height:10px;border-radius:2px;margin:0 4px -1px 2px}
.seg{display:inline-flex;border:1px solid var(--border);border-radius:8px;overflow:hidden;margin:2px 0 16px}
.seg button{border:0;background:var(--panel);color:var(--ink2);font:inherit;font-size:13px;
  padding:6px 14px;cursor:pointer;border-right:1px solid var(--border)}
.seg button:last-child{border-right:0}
.seg button[aria-pressed="true"]{background:var(--ink);color:var(--bg);font-weight:600}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(430px,100%),1fr));gap:16px}
figure.chart{margin:0;padding:10px 12px 6px;border:1px solid var(--border);
  border-radius:8px;background:var(--panel)}
figure.chart h3{font-size:14px;margin:0 0 4px;font-weight:600}
figure.chart .unit{color:var(--ink2);font-weight:400;font-size:12px}
svg{width:100%;height:auto;display:block}
svg text{font:10px system-ui,sans-serif}
#tip{position:fixed;display:none;pointer-events:none;z-index:10;max-width:280px;
  background:var(--panel);border:1px solid var(--border);border-radius:6px;
  padding:6px 9px;font-size:12px;color:var(--ink);box-shadow:0 2px 10px rgba(0,0,0,.15)}
#tip .mut{color:var(--ink2)}
#tip i{display:inline-block;width:8px;height:8px;border-radius:2px;margin-right:5px}
table.off{border-collapse:collapse;width:100%;max-width:860px;font-size:13px}
table.off th{color:var(--ink2);font-size:11px;text-transform:uppercase;letter-spacing:.04em;
  text-align:left;padding:5px 10px;border-bottom:1px solid var(--border)}
table.off td{padding:5px 10px;border-bottom:1px solid var(--border)}
table.off .num{text-align:right;font-variant-numeric:tabular-nums}
table.off th.num{text-align:right}
</style>
</head>
<body>
<main>
<h1>Balance Lab Report</h1>
<p class="lede">
  <span class="chip"><i style="background:var(--c0)"></i>Magic</span> &middot;
  <span class="chip"><i style="background:var(--c1)"></i>Melee</span> &middot;
  <span class="chip"><i style="background:var(--c2)"></i>Ranged</span> &mdash;
  lines are p50 medians, shaded bands span p10&ndash;p90. X is effective floor
  (floor + 50&times;difficulty, 1&ndash;150); dashed rules mark the difficulty
  seams (D0&rarr;D1&rarr;D2) at floors 50/100.
</p>
<div class="seg" role="group" aria-label="Posture">
  <button data-p="0" aria-pressed="false">Tanky</button>
  <button data-p="1" aria-pressed="true">Moderate</button>
  <button data-p="2" aria-pressed="false">Glass Cannon</button>
</div>
<div class="grid">
__FIGURES__
</div>
<h2>Worst offenders &mdash; floor-over-floor TTK jumps</h2>
<p class="lede">Largest ratio changes in time-to-kill trash between consecutive
effective floors, across all postures and columns (both spikes and cliffs).</p>
<table class="off">
  <thead><tr><th class="num">From</th><th class="num">To</th><th>Posture</th>
  <th>Column</th><th class="num">Before</th><th class="num">After</th>
  <th class="num">Ratio</th></tr></thead>
  <tbody id="offbody"></tbody>
</table>
<div id="tip"></div>
<script>
"use strict";
const DATA=__DATA__;
const CHARTS=__CHARTS__;
const BOSSES=__BOSSES__;
const OFF=__OFF__;

// Plot geometry (viewBox units). One shared frame for all six charts.
const L=52,R=10,T=24,B=30,W=460,H=240,PW=W-L-R,PH=H-T-B;
const SERIES=["Magic","Melee","Ranged"];
let posture=1;                      // default Moderate

// bossByEff lets the crosshair tooltip append the boss line on boss floors.
const bossByEff={}; BOSSES.forEach(b=>{bossByEff[b.eff]=b;});

const X=f=>L+(f-1)/149*PW;          // effFloor 1..150 -> plot x
const esc=s=>String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;")
                      .replace(/>/g,"&gt;").replace(/"/g,"&quot;");
function fmt(v){                    // magnitude-aware value formatting
  if(v>=10000) return Math.round(v).toLocaleString("en-US");
  if(v>=1000)  return (Math.round(v*10)/10).toLocaleString("en-US");
  if(v>=100)   return v.toFixed(0);
  if(v>=10)    return v.toFixed(1);
  return v.toFixed(2);
}
function fmtTick(v){                // compact axis labels (60k not 60,000)
  if(v>=1e6)  return (v/1e6)+"M";   // tDps decade ceiling can reach 10^6
  if(v>=1000) return (v/1000)+"k";
  if(v>0&&v<1) return String(+v.toFixed(2));
  return String(v);
}
function niceTicks(max){            // linear axis: 0..niceMax in 1/2/5 steps
  if(max<=0) max=1;
  const raw=max/4, mag=Math.pow(10,Math.floor(Math.log10(raw))), n=raw/mag;
  const step=(n<1.5?1:n<3?2:n<7?5:10)*mag;
  const top=Math.ceil(max/step)*step, ticks=[];
  for(let v=0;v<=top+step/2;v+=step) ticks.push(+v.toFixed(6));
  return {max:top,ticks};
}
function valAt(arr,f){              // arr is complete 1..150, index with fallback
  const d=arr[f-1];
  if(d&&d[0]===f) return d[1];
  const m=arr.find(q=>q[0]===f);
  return m?m[1]:null;
}

function drawChart(i){
  const [title,unit,key,band,log]=CHARTS[i];
  const svg=document.getElementById("svg"+i);
  const D=DATA[posture];            // JSON keys are strings; JS coerces the number
  const lines=[0,1,2].map(c=>D[key][c]);
  const bands=band?[0,1,2].map(c=>[D[band[0]][c],D[band[1]][c]]):null;

  // Y domain: linear charts anchor at 0; log charts span whole decades around
  // the data (bands included) with values <=0 clamped to the axis minimum.
  let vmax=0,vminPos=Infinity;
  const scan=a=>a.forEach(d=>{const v=d[1]; if(v>vmax)vmax=v; if(v>0&&v<vminPos)vminPos=v;});
  lines.forEach(scan);
  if(bands) bands.forEach(pr=>pr.forEach(scan));
  if(!isFinite(vminPos)) vminPos=1;
  let Y,ticks;
  if(log){
    const y0=Math.pow(10,Math.floor(Math.log10(vminPos)));
    let y1=Math.pow(10,Math.ceil(Math.log10(Math.max(vmax,vminPos))));
    if(y1<=y0) y1=y0*10;
    const l0=Math.log10(y0),l1=Math.log10(y1);
    Y=v=>T+PH*(1-(Math.log10(Math.max(v,y0))-l0)/(l1-l0));
    ticks=[]; for(let d=l0;d<=l1+1e-9;d++) ticks.push(Math.pow(10,Math.round(d)));
  }else{
    const nt=niceTicks(vmax);
    Y=v=>T+PH*(1-v/nt.max);
    ticks=nt.ticks;
  }

  let s="";
  // gridlines + y tick labels (recessive; text wears ink colors, never series colors)
  ticks.forEach(t=>{
    const y=Y(t).toFixed(1);
    s+=`<line x1="${L}" x2="${W-R}" y1="${y}" y2="${y}" stroke="var(--grid)" stroke-width="1"/>`;
    s+=`<text x="${L-6}" y="${y}" dy="0.32em" text-anchor="end" fill="var(--ink2)">${fmtTick(t)}</text>`;
  });
  // x baseline + ticks at 1/50/100/150
  s+=`<line x1="${L}" x2="${W-R}" y1="${T+PH}" y2="${T+PH}" stroke="var(--ink2)" stroke-opacity="0.45"/>`;
  [1,50,100,150].forEach(f=>{
    s+=`<text x="${X(f).toFixed(1)}" y="${T+PH+13}" text-anchor="middle" fill="var(--ink2)">${f}</text>`;
  });
  // difficulty seams: dashed rules at 50.5 / 100.5, tiny region tag on the right
  [[50.5,"D1"],[100.5,"D2"]].forEach(([f,tag])=>{
    const x=X(f).toFixed(1);
    s+=`<line x1="${x}" x2="${x}" y1="${T}" y2="${T+PH}" stroke="var(--ink2)" stroke-opacity="0.5" stroke-dasharray="4 3"/>`;
    s+=`<text x="${+x+4}" y="${T+10}" fill="var(--ink2)" opacity="0.8" font-size="9">${tag}</text>`;
  });
  // p10-p90 bands first (under the lines), translucent series fill
  if(bands) bands.forEach((pr,c)=>{
    const lo=pr[0],hi=pr[1];
    let p="M"+lo.map(d=>X(d[0]).toFixed(1)+" "+Y(d[1]).toFixed(1)).join("L");
    for(let k=hi.length-1;k>=0;k--) p+="L"+X(hi[k][0]).toFixed(1)+" "+Y(hi[k][1]).toFixed(1);
    s+=`<path d="${p}Z" fill="var(--c${c})" fill-opacity="0.13" stroke="none"/>`;
  });
  // median lines, 2px, fixed series colors
  lines.forEach((pts,c)=>{
    const pl=pts.map(d=>X(d[0]).toFixed(1)+","+Y(d[1]).toFixed(1)).join(" ");
    s+=`<polyline points="${pl}" fill="none" stroke="var(--c${c})" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
  });
  // direct labels at the right line ends: colored end-cap on the point, ink text
  // above it, decluttered so the three labels never overlap.
  const ends=lines.map((pts,c)=>{
    const d=pts[pts.length-1];
    return {c,x:X(d[0]),y:Y(d[1]),ly:Y(d[1])-5};
  }).sort((a,b)=>a.ly-b.ly);
  for(let k=1;k<ends.length;k++)                       // push down on overlap
    if(ends[k].ly-ends[k-1].ly<11) ends[k].ly=ends[k-1].ly+11;
  for(let k=ends.length-1;k>=0;k--){                   // clamp back into the frame
    const cap=k===ends.length-1?T+PH-2:ends[k+1].ly-11;
    if(ends[k].ly>cap) ends[k].ly=cap;
    if(ends[k].ly<12) ends[k].ly=12;
  }
  ends.forEach(e=>{
    s+=`<rect x="${(e.x-3).toFixed(1)}" y="${(e.y-3).toFixed(1)}" width="6" height="6" rx="1" fill="var(--c${e.c})"/>`;
    s+=`<text x="${W-R}" y="${e.ly.toFixed(1)}" text-anchor="end" fill="var(--ink2)">${SERIES[e.c]}</text>`;
  });
  // crosshair layer (hidden until hover): a snapped rule + one dot per series
  s+=`<g class="ch" style="display:none">`
    +`<line y1="${T}" y2="${T+PH}" stroke="var(--ink2)" stroke-opacity="0.6" stroke-width="1"/>`
    +[0,1,2].map(c=>`<circle r="3.5" fill="var(--c${c})" stroke="var(--bg)" stroke-width="1.5"/>`).join("")
    +`</g>`;
  // transparent overlay drives the crosshair; boss dots go AFTER it so their
  // native <title> tooltips still receive hover.
  s+=`<rect class="ov" x="${L}" y="${T}" width="${PW}" height="${PH}" fill="transparent"/>`;
  if(key==="ttkTrash") BOSSES.forEach(b=>{
    s+=`<circle cx="${X(b.eff).toFixed(1)}" cy="${T+PH-5}" r="3" fill="var(--ink2)">`
      +`<title>${esc("Floor "+b.eff+" — "+b.name+" (HP "+Math.round(b.hp)+")")}</title></circle>`;
  });
  svg.innerHTML=s;

  // hover wiring: snap to the nearest effFloor, move the crosshair + dots,
  // fill the fixed tooltip with the three series values (+ band range, + boss).
  const ov=svg.querySelector(".ov"),ch=svg.querySelector(".ch"),tip=document.getElementById("tip");
  const chLine=ch.querySelector("line"),chDots=ch.querySelectorAll("circle");
  ov.addEventListener("mousemove",ev=>{
    const r=svg.getBoundingClientRect();
    const vx=(ev.clientX-r.left)*(W/r.width);
    const f=Math.max(1,Math.min(150,Math.round(1+(vx-L)/PW*149)));
    const x=X(f).toFixed(1);
    chLine.setAttribute("x1",x); chLine.setAttribute("x2",x);
    const d=f<=50?0:f<=100?1:2;
    let h=`<b>eff. floor ${f}</b> <span class="mut">(D${d} floor ${f-50*d})</span>`;
    [0,1,2].forEach(c=>{
      const v=valAt(lines[c],f);
      chDots[c].setAttribute("cx",x);
      chDots[c].setAttribute("cy",v==null?-99:Y(v).toFixed(1));
      let row=`<br><i style="background:var(--c${c})"></i>${SERIES[c]}: <b>${v==null?"–":fmt(v)}</b>`;
      if(bands&&v!=null){
        const lo=valAt(bands[c][0],f),hi=valAt(bands[c][1],f);
        if(lo!=null&&hi!=null) row+=` <span class="mut">(${fmt(lo)}–${fmt(hi)})</span>`;
      }
      h+=row;
    });
    const b=key==="ttkTrash"?bossByEff[f]:null;
    if(b) h+=`<br><span class="mut">Boss: ${esc(b.name)}</span>`;
    tip.innerHTML=h;
    tip.style.display="block";
    ch.style.display="";
    // place beside the cursor, flipping when the tip would leave the viewport
    const tw=tip.offsetWidth,th=tip.offsetHeight;
    let px=ev.clientX+14,py=ev.clientY+14;
    if(px+tw>innerWidth-8) px=ev.clientX-tw-14;
    if(py+th>innerHeight-8) py=ev.clientY-th-14;
    tip.style.left=px+"px"; tip.style.top=py+"px";
  });
  ov.addEventListener("mouseleave",()=>{
    ch.style.display="none"; tip.style.display="none";
  });
}

function drawAll(){ for(let i=0;i<CHARTS.length;i++) drawChart(i); }

// posture selector: switching redraws every chart from that posture row's data
document.querySelectorAll(".seg button").forEach(btn=>{
  btn.addEventListener("click",()=>{
    posture=+btn.dataset.p;
    document.querySelectorAll(".seg button").forEach(b=>
      b.setAttribute("aria-pressed",b===btn?"true":"false"));
    drawAll();
  });
});

// offenders table (posture-independent; built once)
document.getElementById("offbody").innerHTML=OFF.map(o=>
  `<tr><td class="num">${o.from}</td><td class="num">${o.to}</td>`
  +`<td>${esc(o.posture)}</td><td>${esc(o.col)}</td>`
  +`<td class="num">${o.before.toFixed(2)} s</td><td class="num">${o.after.toFixed(2)} s</td>`
  +`<td class="num">&times;${o.ratio.toFixed(2)}</td></tr>`).join("");

drawAll();
</script>
</main>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(
        description="Render the balance lab CSV into one self-contained HTML chart page.")
    ap.add_argument("csv_path", help="input CSV from the BALANCE_REPORT doctest case")
    ap.add_argument("-o", "--out", default=None,
                    help="output HTML path (default: input with .html extension)")
    args = ap.parse_args()

    out = args.out
    if out is None:
        base = args.csv_path[:-4] if args.csv_path.lower().endswith(".csv") else args.csv_path
        out = base + ".html"

    rows = load(args.csv_path)
    data, bosses = build_series(rows)
    off = worst_offenders(rows)

    figures = "".join(
        FIG_TMPL.format(i=i, title=title,
                        unit=unit + (" &middot; log scale" if log else ""))
        for i, (title, unit, _key, _band, log) in enumerate(CHARTS))

    def j(obj):
        # allow_nan=False guarantees no NaN/Infinity tokens; the </ escape stops a
        # string from ever closing the inline <script> block early.
        return json.dumps(obj, separators=(",", ":"), allow_nan=False).replace("</", "<\\/")

    html = (TEMPLATE
            .replace("__FIGURES__", figures)
            .replace("__DATA__", j(data))
            .replace("__CHARTS__", j(CHARTS))
            .replace("__BOSSES__", j(bosses))
            .replace("__OFF__", j(off)))

    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print(f"wrote {out} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
