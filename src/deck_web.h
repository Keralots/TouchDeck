// Guition deck: the Deck section of the portal, served as /deck.js. The
// shared portal page carries an empty "deck" section and a hidden nav item;
// on a board whose /api/config says "deck": true the portal loads this script,
// which fills the section. The other boards never download or store it.
// IDs and classes are prefixed (dk) so nothing collides with the portal.
#ifndef DECK_WEB_H
#define DECK_WEB_H

#if defined(BOARD_IS_JC3248W535)

#include <Arduino.h>

static const char DECK_JS[] PROGMEM = R"JS((function(){'use strict';
const CSS=`
.dk-row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.dk-status{display:flex;align-items:center;gap:8px;font:13px var(--mono);color:var(--mute);margin-top:12px}
.dk-dot{width:9px;height:9px;border-radius:50%;background:var(--faint)}.dk-dot.ok{background:var(--ok)}.dk-dot.err{background:var(--err)}.dk-dot.busy{background:var(--warn)}
.dk-tabs{display:flex;gap:7px;flex-wrap:wrap;margin-bottom:14px}
.dk-tiles{display:flex;flex-direction:column;gap:8px}
.dk-tile{display:grid;grid-template-columns:36px minmax(0,1fr) auto;gap:12px;align-items:center;padding:10px 12px;border:1px solid var(--line);border-radius:var(--r-md);background:var(--card-2)}
.dk-ic{width:36px;height:36px;border-radius:50%;display:grid;place-items:center;font:600 12px var(--mono);color:#111}
.dk-tmain{min-width:0}.dk-tmain input{max-width:none;padding:7px 10px;font-size:14px}
.dk-tsub{font:11px var(--mono);color:var(--dim);margin-top:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dk-tact{display:flex;gap:5px;align-items:center;flex-wrap:wrap;justify-content:flex-end}.dk-tact select{width:auto;padding:6px 8px;font-size:12px}.dk-tact .btn{padding:6px 9px}
.dk-cap{font:12px var(--mono);color:var(--dim);margin:10px 0 0}.dk-cap.over{color:var(--err)}
.dk-save{position:sticky;bottom:0;display:flex;align-items:center;gap:12px;margin:18px -24px -22px;padding:12px 24px;background:var(--card);border-top:1px solid var(--line);border-radius:0 0 var(--r-lg) var(--r-lg)}
.dk-save .dk-msg{margin-right:auto;font:12px var(--mono);color:var(--dim)}
.dk-modal{position:fixed;inset:0;background:rgba(0,0,0,.55);display:none;align-items:flex-start;justify-content:center;z-index:90;padding:5vh 12px}
.dk-modal.show{display:flex}
.dk-sheet{background:var(--card);border:1px solid var(--line);border-radius:var(--r-lg);width:min(640px,100%);max-height:88vh;display:flex;flex-direction:column;box-shadow:0 20px 50px rgba(0,0,0,.4)}
.dk-head{padding:16px 18px;border-bottom:1px solid var(--line)}.dk-head input{max-width:none}
.dk-chips{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}.dk-chips .chip{cursor:pointer}.dk-chips .chip.on{border-color:var(--accent);background:var(--accent-soft);color:var(--ink)}
.dk-list{overflow-y:auto;padding:6px}
.dk-ent{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;padding:9px 12px;border-radius:var(--r-md);cursor:pointer}.dk-ent:hover{background:var(--card-2)}
.dk-ent b{display:block;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.dk-ent small{font:11px var(--mono);color:var(--dim)}.dk-ent .st{font:12px var(--mono);color:var(--mute);align-self:center}
.dk-empty{padding:22px;text-align:center;color:var(--dim);font-size:13px}
.dk-ic{cursor:pointer;border:0;padding:0}.dk-ic.mdi{font:22px dkmdi;line-height:1}
.dk-igrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(86px,1fr));gap:6px;overflow-y:auto;padding:10px}
.dk-ibtn{display:flex;flex-direction:column;align-items:center;gap:6px;padding:10px 4px;border:1px solid transparent;border-radius:var(--r-md);background:none;color:var(--ink);cursor:pointer;min-width:0}
.dk-ibtn:hover{background:var(--card-2)}.dk-ibtn.on{border-color:var(--accent);background:var(--accent-soft)}
.dk-ibtn i{font:28px dkmdi;font-style:normal;line-height:1}.dk-ibtn small{font:10px var(--mono);color:var(--dim);max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
@media(max-width:560px){.dk-tile{grid-template-columns:36px minmax(0,1fr)}.dk-tact{grid-column:1/-1}.dk-save{margin:18px -16px -18px;padding:12px 16px}}
`;
const st=document.createElement('style');st.textContent=CSS;document.head.appendChild(st);
const sec=document.getElementById('deckSection');if(!sec)return;
sec.innerHTML=`<div class="page-header"><h1 class="page-h1">Deck</h1><p class="page-lede">Home Assistant connection and the pages and tiles of the touch deck. Changes apply on Save without a restart.</p></div>
<div class="card"><div class="card-head"><h2 class="card-title">Home Assistant</h2><span class="tag" id="dkHaVer">-</span></div>
<div class="grid-2"><div class="field"><label class="field-label" for="dkHaUrl">Address</label><input type="text" id="dkHaUrl" placeholder="http://192.168.0.7:8123" autocomplete="off"></div>
<div class="field"><label class="field-label" for="dkHaToken">Long-lived access token</label><input type="password" id="dkHaToken" autocomplete="off" placeholder="paste the token"><p class="field-hint">HA profile, Security tab, Long-lived access tokens. Stored only on the device, never shown again.</p></div></div>
<div class="dk-row"><button type="button" class="btn btn-accent" id="dkHaSave">Save and connect</button><button type="button" class="btn" id="dkHaForget">Forget</button></div>
<div class="dk-status"><span class="dk-dot" id="dkHaDot"></span><span id="dkHaState">checking...</span></div></div>
<div class="card"><div class="card-head"><h2 class="card-title">Icons</h2><span class="tag" id="dkIcTag">-</span></div>
<p class="field-hint">Material Design Icons, the set Home Assistant uses (mdi:...). The font file (about 1.3 MB) is stored on the device; "Install" downloads it in this browser and sends it to the device. Without it the deck shows a few built-in symbols.</p>
<div class="dk-row" style="margin-top:10px"><button type="button" class="btn btn-accent" id="dkIcNet">Install MDI 7.4.47</button><button type="button" class="btn" id="dkIcFile">Upload .ttf file</button><input type="file" id="dkIcInput" accept=".ttf" hidden></div>
<div class="dk-status"><span class="dk-dot" id="dkIcDot"></span><span id="dkIcState">checking...</span></div></div>
<div class="card"><div class="card-head"><h2 class="card-title">Pages and tiles</h2></div>
<label class="check-row standalone" style="margin-bottom:14px"><input type="checkbox" id="dkHeader"><span class="check-box"></span><span class="check-text"><strong>Show the top bar</strong><span class="ct-hint">Page name, WiFi and clock above the tiles. Also switchable in the quick panel on the device.</span></span></label>
<div class="dk-tabs" id="dkTabs"></div><div id="dkPage"></div>
<div class="dk-save"><span class="dk-msg" id="dkMsg">No changes</span><button type="button" class="btn btn-accent" id="dkSave">Save to device</button></div></div>`;
document.body.insertAdjacentHTML('beforeend',`<div class="dk-modal" id="dkPicker"><div class="dk-sheet"><div class="dk-head">
<div class="dk-row"><input type="text" id="dkQ" placeholder="Search by name or entity id" autocomplete="off" style="flex:1"><button type="button" class="btn" id="dkPickClose">Close</button></div>
<div class="dk-chips" id="dkDomains"></div>
<div class="dk-status"><span id="dkListInfo">-</span><button type="button" class="btn" id="dkListUpdate" style="margin-left:auto;padding:5px 10px">Update from HA</button></div></div>
<div class="dk-list" id="dkEnts"></div></div></div>
<div class="dk-modal" id="dkIcPick"><div class="dk-sheet"><div class="dk-head">
<div class="dk-row"><input type="text" id="dkIcQ" placeholder="Search icons, e.g. lamp, fan, thermometer" autocomplete="off" style="flex:1"><button type="button" class="btn" id="dkIcAuto">Automatic</button><button type="button" class="btn" id="dkIcClose">Close</button></div>
<div class="dk-status"><span id="dkIcInfo">-</span></div></div>
<div class="dk-igrid" id="dkIcGrid"></div></div></div>`);

