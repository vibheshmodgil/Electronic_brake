// ============================================================
// WebPage.h - the dashboard, served from flash
// ============================================================
//
// One self-contained page. No CDN, no external stylesheet, no chart
// library, no web font. The one thing it loads from elsewhere is the
// CAMERA card's JPEGs, from the camera board on this same access point
// (firmware/EBrakeCam) - never from the internet.
//
// That is not a preference. A phone joined to this board's access point
// has NO route to the internet, so anything fetched from a CDN simply
// never loads and the page renders broken. Everything the dashboard needs
// has to be in this string. Charts are drawn on a <canvas> by hand for
// the same reason.
//
// ONE LAYOUT, THREE SHAPES
// -------------------------------------------------------------------
// The panels are a CSS grid whose column count changes with the width,
// so the same markup serves a phone, a tablet and a laptop:
//
//   under 700px    2 columns   everything stacks, TPS and RPM side by side
//   700 - 1100px   4 columns   charts sit two across
//   over 1100px    6 columns   every card on one row, charts two across
//
// There is no separate mobile page to keep in step, and no user-agent
// sniffing - it reflows on rotation and on a resized browser window.
//
// HISTORY LIVES IN THE BROWSER, not on the board: the page keeps its own
// ring buffer and the board only ever sends the present moment. Firmware
// RAM stays flat no matter how long a session runs, and the only cost is
// that a device joining late starts with an empty chart.
//
// The thresholds drawn on the charts arrive in the JSON, from the same
// constants the algorithm compares against. Nothing here hardcodes 5.0 or
// 20 - the page cannot drift from the firmware.
//
#pragma once

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>EBrake Monitor</title>
<style>
:root{
--bg:#0d1117;--panel:#161b22;--edge:#30363d;--grid:#283039;
--fg:#e6edf3;--muted:#8b949e;--dim:#6e7681;
--tps:#4ea3ff;--rpm:#3fb950;--red:#f85149;--green:#3fb950;
--amber:#d29922;--ink:#0d1117;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0;background:var(--bg);color:var(--fg);
font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
-webkit-text-size-adjust:100%}
body{padding:10px 10px calc(14px + env(safe-area-inset-bottom))}
.wrap{max-width:1500px;margin:0 auto}

header{display:flex;align-items:baseline;justify-content:space-between;
gap:10px;margin:2px 2px 4px}
header h1{font-size:clamp(12px,3.4vw,15px);margin:0;letter-spacing:.06em}
#link{font-size:clamp(10px,3vw,12px);font-weight:700;color:var(--dim);
white-space:nowrap}
#sub{font-size:10px;color:var(--dim);margin:0 2px 10px}

/* one grid, three column counts */
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.grid>*{min-width:0}
.card{background:var(--panel);border:1px solid var(--edge);border-radius:8px;
padding:10px 12px}
.b-banner,.b-gate,.b-can,.b-chart,.b-events,.b-cam{grid-column:span 2}
.b-tps,.b-rpm{grid-column:span 1}
@media(min-width:700px){
  .grid{grid-template-columns:repeat(4,1fr);gap:10px}
  .b-banner{grid-column:span 2}
  .b-gate,.b-can,.b-chart{grid-column:span 2}
  .b-events,.b-cam{grid-column:span 4}
}
@media(min-width:1100px){
  .grid{grid-template-columns:repeat(6,1fr)}
  .b-banner{grid-column:span 2}
  .b-tps,.b-rpm,.b-gate,.b-can{grid-column:span 1}
  .b-chart{grid-column:span 3}
  .b-events{grid-column:span 6}
  /* camera beside the two charts, all three on one row */
  .b-cam,.b-cam:not([hidden])~.b-chart{grid-column:span 2}
}

.k{font-size:10px;color:var(--muted);letter-spacing:.05em}
.v{font-size:clamp(20px,5.5vw,26px);line-height:1.15;margin-top:2px}
.s{font-size:10px;color:var(--muted);margin-top:3px}

