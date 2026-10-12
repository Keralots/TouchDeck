// Guition deck: the portal's Deck page (/deck). Home Assistant connection
// and a tile list per page with an entity picker. Reuses /portal.css.
// Only compiled into the JC3248W535 build (included from web_server.cpp).
#ifndef DECK_WEB_H
#define DECK_WEB_H

#if defined(BOARD_IS_JC3248W535)

#include <Arduino.h>

static const char DECK_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en" data-mode="dark" data-accent="green">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>StatNode Deck</title>
<script>(function(){try{document.documentElement.dataset.mode=localStorage.getItem('sn_mode')||'dark';document.documentElement.dataset.accent=localStorage.getItem('sn_accent')||'green';}catch(e){}})();</script>
<link rel="stylesheet" href="/portal.css?v=14">
<style>
.wrap{max-width:860px;margin:0 auto;padding:26px 18px 120px}
.top{display:flex;align-items:center;gap:12px;margin-bottom:22px}.top h1{margin:0;font-size:24px;font-weight:600;margin-right:auto}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.status{display:flex;align-items:center;gap:8px;font:13px var(--mono);color:var(--mute);margin-top:12px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--faint)}.dot.ok{background:var(--ok)}.dot.err{background:var(--err)}.dot.busy{background:var(--warn)}
.tabs{display:flex;gap:7px;flex-wrap:wrap;margin-bottom:14px}
.tiles{display:flex;flex-direction:column;gap:8px}
.tile{display:grid;grid-template-columns:36px minmax(0,1fr) auto;gap:12px;align-items:center;padding:10px 12px;border:1px solid var(--line);border-radius:var(--r-md);background:var(--card-2)}
.ic{width:36px;height:36px;border-radius:50%;display:grid;place-items:center;font:600 13px var(--mono);color:#111}
.tmain{min-width:0}.tmain input{max-width:none;padding:7px 10px;font-size:14px}
.tsub{font:11px var(--mono);color:var(--dim);margin-top:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tact{display:flex;gap:5px;align-items:center}.tact select{width:auto;padding:6px 8px;font-size:12px}.tact .btn{padding:6px 9px}
.cap{font:12px var(--mono);color:var(--dim);margin:10px 0 0}.cap.over{color:var(--err)}
.modal{position:fixed;inset:0;background:rgba(0,0,0,.55);display:none;align-items:flex-start;justify-content:center;z-index:80;padding:5vh 12px}
.modal.show{display:flex}
.sheet{background:var(--card);border:1px solid var(--line);border-radius:var(--r-lg);width:min(640px,100%);max-height:88vh;display:flex;flex-direction:column;box-shadow:0 20px 50px rgba(0,0,0,.4)}
.sheet-head{padding:16px 18px;border-bottom:1px solid var(--line)}.sheet-head input{max-width:none}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}.chips .chip{cursor:pointer}.chips .chip.on{border-color:var(--accent);background:var(--accent-soft);color:var(--ink)}
.list{overflow-y:auto;padding:6px}
.ent{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;padding:9px 12px;border-radius:var(--r-md);cursor:pointer}.ent:hover{background:var(--card-2)}
.ent b{display:block;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.ent small{font:11px var(--mono);color:var(--dim)}.ent .st{font:12px var(--mono);color:var(--mute);align-self:center}
.empty{padding:22px;text-align:center;color:var(--dim);font-size:13px}
.preview img{display:block;width:100%;max-width:480px;border-radius:8px;border:1px solid var(--line)}
.bar{position:fixed;left:0;right:0;bottom:0;background:var(--paper);border-top:1px solid var(--line);padding:12px 18px;display:flex;justify-content:center}
.bar-in{width:min(860px,100%);display:flex;align-items:center;gap:12px}.bar-in .msg{margin-right:auto;font:12px var(--mono);color:var(--dim)}
</style>
</head>
<body>
<div class="wrap">
  <div class="top"><h1>Deck</h1><a class="btn" href="/">Back to portal</a></div>

  <div class="card">
    <div class="card-head"><h2 class="card-title">Home Assistant</h2><span class="tag" id="haVer">-</span></div>
    <div class="grid-2">
      <div class="field"><label class="field-label" for="haUrl">Address</label><input type="text" id="haUrl" placeholder="http://192.168.0.7:8123" autocomplete="off"></div>
      <div class="field"><label class="field-label" for="haToken">Long-lived access token</label><input type="password" id="haToken" autocomplete="off" placeholder="paste the token"><p class="field-hint">HA profile, Security tab, Long-lived access tokens. Stored only on the device, never shown again.</p></div>
    </div>
    <div class="row"><button class="btn btn-accent" id="haSave">Save and connect</button><button class="btn" id="haForget">Forget</button></div>
    <div class="status"><span class="dot" id="haDot"></span><span id="haState">checking...</span></div>
  </div>

  <div class="card">
    <div class="card-head"><h2 class="card-title">Pages and tiles</h2><span class="tag" id="srcTag">-</span></div>
    <div class="tabs" id="tabs"></div>
    <div id="pageEdit"></div>
  </div>

  <div class="card preview">
    <div class="card-head"><h2 class="card-title">Device screen</h2><button class="btn" id="prevBtn">Refresh</button></div>
    <img id="prev" alt="Device screen">
  </div>
</div>

<div class="bar"><div class="bar-in"><span class="msg" id="msg">No changes</span><button class="btn btn-accent btn-lg" id="saveDeck">Save to device</button></div></div>

<div class="modal" id="picker"><div class="sheet">
  <div class="sheet-head"><div class="row"><input type="text" id="q" placeholder="Search by name or entity id" autocomplete="off" style="flex:1"><button class="btn" id="pickClose">Close</button></div><div class="chips" id="domains"></div></div>
  <div class="list" id="ents"></div>
</div></div>

<script>
(function(){'use strict';
const $=id=>document.getElementById(id);
const TOGGLE=['light','switch','fan','input_boolean','lock','cover','media_player','climate','automation','siren','humidifier','valve'];
const DOMS=[['','All'],['light','Lights'],['switch','Switches'],['fan','Fans'],['scene','Scenes'],['script','Scripts'],['lock','Locks'],['cover','Covers'],['media_player','Media'],['climate','Climate'],['button','Buttons'],['input_boolean','Helpers'],['automation','Automations']];
const COL={light:'#F6C56B',switch:'#5CC9A7',fan:'#5CC9A7',input_boolean:'#5CC9A7',scene:'#B3A6FF',script:'#B3A6FF',lock:'#9DB8FF',cover:'#9DB8FF'};
let deck=null,cur=0,ents=null,entMap={},dirty=false,pickFor=-1,dom='';
async function api(p,o){const r=await fetch(p,o);const t=await r.text();let j=null;try{j=JSON.parse(t);}catch(e){}if(!r.ok)throw new Error((j&&(j.message||j.error))||t||('HTTP '+r.status));return j;}
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function domainOf(e){const i=(e||'').indexOf('.');return i>0?e.slice(0,i):'';}
function setDirty(v){dirty=v;$('msg').textContent=v?'Unsaved changes':'Saved';}
function pages(){return deck.pages.filter(p=>p.type!=='metrics');}

/* ---- Home Assistant ---- */
async function haStatus(){try{const s=await api('/api/ha/status');if(document.activeElement!==$('haUrl'))$('haUrl').value=s.url||'';$('haToken').placeholder=s.has_token?'saved - leave empty to keep':'paste the token';const st=s.link;$('haDot').className='dot '+(st==='connected'?'ok':(st==='token rejected'||st==='unreachable')?'err':(st==='not set up'?'':'busy'));$('haState').textContent=st+(s.error?' - '+s.error:'');$('haVer').textContent=s.version?('HA '+s.version):'-';}catch(e){$('haState').textContent='status unavailable';}}
$('haSave').onclick=async()=>{const b=new URLSearchParams();b.set('url',$('haUrl').value.trim());b.set('token',$('haToken').value.trim());try{await api('/api/ha/config',{method:'POST',body:b});$('haToken').value='';ents=null;haStatus();}catch(e){alert(e.message);}};
$('haForget').onclick=async()=>{if(!confirm('Remove the Home Assistant address and token from the device?'))return;try{await api('/api/ha/forget',{method:'POST'});ents=null;haStatus();}catch(e){alert(e.message);}};

/* ---- deck editor ---- */
function tileSize(t){const p=t.pos&&t.pos.landscape;return p?((p.w||1)+'x'+(p.h||1)):'1x1';}
function setSize(t,v){const [w,h]=v.split('x').map(Number);if(w===1&&h===1){delete t.pos;return;}t.pos={landscape:{w:w,h:h}};}
function grid(p){const l=(p.layouts&&p.layouts.landscape)||{};return [l.cols||4,l.rows||3];}
function renderTabs(){const ps=pages();if(cur>=ps.length)cur=Math.max(0,ps.length-1);$('tabs').innerHTML=ps.map((p,i)=>'<button type="button" class="chip page-tab'+(i===cur?' on':'')+'" data-i="'+i+'">'+esc(p.name||p.id)+'</button>').join('')+'<button type="button" class="chip page-tab" id="addPage">+ Page</button>';
  $('tabs').querySelectorAll('[data-i]').forEach(b=>b.onclick=()=>{cur=+b.dataset.i;render();});$('addPage').onclick=()=>{const p={id:'p'+Date.now().toString(36),name:'New page',layouts:{landscape:{cols:4,rows:3}},tiles:[]};deck.pages.push(p);cur=pages().length-1;setDirty(true);render();};}
function renderPage(){const ps=pages();const el=$('pageEdit');if(!ps.length){el.innerHTML='<p class="empty">No deck pages. Add one with "+ Page".</p>';return;}
  const p=ps[cur];const [c,r]=grid(p);let used=0;p.tiles.forEach(t=>{const [w,h]=tileSize(t).split('x').map(Number);used+=w*h;});
  let h='<div class="grid-2"><div class="field"><label class="field-label">Page name</label><input type="text" id="pName" value="'+esc(p.name||'')+'"></div><div class="field"><label class="field-label">Grid (columns x rows)</label><div class="row"><select id="pCols" style="width:auto">'+[2,3,4,5,6].map(n=>'<option'+(n===c?' selected':'')+'>'+n+'</option>').join('')+'</select><span>x</span><select id="pRows" style="width:auto">'+[2,3,4].map(n=>'<option'+(n===r?' selected':'')+'>'+n+'</option>').join('')+'</select><button class="btn btn-danger" id="pDel" style="margin-left:auto">Delete page</button></div></div></div>';
  h+='<div class="tiles">'+p.tiles.map((t,i)=>{const d=domainOf(t.entity);const ent=entMap[t.entity];const nm=ent&&ent.name?ent.name:(t.entity||'No entity');
    return '<div class="tile"><div class="ic" style="background:'+(t.color||COL[d]||'#7DB5FF')+'">'+esc((d||'?').slice(0,2).toUpperCase())+'</div><div class="tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(nm)+'"><div class="tsub">'+esc(t.entity||'no entity')+' - '+(t.type==='toggle'?'switch':'button')+(ent&&ent.state?' - '+esc(ent.state):'')+'</div></div>'+
    '<div class="tact"><select data-t="'+i+'"><option value="toggle"'+(t.type==='toggle'?' selected':'')+'>Switch</option><option value="action"'+(t.type!=='toggle'?' selected':'')+'>Button</option></select><select data-s="'+i+'">'+['1x1','2x1','1x2','2x2'].map(v=>'<option'+(v===tileSize(t)?' selected':'')+'>'+v+'</option>').join('')+'</select><button class="btn" data-e="'+i+'" title="Change entity">Entity</button><button class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button class="btn" data-x="'+i+'" title="Remove">&#215;</button></div></div>';}).join('')+'</div>';
  h+='<p class="cap'+(used>c*r?' over':'')+'">'+used+' of '+(c*r)+' cells used'+(used>c*r?' - tiles that do not fit are not shown':'')+'</p><div class="row" style="margin-top:12px"><button class="btn btn-accent" id="addTile">+ Add tile from Home Assistant</button></div>';
  el.innerHTML=h;
  $('pName').oninput=e=>{p.name=e.target.value;setDirty(true);renderTabs();};
  $('pCols').onchange=e=>{p.layouts=p.layouts||{};p.layouts.landscape={cols:+e.target.value,rows:grid(p)[1]};setDirty(true);renderPage();};
  $('pRows').onchange=e=>{p.layouts=p.layouts||{};p.layouts.landscape={cols:grid(p)[0],rows:+e.target.value};setDirty(true);renderPage();};
  $('pDel').onclick=()=>{if(!confirm('Delete the page "'+(p.name||p.id)+'"?'))return;deck.pages.splice(deck.pages.indexOf(p),1);cur=0;setDirty(true);render();};
  el.querySelectorAll('[data-l]').forEach(x=>x.oninput=e=>{const t=p.tiles[+x.dataset.l];const v=e.target.value.trim();if(v)t.label=v;else delete t.label;setDirty(true);});
  el.querySelectorAll('[data-t]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.t];t.type=e.target.value;if(t.type==='toggle')delete t.tap;setDirty(true);renderPage();});
  el.querySelectorAll('[data-s]').forEach(x=>x.onchange=e=>{setSize(p.tiles[+x.dataset.s],e.target.value);setDirty(true);renderPage();});
  el.querySelectorAll('[data-e]').forEach(x=>x.onclick=()=>openPicker(+x.dataset.e));
  el.querySelectorAll('[data-u]').forEach(x=>x.onclick=()=>{const i=+x.dataset.u;if(i<1)return;const t=p.tiles.splice(i,1)[0];p.tiles.splice(i-1,0,t);setDirty(true);renderPage();});
  el.querySelectorAll('[data-x]').forEach(x=>x.onclick=()=>{p.tiles.splice(+x.dataset.x,1);setDirty(true);renderPage();});
  $('addTile').onclick=()=>openPicker(-1);}
function render(){renderTabs();renderPage();}

/* ---- entity picker ---- */
async function loadEnts(){if(ents)return;$('ents').innerHTML='<p class="empty">Loading entities from Home Assistant...</p>';try{ents=await api('/api/ha/entities');entMap={};ents.forEach(e=>entMap[e.id]=e);renderPage();}catch(e){ents=null;$('ents').innerHTML='<p class="empty">'+esc(e.message)+'<br>Set up Home Assistant above first.</p>';throw e;}}
function renderDomains(){$('domains').innerHTML=DOMS.map(d=>'<span class="chip'+(d[0]===dom?' on':'')+'" data-d="'+d[0]+'">'+d[1]+'</span>').join('');$('domains').querySelectorAll('[data-d]').forEach(c=>c.onclick=()=>{dom=c.dataset.d;renderDomains();renderEnts();});}
function renderEnts(){if(!ents)return;const q=$('q').value.trim().toLowerCase();let list=ents.filter(e=>(!dom||domainOf(e.id)===dom)&&(!q||e.id.toLowerCase().includes(q)||(e.name||'').toLowerCase().includes(q)));
  if(!dom&&!q){const pri=e=>TOGGLE.includes(domainOf(e.id))||['scene','script','button','input_button'].includes(domainOf(e.id))?0:1;list=list.slice().sort((a,b)=>pri(a)-pri(b));}
  const shown=list.slice(0,250);$('ents').innerHTML=shown.length?shown.map(e=>'<div class="ent" data-id="'+esc(e.id)+'"><div><b>'+esc(e.name||e.id)+'</b><small>'+esc(e.id)+'</small></div><span class="st">'+esc(e.state)+'</span></div>').join('')+(list.length>shown.length?'<p class="empty">'+(list.length-shown.length)+' more - refine the search</p>':''):'<p class="empty">Nothing matches.</p>';
  $('ents').querySelectorAll('[data-id]').forEach(r=>r.onclick=()=>pick(r.dataset.id));}
async function openPicker(i){pickFor=i;$('q').value='';$('picker').classList.add('show');renderDomains();try{await loadEnts();renderEnts();$('q').focus();}catch(e){}}
function closePicker(){$('picker').classList.remove('show');}
function pick(id){const p=pages()[cur];const d=domainOf(id);const type=TOGGLE.includes(d)?'toggle':'action';
  if(pickFor<0){p.tiles.push({id:'t'+Date.now().toString(36),type:type,entity:id});}
  else{const t=p.tiles[pickFor];t.entity=id;t.type=type;delete t.label;delete t.icon;delete t.tap;}
  setDirty(true);closePicker();renderPage();}
$('q').oninput=renderEnts;$('pickClose').onclick=closePicker;$('picker').onclick=e=>{if(e.target===$('picker'))closePicker();};

/* ---- save, preview ---- */
$('saveDeck').onclick=async()=>{try{await api('/api/deck',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(deck)});setDirty(false);$('srcTag').textContent='saved on device';setTimeout(prev,900);}catch(e){alert('Not saved: '+e.message);}};
function prev(){$('prev').src='/screen_live.bmp?t='+Date.now();}
$('prevBtn').onclick=prev;
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});

(async function init(){haStatus();setInterval(haStatus,3000);try{const r=await api('/api/deck');deck=r.deck;$('srcTag').textContent=r.source;render();}catch(e){$('pageEdit').innerHTML='<p class="empty">'+esc(e.message)+'</p>';}prev();
  try{await loadEnts();}catch(e){}})();
})();
</script>
</body>
</html>)HTML";

#endif  // BOARD_IS_JC3248W535
#endif  // DECK_WEB_H