const $=id=>document.getElementById(id);
const TOGGLE=['light','switch','fan','input_boolean','lock','cover','media_player','climate','automation','siren','humidifier','valve'];
const SENSOR=['sensor','binary_sensor','weather','sun','person','device_tracker','zone'];
const autoType=d=>TOGGLE.includes(d)?'toggle':SENSOR.includes(d)?'sensor':'action';
const DOMS=[['','All'],['light','Lights'],['switch','Switches'],['fan','Fans'],['scene','Scenes'],['script','Scripts'],['lock','Locks'],['cover','Covers'],['media_player','Media'],['climate','Climate'],['button','Buttons'],['input_boolean','Helpers'],['automation','Automations'],['sensor','Sensors'],['binary_sensor','Binary sensors']];
const COL={light:'#F6C56B',switch:'#5CC9A7',fan:'#5CC9A7',input_boolean:'#5CC9A7',scene:'#B3A6FF',script:'#B3A6FF',lock:'#9DB8FF',cover:'#9DB8FF'};
let deck=null,cur=0,ents=null,entMap={},dirty=false,pickFor=-1,dom='',pcm=[];
const PAL=['#3987E5','#00A35C','#D55181','#C98500','#199E70','#D95926','#9085E9','#E66767'];
async function loadPc(){try{const c=await api('/api/config');pcm=(c.metrics||[]).slice().sort((a,b)=>a.id-b.id);}catch(e){pcm=[];}}
function metricRow(t,i){const m=pcm.find(x=>x.id===t.metric);const col=t.color||PAL[((t.metric||1)-1)%8];
  const opts=pcm.map(x=>'<option value="'+x.id+'"'+(x.id===t.metric?' selected':'')+'>'+esc(x.name)+' ('+esc(Math.round(x.value*10)/10)+' '+esc(x.unit)+')</option>').join('')+(m?'':'<option value="'+t.metric+'" selected>#'+t.metric+' (not in the last packet)</option>');
  return '<div class="dk-tile"><div class="dk-ic" style="background:'+col+'">PC</div><div class="dk-tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(m?m.name:'Metric')+'"><div class="dk-tsub">PC metric #'+t.metric+(m?' - '+esc(Math.round(m.value*10)/10)+' '+esc(m.unit):'')+'</div></div>'+
  '<div class="dk-tact"><select data-m="'+i+'">'+opts+'</select><select data-st="'+i+'">'+[['number','Number'],['chart','Chart'],['bar','Bar']].map(o=>'<option value="'+o[0]+'"'+((t.style||'number')===o[0]?' selected':'')+'>'+o[1]+'</option>').join('')+'</select><select data-s="'+i+'">'+['1x1','2x1','1x2','2x2'].map(v=>'<option'+(v===tileSize(t)?' selected':'')+'>'+v+'</option>').join('')+'</select><button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div></div>';}