#banner{display:flex;flex-direction:column;justify-content:center;
text-align:center;padding:14px 12px;transition:background .15s}
#bstate{font-size:clamp(22px,6.5vw,30px);font-weight:700;letter-spacing:.03em}
#bsub{font-size:11px;margin-top:3px;opacity:.85}

.bar{height:9px;background:var(--grid);border-radius:5px;overflow:hidden;
margin:8px 0 6px}
.bar>i{display:block;height:100%;width:0;background:var(--amber);
border-radius:5px;transition:width .12s linear}
.chips{display:flex;flex-wrap:wrap;gap:6px 14px;font-size:10px;color:var(--dim)}

canvas{width:100%;height:120px;display:block;margin-top:4px}
@media(min-width:700px){canvas{height:150px}}
@media(min-width:1100px){canvas{height:190px}}

#events div{font-size:10px;color:var(--muted);margin-top:4px;
white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.row{display:flex;justify-content:space-between;align-items:center;
gap:8px;flex-wrap:wrap}
.btn{text-decoration:none;font:inherit;font-size:10px;color:var(--fg);background:none;cursor:pointer;
border:1px solid var(--edge);border-radius:5px;padding:4px 9px}
#msgs details{border-top:1px solid var(--edge);padding:7px 0}
#msgs details:first-child{margin-top:8px}
#msgs summary{font-size:11px;cursor:pointer}
#msgs summary .id{color:var(--tps)}
.meta{font-size:10px;color:var(--dim);margin:2px 0 0 14px;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sig{display:flex;align-items:center;gap:9px;font-size:11px;
padding:6px 0 6px 14px;cursor:pointer}
.sig span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pred{font-style:normal;font-size:9px;color:var(--amber)}
.sig b{font-weight:400;color:var(--muted);white-space:nowrap}
.sig input{width:18px;height:18px;margin:0;accent-color:var(--green)}
.stale{opacity:.45}
.note{font-size:9px;color:var(--dim);margin:8px 2px 0}
#camImg{display:block;width:100%;aspect-ratio:4/3;object-fit:contain;
background:#000;border-radius:4px;margin-top:6px;transition:opacity .2s}
@media(min-width:700px){#camImg{max-height:55vh}}
@media(min-width:1100px){#camImg{aspect-ratio:auto;height:190px}}
</style></head><body>
<div class="wrap">

<header><h1>EBRAKE MONITOR</h1><span id="link">CONNECTING</span></header>
<div id="sub">&nbsp;</div>

<div class="grid">

  <div class="card b-banner" id="banner">
    <div id="bstate">NO DATA</div>
    <div id="bsub">waiting for the board</div>
  </div>

  <div class="card b-tps"><div class="k">TPS</div>
    <div class="v" id="tps">--</div><div class="s" id="tpsS">&nbsp;</div></div>

  <div class="card b-rpm"><div class="k">RPM</div>
    <div class="v" id="rpm">--</div><div class="s" id="rpmS">&nbsp;</div></div>

  <div class="card b-gate"><div class="k">APPLY GATE</div>
    <div class="v" id="gate" style="font-size:19px">--</div>
    <div class="bar"><i id="gbar"></i></div>
    <div class="chips"><span id="c1"></span><span id="c2"></span></div></div>

  <div class="card b-can"><div class="k">CAN LINK</div>
    <div class="v" id="can" style="font-size:19px">--</div>
    <div class="s" id="canS">&nbsp;</div></div>

  <div class="card b-cam" id="camCard" hidden>
    <div class="row"><span class="k">CAMERA</span>
      <span class="s" id="camS" style="margin:0;flex:1">&nbsp;</span>
      <button class="btn" id="camBtn">pause</button>
      <a class="btn" id="camFull" target="_blank" rel="noopener">full</a></div>
    <img id="camImg" alt=""></div>

  <div class="card b-chart"><div class="k">TPS [%]</div>
    <canvas id="cTps"></canvas></div>

  <div class="card b-chart"><div class="k">SPEED [rpm]</div>
    <canvas id="cRpm"></canvas></div>

  <div class="card b-events" id="events"><div class="k">EVENTS</div></div>

  <!-- toggled CAN signals land here, as grid items like the charts above -->
  <div id="plots" style="display:contents"></div>

  <div class="card b-events">
    <div class="row"><span class="k">CAN SIGNALS</span>
      <span><label class="btn">LOAD .DBC<input type="file" id="dbcFile" accept=".dbc" hidden></label>
      <button class="btn" id="dbcBuiltIn" hidden>BUILT-IN</button>
      <a class="btn" href="/can.dbc" download="ebrake.dbc">SAVE .DBC</a></span></div>
    <div class="s" id="dbcS">&nbsp;</div><div class="s" id="over"></div>
    <div id="msgs"></div>
  </div>

</div>

<div class="note">History is kept by this device, so it starts empty on
connect. Thresholds come from the firmware's own constants.</div>
</div>

<script>
var N=200, hT=[], hR=[], hB=[], lastOk=0, cfg=null, evSig="";
function $(i){return document.getElementById(i)}
function push(a,v){a.push(v); if(a.length>N)a.shift()}

function fit(c){
  var r=c.getBoundingClientRect(), d=window.devicePixelRatio||1;
  var w=Math.round(r.width*d), h=Math.round(r.height*d);
  if(c.width!=w||c.height!=h){ c.width=w; c.height=h }
  var x=c.getContext('2d'); x.setTransform(d,0,0,d,0,0);
  return {x:x,w:r.width,h:r.height};
}

// data, colour, shaded band [lo,hi], values the axis must always include,
// brake state per sample
function chart(c,data,col,band,inc,brk){
  var g=fit(c), x=g.x, w=g.w, h=g.h, i;
  x.clearRect(0,0,w,h);
  if(!data.length){return}

  var lo=Infinity, hi=-Infinity;
  for(i=0;i<data.length;i++){ if(data[i]<lo)lo=data[i]; if(data[i]>hi)hi=data[i] }
  for(i=0;i<inc.length;i++){ if(inc[i]<lo)lo=inc[i]; if(inc[i]>hi)hi=inc[i] }
  var sp=hi-lo; if(sp<1e-6)sp=1;
  lo-=sp*0.18; hi+=sp*0.18; sp=hi-lo;

  var Y=function(v){ return h-(v-lo)/sp*h };
  // newest sample sits at the right edge and older data runs left, so a
  // partly filled buffer grows backwards instead of hanging off the left
  var X=function(k){ return w-(data.length-1-k)/(N-1)*w };

  // brake-applied shading, so the chart says when it was braked
  x.fillStyle='rgba(248,81,73,.10)';
  for(i=0;i<brk.length&&i<data.length;i++){
    if(brk[i]){ var x0=X(i); x.fillRect(x0,0,Math.max(1,X(i+1)-x0),h) }
  }

  if(band){
    x.fillStyle=col+'26';
    x.fillRect(0,Y(band[1]),w,Math.max(1,Y(band[0])-Y(band[1])));
    x.strokeStyle=col+'99'; x.lineWidth=1; x.setLineDash([4,3]);
    [band[0],band[1]].forEach(function(v){
      x.beginPath(); x.moveTo(0,Y(v)); x.lineTo(w,Y(v)); x.stroke() });
    x.setLineDash([]);
  }

  x.strokeStyle=col; x.lineWidth=1.8; x.beginPath();
  for(i=0;i<data.length;i++){
    var px=X(i), py=Y(data[i]);
    i?x.lineTo(px,py):x.moveTo(px,py);
  }
  x.stroke();

  x.fillStyle='#6e7681'; x.font='9px monospace';
  x.fillText(hi.toFixed(1),3,10); x.fillText(lo.toFixed(1),3,h-3);
}

function stale(on){
  document.body.classList.toggle('stale',on);
  if(on){
    $('link').textContent='NO SIGNAL'; $('link').style.color='var(--red)';
    $('banner').style.background='var(--panel)';
    $('bstate').textContent='STALE'; $('bstate').style.color='var(--red)';
    $('bsub').textContent='the board stopped answering';
    $('bsub').style.color='var(--muted)';
    $('can').textContent='?'; $('can').style.color='var(--dim)';
    $('canS').textContent='unknown while the link is down';
  }
}

function paint(d){
  cfg=d; camConfig(d);
  $('link').textContent='LIVE'; $('link').style.color='var(--green)';
  $('sub').textContent='up '+(d.uptime/1000).toFixed(0)+' s   '
    +d.changes+' state changes';

  var ap=d.brake===1;
  $('banner').style.background=ap?'var(--red)':'var(--green)';
  $('bstate').textContent=ap?'BRAKE APPLIED':'BRAKE RELEASED';
  $('bstate').style.color='var(--ink)';
  $('bsub').textContent=ap?'relay OFF - shaft held':'relay ON - shaft free';
  $('bsub').style.color='var(--ink)';

  var above=d.tps>=d.relTh, below=d.tps<d.appTh;
  $('tps').textContent=d.tps.toFixed(2)+' %';
  $('tps').style.color=above?'var(--amber)':'var(--tps)';
  $('tpsS').textContent=above?('at/above release '+d.relTh.toFixed(1)+' %')
    :(below?('below apply '+d.appTh.toFixed(1)+' %')
           :('in band '+d.appTh.toFixed(1)+' - '+d.relTh.toFixed(1)+' %'));

  var slow=Math.abs(d.rpm)<d.rpmTh;
  $('rpm').textContent=d.rpm;
  $('rpm').style.color=slow?'var(--rpm)':'var(--amber)';
  $('rpmS').textContent=slow?('stopped   |rpm| < '+d.rpmTh)
                            :('turning   |rpm| >= '+d.rpmTh);

  if(ap){
    $('gate').textContent='HELD'; $('gate').style.color='var(--muted)';
    $('gbar').style.width='0%';
    $('c1').textContent='releases at TPS >= '+d.relTh.toFixed(1)+' %';
    $('c1').style.color='var(--dim)'; $('c2').textContent='';
  }else{
    if(d.timer===1){
      $('gate').textContent=(d.timerMs/1000).toFixed(1)+' / '
        +(d.delayMs/1000).toFixed(1)+' s';
      $('gate').style.color='var(--amber)';
      $('gbar').style.width=(100*d.timerMs/d.delayMs)+'%';
    }else{
      $('gate').textContent='open'; $('gate').style.color='var(--muted)';
      $('gbar').style.width='0%';
    }
    $('c1').textContent=below?'TPS ok':'TPS high';
    $('c1').style.color=below?'var(--green)':'var(--amber)';
    $('c2').textContent=slow?'RPM ok':'RPM high';
    $('c2').style.color=slow?'var(--green)':'var(--amber)';
  }

  $('can').textContent=d.canOk?'OK':'FAULT';
  $('can').style.color=d.canOk?'var(--green)':'var(--red)';
  $('canS').textContent='0x0B7 '+(d.ageTps<0?'never':d.ageTps+' ms')
    +'   0x015 '+(d.ageRpm<0?'never':d.ageRpm+' ms')
    +'   limit '+d.canTimeout+' ms';

  push(hT,d.tps); push(hR,d.rpm); push(hB,ap?1:0);
  redraw(d);
}

function redraw(d){
  chart($('cTps'),hT,'#4ea3ff',[d.appTh,d.relTh],[0,d.relTh],hB);
  chart($('cRpm'),hR,'#3fb950',[-d.rpmTh,d.rpmTh],[-d.rpmTh,d.rpmTh],hB);
  redrawSigs();
}

// ============================================================
// CAN signals
// ============================================================
//
// The board sends each ID's latest raw bytes and serves its DBC at
// /can.dbc (CanDbc.h); decoding happens here. A signal is treated as a
// prediction unless its CM_ comment starts with CONFIRMED, and says so
// on screen - most of that DBC is inferred from message names.
//
// An ID the DBC does not describe still gets toggles, as raw bytes and
// big-endian 16-bit words, so nothing on the bus is invisible.

function sg(n,st,len,intel,sgn,f,o,u){
  return {n:n,st:st,len:len,intel:intel,sgn:sgn,f:f,o:o,u:u}}

var COLS=['#d2a8ff','#ff8f4d','#56d4dd','#e3b341','#ff7b72','#7ee787','#79c0ff'];
var dbc=null, frames={}, H={}, HB={}, sel={}, plots={}, listSig='', RAW={};
var STALE_MS=1000;   // a frame older than this is no longer a live value

function store(k,v){ try{ localStorage.setItem(k,v) }catch(e){} }
function load(k){ try{ return localStorage.getItem(k) }catch(e){ return null } }

function parseDbc(text){
  var names={}, sigs={}, cur=null, n=0, ok=0, m;
  text.split(/\r?\n/).forEach(function(l){
    m=l.match(/^BO_\s+(\d+)\s+(\w+)\s*:/);
    if(m){ cur=+m[1]; names[cur]=m[2]; sigs[cur]=[]; n++; return }
    m=l.match(/^\s*SG_\s+(\w+)[^:]*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(\s*([^,\s]+)\s*,\s*([^)\s]+)\s*\)\s*\[[^\]]*\]\s*"([^"]*)"/);
    if(m&&cur!=null) sigs[cur].push(sg(m[1],+m[2],+m[3],m[4]==='1',m[5]==='-',+m[6],+m[7],m[8]));
  });
  var re=/CM_\s+SG_\s+(\d+)\s+(\w+)\s+"CONFIRMED/g;
  while((m=re.exec(text)))(sigs[+m[1]]||[]).forEach(function(g){
    if(g.n===m[2]){ g.ok=true; ok++ } });
  return n?{names:names,sigs:sigs,n:n,ok:ok}:null;
}

// Intel counts up from the LSB; Motorola starts at the MSB and walks the
// DBC's sawtooth bit numbering. Multiplying keeps 32+ bit signals exact.
function decode(b,s){
  var v=0, i, k=s.st;
  for(i=0;i<s.len;i++){
    if(s.intel)k=s.st+s.len-1-i;
    if((k>>3)>=b.length)return NaN;
    v=v*2+((b[k>>3]>>(k&7))&1);
    if(!s.intel)k=(k%8===0)?k+15:k-1;
  }
  if(s.sgn&&v>=Math.pow(2,s.len-1))v-=Math.pow(2,s.len);
  return v*s.f+s.o;
}

function nameOf(id){ return (dbc&&dbc.names[id])||'not in DBC' }
function sigsOf(id){
  if(dbc&&dbc.sigs[id]&&dbc.sigs[id].length)return dbc.sigs[id];
  var f=frames[id]; if(!f)return [];
  var n=f.d.length/2, k=id+':'+n, i;
  if(!RAW[k]){
    var s=[];
    for(i=0;i<n;i++)s.push(sg('byte'+i,i*8+7,8,0,0,1,0,''));
    for(i=0;i+1<n;i+=2){
      s.push(sg('word'+i+'-'+(i+1)+' u16',i*8+7,16,0,0,1,0,''));
      s.push(sg('word'+i+'-'+(i+1)+' s16',i*8+7,16,0,1,1,0,''));
    }
    s.raw=true; RAW[k]=s;
  }
  return RAW[k];
}
// the CONFIRMED/PREDICTED convention is the built-in DBC's own; a DBC
// loaded from a file is taken as it is
function tag(ss,g){ return ss.raw?'raw':(g.ok||dbc.file?'':'predicted') }
function hex(id){ return '0x'+(id&0x1FFFFFFF).toString(16).toUpperCase()+(id>=0x80000000?' ext':'') }
function fmt(v){ return isNaN(v)?'--':String(+v.toFixed(3)) }
function el(t,c,x){ var e=document.createElement(t); if(c)e.className=c;
  if(x!=null)e.textContent=x; return e }