async function api(p,o){const r=await fetch(p,o);const t=await r.text();let j=null;try{j=JSON.parse(t);}catch(e){}if(!r.ok)throw new Error((j&&(j.message||j.error))||t||('HTTP '+r.status));return j;}
const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const domainOf=e=>{const i=(e||'').indexOf('.');return i>0?e.slice(0,i):'';};
function setDirty(v){dirty=v;$('dkMsg').textContent=v?'Unsaved changes':'Saved';}
const pages=()=>deck.pages.filter(p=>p.type!=='metrics');
const visible=()=>document.querySelector('.page.active')?.dataset.page==='deck';

/* Home Assistant */
async function haStatus(){try{const s=await api('/api/ha/status');if(document.activeElement!==$('dkHaUrl'))$('dkHaUrl').value=s.url||'';$('dkHaToken').placeholder=s.has_token?'saved - leave empty to keep':'paste the token';const l=s.link;$('dkHaDot').className='dk-dot '+(l==='connected'?'ok':(l==='token rejected'||l==='unreachable')?'err':(l==='not set up'?'':'busy'));$('dkHaState').textContent=l+(s.error?' - '+s.error:'');$('dkHaVer').textContent=s.version?('HA '+s.version):'-';}catch(e){$('dkHaState').textContent='status unavailable';}}
$('dkHaSave').onclick=async()=>{const b=new URLSearchParams();b.set('url',$('dkHaUrl').value.trim());b.set('token',$('dkHaToken').value.trim());try{await api('/api/ha/config',{method:'POST',body:b});$('dkHaToken').value='';haStatus();}catch(e){alert(e.message);}};
$('dkHaForget').onclick=async()=>{if(!confirm('Remove the Home Assistant address and token from the device?'))return;try{await api('/api/ha/forget',{method:'POST'});haStatus();}catch(e){alert(e.message);}};