// rebuilt only when the set of messages changes, so an open <details> and
// a checkbox mid-tap survive the 250 ms updates
function buildList(){
  var ids={}, k;
  if(dbc)for(k in dbc.names)ids[k]=1;
  for(k in frames)ids[k]=1;
  var list=Object.keys(ids).map(Number).sort(function(a,b){return a-b});
  // received IDs are part of the key: a message's raw toggles only exist
  // once its first frame has shown how long it is
  var s=list.join()+'|'+Object.keys(frames).join()+'|'+(dbc?dbc.n:'');
  if(s===listSig)return; listSig=s;

  var box=$('msgs'); box.textContent='';
  list.forEach(function(id){
    var d=el('details'), sm=el('summary');
    sm.appendChild(el('span','id',hex(id))); sm.appendChild(document.createTextNode(' '+nameOf(id)));
    d.appendChild(sm);
    d.appendChild(el('div','meta',frames[id]?'':'never received')).id='m'+id;
    var ss=sigsOf(id);
    ss.forEach(function(g){
      var key=id+':'+g.n, row=el('label','sig'), cb=el('input');
      cb.type='checkbox'; cb.checked=!!sel[key];
      cb.onchange=function(){ toggle(key,cb.checked) };
      row.appendChild(cb); row.appendChild(el('span','',g.n));
      row.appendChild(el('i','pred',tag(ss,g)));
      row.appendChild(el('b','','--')).id='v'+key;
      d.appendChild(row);
    });
    box.appendChild(d);
  });
  $('dbcS').textContent=!dbc?'loading the DBC from the board'
    :dbc.file?(dbc.file+' - '+dbc.n+' messages, loaded on this device')
    :(dbc.n+' messages - '+dbc.ok+' signals confirmed, the rest predicted until checked');
  $('dbcBuiltIn').hidden=!(dbc&&dbc.file);
  syncPlots();
}

function findSig(key){
  var p=key.indexOf(':'), id=+key.slice(0,p), n=key.slice(p+1);
  return sigsOf(id).filter(function(g){return g.n===n})[0];
}

function toggle(key,on){
  if(on)sel[key]=1; else delete sel[key];
  store('sel',JSON.stringify(sel));
  var cb=document.getElementById('v'+key);
  if(cb)cb.parentNode.firstChild.checked=on;
  syncPlots();
}

function syncPlots(){
  var k, i=0;
  for(k in plots)if(!sel[k]||!findSig(k)){ plots[k].card.remove(); delete plots[k] }
  for(k in sel){
    var g=findSig(k); if(!g||plots[k])continue;
    var card=el('div','card b-chart'), head=el('div','row'), x=el('button','btn','hide');
    var id=+k.split(':')[0];
    var title=el('span','k',hex(id)+' '+nameOf(id)+'  '+g.n+(g.u?' ['+g.u+']':'')+'  ');
    title.appendChild(el('i','pred',tag(sigsOf(id),g)));
    head.appendChild(title);
    var val=el('span','v'); val.style.fontSize='15px'; head.appendChild(val);
    x.onclick=(function(key){return function(){toggle(key,false)}})(k);
    head.appendChild(x); card.appendChild(head);
    var c=el('canvas'); card.appendChild(c);
    $('plots').appendChild(card);
    plots[k]={card:card,c:c,val:val};
  }
  for(k in plots)plots[k].col=COLS[i++%COLS.length];
  redrawSigs();
}