/* pages and tiles */
const tileSize=t=>{const p=t.pos&&t.pos.landscape;return p?((p.w||1)+'x'+(p.h||1)):'1x1';};
function setSize(t,v){const [w,h]=v.split('x').map(Number);if(w===1&&h===1){delete t.pos;return;}t.pos={landscape:{w:w,h:h}};}
const grid=p=>{const l=(p.layouts&&p.layouts.landscape)||{};return [l.cols||4,l.rows||3];};
function renderTabs(){const ps=pages();if(cur>=ps.length)cur=Math.max(0,ps.length-1);$('dkTabs').innerHTML=ps.map((p,i)=>'<button type="button" class="chip page-tab'+(i===cur?' on':'')+'" data-i="'+i+'">'+esc(p.name||p.id)+'</button>').join('')+'<button type="button" class="chip page-tab" id="dkAddPage">+ Page</button>';
  $('dkTabs').querySelectorAll('[data-i]').forEach(b=>b.onclick=()=>{cur=+b.dataset.i;render();});$('dkAddPage').onclick=()=>{deck.pages.push({id:'p'+Date.now().toString(36),name:'New page',layouts:{landscape:{cols:4,rows:3}},tiles:[]});cur=pages().length-1;setDirty(true);render();};}
function renderPage(){const ps=pages();const el=$('dkPage');if(!ps.length){el.innerHTML='<p class="dk-empty">No deck pages. Add one with "+ Page".</p>';return;}
  const p=ps[cur];const [c,r]=grid(p);let used=0;p.tiles.forEach(t=>{const [w,h]=tileSize(t).split('x').map(Number);used+=w*h;});
  let h='<div class="grid-2"><div class="field"><label class="field-label">Page name</label><input type="text" id="dkPName" value="'+esc(p.name||'')+'"></div><div class="field"><label class="field-label">Grid (columns x rows)</label><div class="dk-row"><select id="dkPCols" style="width:auto">'+[2,3,4,5,6].map(n=>'<option'+(n===c?' selected':'')+'>'+n+'</option>').join('')+'</select><span>x</span><select id="dkPRows" style="width:auto">'+[2,3,4].map(n=>'<option'+(n===r?' selected':'')+'>'+n+'</option>').join('')+'</select><button type="button" class="btn btn-danger" id="dkPDel" style="margin-left:auto">Delete page</button></div></div></div>';
  h+='<div class="dk-tiles">'+p.tiles.map((t,i)=>{if(t.type==='metric')return metricRow(t,i);const d=domainOf(t.entity);const en=entMap[t.entity];const nm=en&&en.name?en.name:(t.entity||'No entity');
    const cp=tileCp(t);
    return '<div class="dk-tile"><button type="button" class="dk-ic'+(cp?' mdi':'')+'" data-ic="'+i+'" title="'+esc(cp?(t.icon||(en&&en.icon)||'automatic'):'Choose an icon')+'" style="background:'+(t.color||COL[d]||'#7DB5FF')+'">'+(cp?String.fromCodePoint(cp):esc((d||'?').slice(0,2).toUpperCase()))+'</button><div class="dk-tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(nm)+'"><div class="dk-tsub">'+esc(t.entity||'no entity')+(en&&en.state?' - '+esc(en.state)+(en.unit?' '+esc(en.unit):''):'')+'</div></div>'+
    '<div class="dk-tact"><select data-t="'+i+'"><option value="toggle"'+(t.type==='toggle'?' selected':'')+'>Switch</option><option value="action"'+(t.type==='action'?' selected':'')+'>Button</option><option value="sensor"'+(t.type==='sensor'?' selected':'')+'>Sensor</option></select><select data-s="'+i+'">'+['1x1','2x1','1x2','2x2'].map(v=>'<option'+(v===tileSize(t)?' selected':'')+'>'+v+'</option>').join('')+'</select><button type="button" class="btn" data-e="'+i+'">Entity</button><button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div></div>';}).join('')+'</div>';
  h+='<p class="dk-cap'+(used>c*r?' over':'')+'">'+used+' of '+(c*r)+' cells used'+(used>c*r?' - tiles that do not fit are not shown':'')+'</p><div class="dk-row" style="margin-top:12px"><button type="button" class="btn btn-accent" id="dkAddTile">+ Add tile from Home Assistant</button><button type="button" class="btn" id="dkAddPc">+ Add PC metric</button></div><p class="field-hint">Chart and Bar need a 2x1 or bigger tile; a 1x1 tile shows the number.</p>';
  el.innerHTML=h;
  $('dkPName').oninput=e=>{p.name=e.target.value;setDirty(true);renderTabs();};
  $('dkPCols').onchange=e=>{p.layouts=p.layouts||{};p.layouts.landscape={cols:+e.target.value,rows:grid(p)[1]};setDirty(true);renderPage();};
  $('dkPRows').onchange=e=>{p.layouts=p.layouts||{};p.layouts.landscape={cols:grid(p)[0],rows:+e.target.value};setDirty(true);renderPage();};
  $('dkPDel').onclick=()=>{if(!confirm('Delete the page "'+(p.name||p.id)+'"?'))return;deck.pages.splice(deck.pages.indexOf(p),1);cur=0;setDirty(true);render();};
  el.querySelectorAll('[data-l]').forEach(x=>x.oninput=e=>{const t=p.tiles[+x.dataset.l];const v=e.target.value.trim();if(v)t.label=v;else delete t.label;setDirty(true);});
  el.querySelectorAll('[data-t]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.t];t.type=e.target.value;if(t.type!=='action')delete t.tap;setDirty(true);renderPage();});
  el.querySelectorAll('[data-s]').forEach(x=>x.onchange=e=>{setSize(p.tiles[+x.dataset.s],e.target.value);setDirty(true);renderPage();});
  el.querySelectorAll('[data-e]').forEach(x=>x.onclick=()=>openPicker(+x.dataset.e));
  el.querySelectorAll('[data-ic]').forEach(x=>x.onclick=()=>openIcons(+x.dataset.ic));
  el.querySelectorAll('[data-u]').forEach(x=>x.onclick=()=>{const i=+x.dataset.u;if(i<1)return;const t=p.tiles.splice(i,1)[0];p.tiles.splice(i-1,0,t);setDirty(true);renderPage();});
  el.querySelectorAll('[data-x]').forEach(x=>x.onclick=()=>{p.tiles.splice(+x.dataset.x,1);setDirty(true);renderPage();});
  $('dkAddTile').onclick=()=>openPicker(-1);
  $('dkAddPc').onclick=async()=>{if(!pcm.length)await loadPc();const usedIds=p.tiles.filter(t=>t.type==='metric').map(t=>t.metric);const m=pcm.find(x=>!usedIds.includes(x.id))||pcm[0];if(!m){alert('No PC metrics yet - start the companion app on the PC.');return;}p.tiles.push({id:'m'+Date.now().toString(36),type:'metric',metric:m.id,style:'number'});setDirty(true);renderPage();};
  el.querySelectorAll('[data-m]').forEach(x=>x.onchange=e=>{p.tiles[+x.dataset.m].metric=+e.target.value;setDirty(true);renderPage();});
  el.querySelectorAll('[data-st]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.st];if(e.target.value==='number')delete t.style;else t.style=e.target.value;setDirty(true);});}
function render(){renderTabs();renderPage();}

/* entity picker */
function setList(r){ents=(r.entities||[]).slice().sort((a,b)=>a.id<b.id?-1:a.id>b.id?1:0);entMap={};ents.forEach(e=>entMap[e.id]=e);
  $('dkListInfo').textContent=r.updated?(ents.length+' entities, list from '+new Date(r.updated*1000).toLocaleString()):'No list yet - press "Update from HA"';}
async function loadEnts(refresh){if(ents&&!refresh)return;$('dkEnts').innerHTML='<p class="dk-empty">'+(refresh?'Fetching the entity list from Home Assistant...':'Loading...')+'</p>';$('dkListUpdate').disabled=true;
  try{setList(await api('/api/ha/entities'+(refresh?'?refresh=1':'')));if(deck)renderPage();}catch(e){$('dkEnts').innerHTML='<p class="dk-empty">'+esc(e.message)+'<br>Check the Home Assistant settings above.</p>';throw e;}finally{$('dkListUpdate').disabled=false;}}
$('dkListUpdate').onclick=async()=>{try{await loadEnts(true);renderEnts();}catch(e){}};
function renderDomains(){$('dkDomains').innerHTML=DOMS.map(d=>'<span class="chip'+(d[0]===dom?' on':'')+'" data-d="'+d[0]+'">'+d[1]+'</span>').join('');$('dkDomains').querySelectorAll('[data-d]').forEach(c=>c.onclick=()=>{dom=c.dataset.d;renderDomains();renderEnts();});}
function renderEnts(){if(!ents)return;if(!ents.length){$('dkEnts').innerHTML='<p class="dk-empty">No entity list on the device yet. Press "Update from HA".</p>';return;}const q=$('dkQ').value.trim().toLowerCase();let list=ents.filter(e=>(!dom||domainOf(e.id)===dom)&&(!q||e.id.toLowerCase().includes(q)||(e.name||'').toLowerCase().includes(q)));
  if(!dom&&!q){const pri=e=>{const d=domainOf(e.id);return TOGGLE.includes(d)||['scene','script','button','input_button'].includes(d)?0:(d==='sensor'?1:2);};list=list.slice().sort((a,b)=>pri(a)-pri(b));}
  const shown=list.slice(0,250);$('dkEnts').innerHTML=shown.length?shown.map(e=>'<div class="dk-ent" data-id="'+esc(e.id)+'"><div><b>'+esc(e.name||e.id)+'</b><small>'+esc(e.id)+'</small></div><span class="st">'+esc(e.state)+(e.unit?' '+esc(e.unit):'')+'</span></div>').join('')+(list.length>shown.length?'<p class="dk-empty">'+(list.length-shown.length)+' more - refine the search</p>':''):'<p class="dk-empty">Nothing matches.</p>';
  $('dkEnts').querySelectorAll('[data-id]').forEach(r=>r.onclick=()=>pick(r.dataset.id));}
async function openPicker(i){pickFor=i;$('dkQ').value='';$('dkPicker').classList.add('show');renderDomains();try{await loadEnts();renderEnts();$('dkQ').focus();}catch(e){}}
function closePicker(){$('dkPicker').classList.remove('show');}
function pick(id){const p=pages()[cur];const type=autoType(domainOf(id));
  if(pickFor<0)p.tiles.push({id:'t'+Date.now().toString(36),type:type,entity:id});
  else{const t=p.tiles[pickFor];t.entity=id;t.type=type;delete t.label;delete t.sub;delete t.icon;delete t.tap;}
  setDirty(true);closePicker();renderPage();}
$('dkQ').oninput=renderEnts;$('dkPickClose').onclick=closePicker;$('dkPicker').onclick=e=>{if(e.target===$('dkPicker'))closePicker();};

/* icons */
const MDI_URL='https://cdn.jsdelivr.net/npm/@mdi/font@7.4.47/fonts/materialdesignicons-webfont.ttf';
let mdi=null,mdiNames=[],icFor=-1;
// name -> codepoint from the font's own glyph names (post v2 + cmap format 12).
function parseMdi(buf){const d=new DataView(buf),u=new Uint8Array(buf),T={};const n=d.getUint16(4);
  for(let i=0;i<n;i++){const r=12+16*i;T[String.fromCharCode(u[r],u[r+1],u[r+2],u[r+3])]=[d.getUint32(r+8),d.getUint32(r+12)];}
  const [po,pl]=T.post,co=T.cmap[0],g=d.getUint16(po+32),cu=[];for(let p=po+34+2*g;p<po+pl;p+=1+u[p])cu.push(String.fromCharCode.apply(null,u.subarray(p+1,p+1+u[p])));
  const nm=gi=>{if(gi>=g)return '';const k=d.getUint16(po+34+2*gi);return k>=258?(cu[k-258]||''):'';};const m=new Map();
  for(let i=0,ns=d.getUint16(co+2);i<ns;i++){const so=co+d.getUint32(co+8+8*i);if(d.getUint16(so)!==12)continue;
    for(let k=0,ng=d.getUint32(so+12);k<ng;k++){const b=so+16+12*k,s0=d.getUint32(b),e0=d.getUint32(b+4),g0=d.getUint32(b+8);for(let c=s0;c<=e0;c++){const x=nm(g0+c-s0);if(x&&!m.has(x))m.set(x,c);}}break;}
  return m;}
const iconCp=n=>{if(!mdi||!n)return 0;if(n.startsWith('mdi:'))n=n.slice(4);else if(n.includes(':'))return 0;return mdi.get(n)||0;};
const tileCp=t=>iconCp(t.icon)||iconCp((entMap[t.entity]||{}).icon);
async function loadMdi(size){try{const r=await fetch('/mdi.ttf?v='+size);if(!r.ok)throw new Error('HTTP '+r.status);const buf=await r.arrayBuffer();mdi=parseMdi(buf);mdiNames=[...mdi.keys()].sort();
  const f=new FontFace('dkmdi',buf);await f.load();document.fonts.add(f);if(deck)renderPage();}catch(e){mdi=null;$('dkIcState').textContent='font on the device, but the portal could not read it: '+e.message;}}
function icStatus(s){$('dkIcDot').className='dk-dot '+(s.loaded?'ok':'');$('dkIcState').textContent=s.loaded?(s.count+' icons, '+Math.round(s.size/1024)+' KB'):'not installed';$('dkIcTag').textContent=s.loaded?'installed':'-';}
async function icRefresh(){try{const s=await api('/api/deck/icons');icStatus(s);if(s.loaded&&!mdi)loadMdi(s.size);}catch(e){$('dkIcState').textContent='status unavailable';}}
async function icInstall(blob){$('dkIcDot').className='dk-dot busy';$('dkIcState').textContent='Sending to the device and installing (about 20 s)...';$('dkIcNet').disabled=$('dkIcFile').disabled=true;
  try{const fd=new FormData();fd.append('font',blob,'mdi.ttf');const s=await api('/api/deck/icons',{method:'POST',body:fd});icStatus(s);mdi=null;if(s.loaded)await loadMdi(s.size);}
  catch(e){$('dkIcDot').className='dk-dot err';$('dkIcState').textContent='Not installed: '+e.message;}finally{$('dkIcNet').disabled=$('dkIcFile').disabled=false;}}
$('dkIcNet').onclick=async()=>{$('dkIcState').textContent='Downloading from jsDelivr...';try{const r=await fetch(MDI_URL);if(!r.ok)throw new Error('HTTP '+r.status);await icInstall(await r.blob());}catch(e){$('dkIcDot').className='dk-dot err';$('dkIcState').textContent='Download failed ('+e.message+'). Get the file yourself and use "Upload .ttf file".';}};
$('dkIcFile').onclick=()=>$('dkIcInput').click();
$('dkIcInput').onchange=e=>{const f=e.target.files[0];e.target.value='';if(f)icInstall(f);};
function renderIcons(){const t=pages()[cur].tiles[icFor];const q=$('dkIcQ').value.trim().toLowerCase().replace(/^mdi:/,'');const cur0=(t.icon||'').replace(/^mdi:/,'');
  const list=q?mdiNames.filter(n=>n.includes(q)):mdiNames;const shown=list.slice(0,240);
  $('dkIcInfo').textContent=(t.icon?'Chosen: '+t.icon:'Automatic (the entity icon in HA, else a default)')+' - '+list.length+' match'+(list.length===1?'':'es')+(list.length>shown.length?', showing '+shown.length:'');
  $('dkIcGrid').innerHTML=shown.map(n=>'<button type="button" class="dk-ibtn'+(n===cur0?' on':'')+'" data-n="'+esc(n)+'" title="mdi:'+esc(n)+'"><i>'+String.fromCodePoint(mdi.get(n))+'</i><small>'+esc(n)+'</small></button>').join('')||'<p class="dk-empty">Nothing matches.</p>';
  $('dkIcGrid').querySelectorAll('[data-n]').forEach(b=>b.onclick=()=>setIcon('mdi:'+b.dataset.n));}
function setIcon(v){const t=pages()[cur].tiles[icFor];if(v)t.icon=v;else delete t.icon;setDirty(true);$('dkIcPick').classList.remove('show');renderPage();}
function openIcons(i){if(!mdi){alert('Install the icon font first (Icons card above).');return;}icFor=i;$('dkIcQ').value='';$('dkIcPick').classList.add('show');renderIcons();$('dkIcQ').focus();}
$('dkIcQ').oninput=renderIcons;$('dkIcAuto').onclick=()=>setIcon('');$('dkIcClose').onclick=()=>$('dkIcPick').classList.remove('show');
$('dkIcPick').onclick=e=>{if(e.target===$('dkIcPick'))$('dkIcPick').classList.remove('show');};

$('dkHeader').onchange=async e=>{const b=new URLSearchParams();b.set('header',e.target.checked?'1':'0');try{await api('/api/deck/prefs',{method:'POST',body:b});}catch(x){alert(x.message);}};

/* save */
$('dkSave').onclick=async()=>{try{await api('/api/deck',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(deck)});setDirty(false);}catch(e){alert('Not saved: '+e.message);}};
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});

(async function init(){haStatus();icRefresh();setInterval(()=>{if(visible())haStatus();},3000);
  await loadPc();
  try{const r=await api('/api/deck');deck=r.deck;$('dkHeader').checked=r.header!==false;render();}catch(e){$('dkPage').innerHTML='<p class="dk-empty">'+esc(e.message)+'</p>';}
  try{await loadEnts();}catch(e){}})();
})();)JS";

#endif  // BOARD_IS_JC3248W535
#endif  // DECK_WEB_H