function redrawSigs(){
  for(var k in plots){
    var p=plots[k], h=H[k]||[];
    p.val.textContent=h.length&&!h.stale?fmt(h[h.length-1]):'--';
    p.val.style.color=p.col;
    chart(p.c,h,p.col,null,[],HB[k]||[]);
  }
}

function paintCan(d){
  frames={};
  d.f.forEach(function(f){ frames[f.id]=f });
  buildList();
  var brk=hB.length?hB[hB.length-1]:0;
  for(var id in frames){
    var f=frames[id], b=[], i;
    for(i=0;i<f.d.length;i+=2)b.push(parseInt(f.d.substr(i,2),16));
    var m=$('m'+id);
    if(m)m.textContent=f.age+' ms ago   x'+f.n+'   '+(f.d.match(/../g)||[]).join(' ');
    // the board keeps an ID's last payload forever; once it stops arriving
    // show '--' and stop plotting, rather than draw a flat line that looks live
    var old=f.age>STALE_MS;
    sigsOf(+id).forEach(function(g){
      var key=id+':'+g.n, v=old?NaN:decode(b,g);
      if(!H[key]){H[key]=[];HB[key]=[]}
      if(!isNaN(v)){ push(H[key],v); push(HB[key],brk) }
      H[key].stale=old;
      var o=document.getElementById('v'+key);
      if(o)o.textContent=fmt(v)+(g.u&&!isNaN(v)?' '+g.u:'');
    });
  }
  $('over').textContent=d.over?(d.over+' frames from IDs past the board\'s table, not shown'):'';
  redrawSigs();
}

// aborted like poll(), so a dead connection after a board reset cannot
// stall the signal view until the browser's own TCP timeout
function pollCan(){
  var ac=new AbortController(), to=setTimeout(function(){ac.abort()},1200);
  fetch('/api/can',{signal:ac.signal,cache:'no-store'}).then(function(r){return r.json()})
    .then(paintCan).catch(function(){}).finally(function(){
      clearTimeout(to); setTimeout(pollCan,250) });
}

function useDbc(p){ dbc=p; H={}; HB={}; listSig=''; buildList() }

// once per page load, retried until the board answers - unless this device
// has its own DBC loaded, which then wins
function loadDbc(){
  fetch('/can.dbc',{cache:'no-store'}).then(function(r){return r.text()})
    .then(function(t){ var p=parseDbc(t); if(!p)throw 0;
      if(!(dbc&&dbc.file))useDbc(p) })
    .catch(function(){ setTimeout(loadDbc,2000) });
}

// LOAD .DBC: a DBC from this phone or laptop, decoded here and kept in this
// browser only. Nothing is sent to the board, which keeps its built-in one.
function fileDbc(name,text){
  var p=parseDbc(text); if(!p)return false;
  p.file=name; useDbc(p); return true;
}
$('dbcFile').onchange=function(){
  var f=this.files[0]; this.value=''; if(!f)return;
  var r=new FileReader();
  r.onload=function(){
    if(!fileDbc(f.name,r.result)){ $('dbcS').textContent=f.name+': no BO_ messages found - not a DBC?'; return }
    // a big DBC can exceed browser storage; it still works until reload
    try{ localStorage.setItem('dbcText',r.result); localStorage.setItem('dbcName',f.name) }
    catch(e){ $('dbcS').textContent+='  (too big to remember - reload it next time)' }
  };
  r.readAsText(f);
};
$('dbcBuiltIn').onclick=function(){
  try{ localStorage.removeItem('dbcText'); localStorage.removeItem('dbcName') }catch(e){}
  dbc=null; loadDbc(); buildList();
};

try{ sel=JSON.parse(load('sel'))||{} }catch(e){ sel={} }
buildList();
if(!(load('dbcText')&&fileDbc(load('dbcName')||'saved.dbc',load('dbcText'))))loadDbc();

function events(list){
  var sig=JSON.stringify(list); if(sig===evSig)return; evSig=sig;
  var box=$('events');
  while(box.childNodes.length>1)box.removeChild(box.lastChild);
  for(var i=list.length-1;i>=0;i--){
    var e=document.createElement('div');
    e.textContent=(list[i].t/1000).toFixed(1)+' s   '+list[i].m;
    if(/CAN signals unavailable/.test(list[i].m))e.style.color='var(--red)';
    box.appendChild(e);
  }
}

function poll(){
  var ac=new AbortController(), to=setTimeout(function(){ac.abort()},1200);
  fetch('/api/status',{signal:ac.signal,cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(d){ clearTimeout(to); lastOk=Date.now();
      document.body.classList.remove('stale'); paint(d) })
    .catch(function(){ clearTimeout(to) })
    .finally(function(){
      if(Date.now()-lastOk>1500)stale(true);
      setTimeout(poll,150) });
}
function pollEvents(){
  var ac=new AbortController(), to=setTimeout(function(){ac.abort()},1200);
  fetch('/api/events',{signal:ac.signal,cache:'no-store'}).then(function(r){return r.json()})
    .then(events).catch(function(){}).finally(function(){
      clearTimeout(to); setTimeout(pollEvents,1000) });
}

// rotation and window resizes change the canvas size, so redraw from the
// history we already hold rather than waiting for the next poll
var rt=null;
window.addEventListener('resize',function(){
  clearTimeout(rt); rt=setTimeout(function(){ if(cfg)redraw(cfg) },120) });

// ============================================================
// Camera
// ============================================================
//
// Frames come from the camera board (firmware/EBrakeCam) at the address in
// the status JSON. One JPEG at a time, the next asked for as soon as the
// last one lands: several phones can watch at once, a slow link just gets
// fewer frames, and a dead camera is a timeout rather than a frozen image
// that looks live. The picture is never part of the brake decision.

var camHost=null, camOn=load('cam')!=='0', camAt=0, camT=[];

function camStatus(){
  var s=$('camS'), im=$('camImg'), age=Date.now()-camAt;
  $('camBtn').textContent=camOn?'pause':'play';
  if(!camOn){ s.textContent='paused'; s.style.color='var(--dim)'; im.style.opacity=.35; return }
  if(camAt&&age<2500){
    s.textContent=camT.length+' fps'; s.style.color='var(--green)'; im.style.opacity=1;
  }else{
    s.textContent=camAt?('NO PICTURE - last frame '+(age/1000).toFixed(0)+' s ago')
      :('connecting to '+camHost);
    s.style.color=camAt?'var(--red)':'var(--dim)'; im.style.opacity=.35;
  }
}

function pollCam(){
  if(!camOn||!camHost||document.hidden){ if(camHost)camStatus(); return setTimeout(pollCam,400) }
  var ac=new AbortController(), to=setTimeout(function(){ac.abort()},2500);
  fetch('http://'+camHost+'/capture',{signal:ac.signal,cache:'no-store'})
    .then(function(r){ if(!r.ok)throw 0; return r.blob() })
    .then(function(b){
      var im=$('camImg'), old=im.src;
      im.src=URL.createObjectURL(b);
      if(old.slice(0,5)==='blob:')URL.revokeObjectURL(old);
      var now=Date.now(); camAt=now; camT.push(now);
      while(camT.length&&now-camT[0]>1000)camT.shift();
    })
    .catch(function(){})
    .finally(function(){ clearTimeout(to); camStatus(); setTimeout(pollCam,camAt?30:1500) });
}

$('camBtn').onclick=function(){
  camOn=!camOn; store('cam',camOn?'1':'0'); camStatus() };

// the address arrives with the first status document
function camConfig(d){
  if(camHost!==null||!d)return;
  camHost=d.cam||'';
  if(!camHost)return;
  $('camCard').hidden=false;
  $('camFull').href='http://'+camHost+'/';
  camStatus(); pollCam();
}

poll(); pollEvents(); pollCan();
</script></body></html>)rawliteral";
