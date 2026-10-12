// Guition deck: the Deck section of the portal, served as /deck.js. The
// shared portal page carries an empty "deck" section and a hidden nav item;
// on a board whose /api/config says "deck": true the portal loads this script,
// which fills the section. The other boards never download or store it.
// IDs and classes are prefixed (dk) so nothing collides with the portal.
#ifndef DECK_WEB_H
#define DECK_WEB_H

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
.dk-tile.sel{border-color:var(--accent)}
.dk-more{grid-column:1/-1;display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;padding-top:10px;border-top:1px solid var(--line)}
.dk-more .field{margin:0}.dk-more .wide{grid-column:1/-1}.dk-more select,.dk-more input,.dk-more textarea{max-width:none;padding:7px 10px;font-size:13px}
.dk-more textarea{width:100%;min-height:58px;font:12px var(--mono);background:var(--card);color:var(--ink);border:1px solid var(--line-2);border-radius:var(--r-md);resize:vertical}
.dk-grid{display:grid;gap:6px;padding:6px;width:100%;max-width:560px;margin:12px 0 6px;background:var(--card-2);border:1px solid var(--line);border-radius:var(--r-md);user-select:none}.dk-grid.pt{max-width:300px}
.dk-cell{border:1px dashed var(--line);border-radius:8px}
.dk-gt{position:relative;display:flex;flex-direction:column;justify-content:space-between;gap:4px;min-width:0;min-height:0;overflow:hidden;padding:7px 8px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--ink);cursor:grab;touch-action:none}
.dk-gt.sel{outline:2px solid var(--accent);outline-offset:1px}.dk-gt.drag{z-index:5;opacity:.85;cursor:grabbing;box-shadow:0 8px 20px rgba(0,0,0,.35)}
.dk-gt i{font:20px dkmdi;font-style:normal;line-height:1}.dk-gt i.tx{font:600 11px var(--mono)}
.dk-gt b{font-size:12px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dk-rs{position:absolute;right:0;bottom:0;width:18px;height:18px;cursor:nwse-resize;touch-action:none;background:linear-gradient(135deg,transparent 55%,var(--mute) 55%);opacity:.7}
.dk-ghost{z-index:4;border:2px solid var(--ok);border-radius:8px;background:rgba(92,201,167,.15);pointer-events:none}.dk-ghost.bad{border-color:var(--err);background:rgba(229,83,75,.15)}
.dk-tray{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0}.dk-tray .dk-gt{width:120px;height:60px}
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
<div class="dk-status"><span class="dk-dot" id="dkHaDot"></span><span id="dkHaState">checking...</span></div>
<div class="field" style="margin-top:16px"><label class="field-label">Status LED</label><div class="dk-row"><span id="dkLedEnt" class="dk-tsub" style="margin:0">-</span><button type="button" class="btn" id="dkLedPick">Choose entity</button><button type="button" class="btn" id="dkLedClear">Unlink</button></div><p class="field-hint">The LED's day brightness follows this input_boolean, input_number or light, and the Light slider on the device sets it. The LED itself and its night level: Hardware page. Applies on "Save to device" below.</p></div></div>
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
/* Domains a slider tile can drive (brightness, position, speed, volume, value). */
const SLIDER=['light','cover','fan','media_player','input_number','number'];
const autoType=d=>TOGGLE.includes(d)?'toggle':SENSOR.includes(d)?'sensor':'action';
const DOMS=[['','All'],['light','Lights'],['switch','Switches'],['fan','Fans'],['scene','Scenes'],['script','Scripts'],['lock','Locks'],['cover','Covers'],['media_player','Media'],['climate','Climate'],['button','Buttons'],['input_boolean','Helpers'],['automation','Automations'],['sensor','Sensors'],['binary_sensor','Binary sensors']];
const COL={light:'#F6C56B',switch:'#5CC9A7',fan:'#5CC9A7',input_boolean:'#5CC9A7',scene:'#B3A6FF',script:'#B3A6FF',lock:'#9DB8FF',cover:'#9DB8FF'};
let deck=null,cur=0,ents=null,entMap={},dirty=false,pickFor=-1,dom='',pcm=[],ori=null,sel=-1,more=-1;
const PAL=['#3987E5','#00A35C','#D55181','#C98500','#199E70','#D95926','#9085E9','#E66767'];
async function loadPc(){try{const c=await api('/api/config');pcm=(c.metrics||[]).slice().sort((a,b)=>a.id-b.id);if(!ori&&c.display)ori=c.display.rotation%2?'landscape':'portrait';}catch(e){pcm=[];}}
function metricRow(t,i){const m=pcm.find(x=>x.id===t.metric);const col=t.color||PAL[((t.metric||1)-1)%8];
  const opts=pcm.map(x=>'<option value="'+x.id+'"'+(x.id===t.metric?' selected':'')+'>'+esc(x.name)+' ('+esc(Math.round(x.value*10)/10)+' '+esc(x.unit)+')</option>').join('')+(m?'':'<option value="'+t.metric+'" selected>#'+t.metric+' (not in the last packet)</option>');
  return '<div class="dk-tile'+(i===sel?' sel':'')+'"><div class="dk-ic" style="background:'+col+'">PC</div><div class="dk-tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(m?m.name:'Metric')+'"><div class="dk-tsub">PC metric #'+t.metric+(m?' - '+esc(Math.round(m.value*10)/10)+' '+esc(m.unit):'')+'</div></div>'+
  '<div class="dk-tact"><select data-m="'+i+'">'+opts+'</select><select data-st="'+i+'">'+[['number','Number'],['chart','Chart'],['bar','Bar']].map(o=>'<option value="'+o[0]+'"'+((t.style||'number')===o[0]?' selected':'')+'>'+o[1]+'</option>').join('')+'</select>'+sizeSel(t,i)+'<button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div></div>';}
/* A tile's "more options" panel: confirm, and for buttons the tap action. */
const opt=(v,l,on)=>'<option value="'+esc(v)+'"'+(on?' selected':'')+'>'+esc(l)+'</option>';
const isHttp=t=>!!(t.tap&&t.tap.action==='http');
const tapName=a=>a.action==='page'?'Open page '+a.page:a.action==='call-service'?'Service '+a.domain+'.'+a.service:a.action;
const hdrText=h=>Object.keys(h||{}).map(k=>k+': '+h[k]).join('\n');
function moreRow(t,i){const cf=t.confirm||'';
  let h='<div class="dk-more"><div class="field"><label class="field-label">Confirm on the device</label><select data-cf="'+i+'">'+[['','No'],['off','Before turning off'],['always','Before every action']].map(o=>opt(o[0],o[1],cf===o[0])).join('')+'</select></div>';
  if(t.type==='action'){const tp=t.tap||{},k=isHttp(t)?'http':(t.tap?'keep':'');
    h+='<div class="field"><label class="field-label">On tap</label><select data-ak="'+i+'">'+opt('','Home Assistant entity',k==='')+opt('http','HTTP request',k==='http')+(k==='keep'?opt('keep',tapName(tp),true):'')+'</select></div>';
    if(k==='http'){const body=typeof tp.body==='object'&&tp.body!==null?JSON.stringify(tp.body):(tp.body||'');
      h+='<div class="field"><label class="field-label">Method</label><select data-hm="'+i+'">'+['GET','POST','PUT','PATCH','DELETE'].map(m=>opt(m,m,(tp.method||'GET')===m)).join('')+'</select></div>'+
      '<div class="field wide"><label class="field-label">URL</label><input type="text" data-hu="'+i+'" value="'+esc(tp.url||'')+'" placeholder="http://192.168.0.50/cm?cmnd=Power%20Toggle" spellcheck="false"></div>'+
      '<div class="field"><label class="field-label">Body (optional)</label><textarea data-hb="'+i+'" spellcheck="false">'+esc(body)+'</textarea></div>'+
      '<div class="field"><label class="field-label">Headers (Name: value)</label><textarea data-hh="'+i+'" spellcheck="false" placeholder="Authorization: Bearer ...">'+esc(hdrText(tp.headers))+'</textarea></div>'+
      '<div class="field"><label class="field-label">Content type</label><input type="text" data-hc="'+i+'" value="'+esc(tp.contentType||'')+'" placeholder="automatic" spellcheck="false"></div>'+
      '<div class="field wide"><div class="dk-row"><button type="button" class="btn" data-ht="'+i+'">Test</button><span class="dk-tsub" id="dkHt'+i+'" style="margin:0;white-space:normal;flex:1">Sends the request from the device and shows the answer.</span></div></div>'+
      '<p class="field-hint wide" style="margin:0">Plain http on the local network. A body that starts with { or [ goes out as application/json, other text as text/plain. The request is stored in the deck configuration, which anyone on your network can read through this portal: avoid putting important passwords in it.</p>';}}
  return h+'</div>';}
function parseHdrs(s){const o={};s.split('\n').forEach(l=>{const c=l.indexOf(':');if(c>0){const k=l.slice(0,c).trim(),v=l.slice(c+1).trim();if(k)o[k]=v;}});return o;}
async function testHttp(t,i){const out=$('dkHt'+i);out.textContent='Sending...';
  try{const r=await api('/api/deck/http-test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(t.tap)});
    for(let n=0;n<30;n++){await new Promise(z=>setTimeout(z,400));const s=await api('/api/deck/http-test?ticket='+r.ticket);
      if(s.done){out.textContent=s.code?('HTTP '+s.code+(s.body?' - '+s.body.slice(0,300):'')):(s.err||'No answer');return;}}
    out.textContent='No result yet - check the URL';}catch(e){out.textContent=e.message;}}
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
/* Grid and positions are per orientation (ori is the one being edited). */
const DEF={landscape:[4,3],portrait:[3,4]};
const grid=p=>{const l=(p.layouts&&p.layouts[ori])||{};return [gi(l.cols,DEF[ori][0]),gi(l.rows,DEF[ori][1])];};
const tpos=t=>(t.pos&&t.pos[ori])||{};
const cl=(v,a,b)=>Math.min(b,Math.max(a,v==null?a:Math.round(v)));
/* Like the firmware's parser: a non-integer takes the default, an integer is clamped to 1..6. */
const gi=(v,d)=>Number.isInteger(v)?Math.min(6,Math.max(1,v)):d;
const tdim=t=>{const q=tpos(t);return [gi(q.w,1),gi(q.h,1)];};
const tileSize=t=>tdim(t).join('x');
const cell=(x,y,w,h)=>{const q={x:x,y:y};if(w>1)q.w=w;if(h>1)q.h=h;return q;};
function setPos(t,q){t.pos=t.pos||{};if(Object.keys(q).length)t.pos[ori]=q;else delete t.pos[ori];if(!Object.keys(t.pos).length)delete t.pos;}
function setSize(t,v){const [w,h]=v.split('x').map(Number);const q=Object.assign({},tpos(t));delete q.w;delete q.h;if(w>1)q.w=w;if(h>1)q.h=h;setPos(t,q);}
const sizeSel=(t,i)=>{const s=tileSize(t),o=['1x1','2x1','1x2','2x2'];if(!o.includes(s))o.push(s);return '<select data-s="'+i+'">'+o.map(v=>'<option'+(v===s?' selected':'')+'>'+v+'</option>').join('')+'</select>';};
/* Where each tile lands, by the firmware's rule: explicit cells first, then
   the rest flow into the first free place that fits. null = not shown. */
function layout(p){const [c,r]=grid(p);const used=[...Array(r)].map(()=>Array(c).fill(false));const at=p.tiles.map(()=>null);
  const fits=(x,y,w,h)=>{if(x+w>c||y+h>r)return false;for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)if(used[j][i])return false;return true;};
  const put=(k,x,y,w,h)=>{for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)used[j][i]=true;at[k]={x:x,y:y,w:w,h:h};};
  p.tiles.forEach((t,k)=>{const q=tpos(t),[w,h]=tdim(t);if(Number.isInteger(q.x)&&Number.isInteger(q.y)){const x=cl(q.x,0,15),y=cl(q.y,0,15);if(fits(x,y,w,h))put(k,x,y,w,h);}});
  p.tiles.forEach((t,k)=>{if(at[k])return;const [w,h]=tdim(t);for(let y=0;y<r&&!at[k];y++)for(let x=0;x<c&&!at[k];x++)if(fits(x,y,w,h))put(k,x,y,w,h);});
  return at;}
/* Pin every placed tile where it shows, so a drag changes only what moved.
   A hidden tile loses its old cell, or it could claim space before a pinned one. */
function pinAll(p,at){p.tiles.forEach((t,k)=>{const a=at[k];if(a){setPos(t,cell(a.x,a.y,a.w,a.h));return;}const q=Object.assign({},tpos(t));delete q.x;delete q.y;setPos(t,q);});}
const free=(at,k,q,c,r)=>q.x>=0&&q.y>=0&&q.x+q.w<=c&&q.y+q.h<=r&&at.every((o,j)=>j===k||!o||o.x>=q.x+q.w||o.x+o.w<=q.x||o.y>=q.y+q.h||o.y+o.h<=q.y);
function gtile(t,k,a){const m=t.type==='metric',mm=m&&pcm.find(x=>x.id===t.metric),en=entMap[t.entity],d=domainOf(t.entity);
  const col=t.color||(m?PAL[((t.metric||1)-1)%8]:(COL[d]||'#7DB5FF')),cp=m?0:tileCp(t);
  const nm=t.label||(m?(mm?mm.name:'Metric #'+t.metric):(en&&en.name)||t.entity||'Tile');
  return '<div class="dk-gt'+(k===sel?' sel':'')+'" data-g="'+k+'"'+(a?' style="grid-area:'+(a.y+1)+'/'+(a.x+1)+'/span '+a.h+'/span '+a.w+'"':'')+'><i style="color:'+col+'"'+(cp?'':' class="tx"')+'>'+(cp?String.fromCodePoint(cp):(m?'PC':esc((d||'?').slice(0,2).toUpperCase())))+'</i><b>'+esc(nm)+'</b>'+(a?'<span class="dk-rs" data-rs="'+k+'"></span>':'')+'</div>';}
/* Drag a tile to a free place or onto a tile of its size (swap); the corner
   handle resizes. A press without movement selects the tile. */
function gdrag(e,p,at,k,size){if(e.button>0)return;e.preventDefault();e.stopPropagation();
  const n=e.currentTarget,tile=size?n.parentElement:n,g=$('dkGrid'),gh=$('dkGhost'),[c,r]=grid(p);
  const b=g.getBoundingClientRect(),cs=getComputedStyle(g),pad=parseFloat(cs.paddingLeft)||0,gap=parseFloat(cs.columnGap)||0;
  const sx=(b.width-2*pad+gap)/c,sy=(b.height-2*pad+gap)/r;
  const a=at[k],[tw,th]=tdim(p.tiles[k]),tb=tile.getBoundingClientRect();
  const ox=a?e.clientX-tb.left:sx/2,oy=a?e.clientY-tb.top:sy/2;
  let moved=false,tgt=null;n.setPointerCapture(e.pointerId);
  n.onpointermove=ev=>{const dx=ev.clientX-e.clientX,dy=ev.clientY-e.clientY;if(!moved&&Math.abs(dx)+Math.abs(dy)<6)return;moved=true;tile.classList.add('drag');
    if(size)tgt={x:a.x,y:a.y,w:cl(Math.round((ev.clientX-b.left-pad)/sx)-a.x,1,Math.min(6,c-a.x)),h:cl(Math.round((ev.clientY-b.top-pad)/sy)-a.y,1,Math.min(6,r-a.y))};
    else{tile.style.transform='translate('+dx+'px,'+dy+'px)';if(tw>c||th>r){tgt=null;gh.hidden=true;return;}
      tgt={x:cl((ev.clientX-ox-b.left-pad)/sx,0,c-tw),y:cl((ev.clientY-oy-b.top-pad)/sy,0,r-th),w:tw,h:th};}
    tgt.j=-1;tgt.ok=free(at,k,tgt,c,r);
    if(!tgt.ok&&!size&&a){tgt.j=at.findIndex((o,j)=>j!==k&&o&&o.x===tgt.x&&o.y===tgt.y&&o.w===tw&&o.h===th);tgt.ok=tgt.j>=0;}
    gh.hidden=false;gh.className='dk-ghost'+(tgt.ok?'':' bad');gh.style.gridArea=(tgt.y+1)+'/'+(tgt.x+1)+'/span '+tgt.h+'/span '+tgt.w;};
  n.onpointerup=n.onpointercancel=ev=>{n.onpointermove=n.onpointerup=n.onpointercancel=null;
    if(!moved){sel=k;renderPage();const row=document.querySelector('.dk-tile.sel');if(row)row.scrollIntoView({block:'nearest',behavior:'smooth'});return;}
    if(ev.type==='pointerup'&&tgt&&tgt.ok){pinAll(p,at);if(tgt.j>=0)setPos(p.tiles[tgt.j],cell(a.x,a.y,tw,th));setPos(p.tiles[k],cell(tgt.x,tgt.y,tgt.w,tgt.h));sel=k;setDirty(true);}
    renderPage();};}
function renderTabs(){const ps=pages();if(cur>=ps.length)cur=Math.max(0,ps.length-1);$('dkTabs').innerHTML=ps.map((p,i)=>'<button type="button" class="chip page-tab'+(i===cur?' on':'')+'" data-i="'+i+'">'+(stackedAt(ps,i)?'&#8595; ':'')+esc(p.name||p.id)+'</button>').join('')+'<button type="button" class="chip page-tab" id="dkAddPage">+ Page</button>';
  $('dkTabs').querySelectorAll('[data-i]').forEach(b=>b.onclick=()=>{cur=+b.dataset.i;sel=-1;more=-1;render();});$('dkAddPage').onclick=()=>{deck.pages.push({id:'p'+Date.now().toString(36),name:'New page',layouts:{landscape:{cols:4,rows:3}},tiles:[]});cur=pages().length-1;setDirty(true);render();};}
/* A page stacked below the one before it (same group) is in its column. */
const stackedAt=(ps,i)=>i>0&&!!ps[i].group&&ps[i].group===ps[i-1].group;
function renderPage(){const ps=pages();const el=$('dkPage');if(!ps.length){el.innerHTML='<p class="dk-empty">No deck pages. Add one with "+ Page".</p>';return;}
  const p=ps[cur];const [c,r]=grid(p),land=ori==='landscape',at=layout(p),lost=p.tiles.map((t,k)=>k).filter(k=>!at[k]);let cells=0;at.forEach(a=>{if(a)cells+=a.w*a.h;});
  let h='<div class="dk-row" style="margin-bottom:12px"><span class="field-label" style="margin:0">Layout</span>'+[['landscape','Landscape'],['portrait','Portrait']].map(o=>'<button type="button" class="chip page-tab'+(o[0]===ori?' on':'')+'" data-o="'+o[0]+'">'+o[1]+'</button>').join('')+'</div>';
  h+='<div class="grid-2"><div class="field"><label class="field-label">Page name</label><input type="text" id="dkPName" value="'+esc(p.name||'')+'"></div><div class="field"><label class="field-label">Grid (columns x rows)</label><div class="dk-row"><select id="dkPCols" style="width:auto">'+(land?[2,3,4,5,6]:[2,3,4]).map(n=>'<option'+(n===c?' selected':'')+'>'+n+'</option>').join('')+'</select><span>x</span><select id="dkPRows" style="width:auto">'+(land?[2,3,4]:[2,3,4,5,6]).map(n=>'<option'+(n===r?' selected':'')+'>'+n+'</option>').join('')+'</select><button type="button" class="btn btn-danger" id="dkPDel" style="margin-left:auto">Delete page</button></div></div></div>';
  h+='<label class="check-row standalone"><input type="checkbox" id="dkPStack"'+(stackedAt(ps,cur)?' checked':'')+(cur===0?' disabled':'')+'><span class="check-box"></span><span class="check-text"><strong>Below the previous page</strong><span class="ct-hint">Stacked pages form one column: swipe up and down between them, left and right to the next column.</span></span></label>';
  let g='';for(let y=0;y<r;y++)for(let x=0;x<c;x++)g+='<div class="dk-cell" style="grid-area:'+(y+1)+'/'+(x+1)+'"></div>';
  const hd=$('dkHeader').checked?30:0;
  h+='<div class="dk-grid'+(land?'':' pt')+'" id="dkGrid" style="grid-template-columns:repeat('+c+',minmax(0,1fr));grid-template-rows:repeat('+r+',minmax(0,1fr));aspect-ratio:'+(land?480:320)+'/'+((land?320:480)-hd)+'">'+g+p.tiles.map((t,k)=>at[k]?gtile(t,k,at[k]):'').join('')+'<div class="dk-ghost" id="dkGhost" hidden></div></div>';
  if(lost.length)h+='<p class="dk-cap over">No room on the page for these: drag one to a free place, make it smaller or remove it.</p><div class="dk-tray">'+lost.map(k=>gtile(p.tiles[k],k,null)).join('')+'</div>';
  h+='<p class="field-hint">Drag a tile to move it, or onto a tile of the same size to swap them; the corner handle resizes. Tap a tile to find it in the list.</p>';
  h+='<div class="dk-tiles">'+p.tiles.map((t,i)=>{if(t.type==='metric')return metricRow(t,i);const d=domainOf(t.entity);const en=entMap[t.entity];const nm=en&&en.name?en.name:(t.entity||'No entity');
    const cp=tileCp(t);
    return '<div class="dk-tile'+(i===sel?' sel':'')+'"><button type="button" class="dk-ic'+(cp?' mdi':'')+'" data-ic="'+i+'" title="'+esc(cp?(t.icon||(en&&en.icon)||'automatic'):'Choose an icon')+'" style="background:'+(t.color||COL[d]||'#7DB5FF')+'">'+(cp?String.fromCodePoint(cp):esc((d||'?').slice(0,2).toUpperCase()))+'</button><div class="dk-tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(nm)+'"><div class="dk-tsub">'+esc(t.entity||(isHttp(t)?'HTTP '+(t.tap.method||'GET')+' '+(t.tap.url||''):'no entity'))+(en&&en.state?' - '+esc(en.state)+(en.unit?' '+esc(en.unit):''):'')+'</div></div>'+
    '<div class="dk-tact"><select data-t="'+i+'"><option value="toggle"'+(t.type==='toggle'?' selected':'')+'>Switch</option><option value="action"'+(t.type==='action'?' selected':'')+'>Button</option><option value="sensor"'+(t.type==='sensor'?' selected':'')+'>Sensor</option>'+(SLIDER.includes(d)||t.type==='slider'?'<option value="slider"'+(t.type==='slider'?' selected':'')+'>Slider</option>':'')+'</select>'+sizeSel(t,i)+'<button type="button" class="btn" data-e="'+i+'">Entity</button><button type="button" class="btn'+(i===more?' on':'')+'" data-mo="'+i+'" title="More options">&#8943;</button><button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div>'+(i===more?moreRow(t,i):'')+'</div>';}).join('')+'</div>';
  h+='<p class="dk-cap">'+cells+' of '+(c*r)+' cells used</p><div class="dk-row" style="margin-top:12px"><button type="button" class="btn btn-accent" id="dkAddTile">+ Add tile from Home Assistant</button><button type="button" class="btn" id="dkAddPc">+ Add PC metric</button><button type="button" class="btn" id="dkAddHttp">+ Add HTTP button</button></div><p class="field-hint">Chart and Bar need a 2x1 or bigger tile (a 1x1 tile shows the number), so picking one makes a 1x1 tile 2x1.</p>';
  el.innerHTML=h;
  $('dkPName').oninput=e=>{p.name=e.target.value;setDirty(true);renderTabs();};
  const setGrid=(gc,gr)=>{p.layouts=p.layouts||{};p.layouts[ori]={cols:gc,rows:gr};setDirty(true);renderPage();};
  $('dkPCols').onchange=e=>setGrid(+e.target.value,r);
  $('dkPRows').onchange=e=>setGrid(c,+e.target.value);
  el.querySelectorAll('[data-o]').forEach(b=>b.onclick=()=>{ori=b.dataset.o;sel=-1;renderPage();});
  el.querySelectorAll('[data-g]').forEach(n=>n.onpointerdown=e=>gdrag(e,p,at,+n.dataset.g,false));
  el.querySelectorAll('[data-rs]').forEach(n=>n.onpointerdown=e=>gdrag(e,p,at,+n.dataset.rs,true));
  /* The page and the pages stacked under it move together. */
  $('dkPStack').onchange=e=>{const prev=ps[cur-1];if(!prev)return;let end=cur+1;while(end<ps.length&&stackedAt(ps,end))end++;const run=ps.slice(cur,end);
    if(e.target.checked){const g=prev.group||prev.id;prev.group=g;run.forEach(x=>x.group=g);}else if(run.length>1)run.forEach(x=>x.group=p.id);else delete p.group;setDirty(true);renderTabs();};
  $('dkPDel').onclick=()=>{if(!confirm('Delete the page "'+(p.name||p.id)+'"?'))return;deck.pages.splice(deck.pages.indexOf(p),1);cur=0;setDirty(true);render();};
  el.querySelectorAll('[data-l]').forEach(x=>x.oninput=e=>{const t=p.tiles[+x.dataset.l];const v=e.target.value.trim();if(v)t.label=v;else delete t.label;setDirty(true);});
  el.querySelectorAll('[data-t]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.t];t.type=e.target.value;if(t.type!=='action')delete t.tap;setDirty(true);renderPage();});
  el.querySelectorAll('[data-s]').forEach(x=>x.onchange=e=>{setSize(p.tiles[+x.dataset.s],e.target.value);setDirty(true);renderPage();});
  el.querySelectorAll('[data-e]').forEach(x=>x.onclick=()=>openPicker(+x.dataset.e));
  el.querySelectorAll('[data-ic]').forEach(x=>x.onclick=()=>openIcons(+x.dataset.ic));
  el.querySelectorAll('[data-u]').forEach(x=>x.onclick=()=>{const i=+x.dataset.u;if(i<1)return;const t=p.tiles.splice(i,1)[0];p.tiles.splice(i-1,0,t);sel=-1;more=-1;setDirty(true);renderPage();});
  el.querySelectorAll('[data-x]').forEach(x=>x.onclick=()=>{p.tiles.splice(+x.dataset.x,1);sel=-1;more=-1;setDirty(true);renderPage();});
  el.querySelectorAll('[data-mo]').forEach(x=>x.onclick=()=>{const i=+x.dataset.mo;more=more===i?-1:i;renderPage();});
  el.querySelectorAll('[data-cf]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.cf];if(e.target.value)t.confirm=e.target.value;else delete t.confirm;setDirty(true);});
  $('dkAddTile').onclick=()=>openPicker(-1);
  $('dkAddHttp').onclick=()=>{p.tiles.push({id:'h'+Date.now().toString(36),type:'action',label:'HTTP',icon:'mdi:web',tap:{action:'http',method:'GET',url:'http://'}});more=p.tiles.length-1;setDirty(true);renderPage();};
  const tapOf=x=>p.tiles[+(x.dataset.ak||x.dataset.hm||x.dataset.hu||x.dataset.hb||x.dataset.hh||x.dataset.hc||x.dataset.ht)];
  el.querySelectorAll('[data-ak]').forEach(x=>x.onchange=e=>{const t=tapOf(x),v=e.target.value;if(v==='keep')return;if(v==='http')t.tap={action:'http',method:'GET',url:'http://'};else delete t.tap;setDirty(true);renderPage();});
  el.querySelectorAll('[data-hm]').forEach(x=>x.onchange=e=>{tapOf(x).tap.method=e.target.value;setDirty(true);renderPage();});
  el.querySelectorAll('[data-hu]').forEach(x=>x.oninput=e=>{tapOf(x).tap.url=e.target.value.trim();setDirty(true);});
  el.querySelectorAll('[data-hb]').forEach(x=>x.oninput=e=>{const a=tapOf(x).tap;if(e.target.value)a.body=e.target.value;else delete a.body;setDirty(true);});
  el.querySelectorAll('[data-hh]').forEach(x=>x.oninput=e=>{const a=tapOf(x).tap,o=parseHdrs(e.target.value);if(Object.keys(o).length)a.headers=o;else delete a.headers;setDirty(true);});
  el.querySelectorAll('[data-hc]').forEach(x=>x.oninput=e=>{const a=tapOf(x).tap,v=e.target.value.trim();if(v)a.contentType=v;else delete a.contentType;setDirty(true);});
  el.querySelectorAll('[data-ht]').forEach(x=>x.onclick=()=>testHttp(tapOf(x),+x.dataset.ht));
  $('dkAddPc').onclick=async()=>{if(!pcm.length)await loadPc();const usedIds=p.tiles.filter(t=>t.type==='metric').map(t=>t.metric);const m=pcm.find(x=>!usedIds.includes(x.id))||pcm[0];if(!m){alert('No PC metrics yet - start the companion app on the PC.');return;}p.tiles.push({id:'m'+Date.now().toString(36),type:'metric',metric:m.id,style:'number'});setDirty(true);renderPage();};
  el.querySelectorAll('[data-m]').forEach(x=>x.onchange=e=>{p.tiles[+x.dataset.m].metric=+e.target.value;setDirty(true);renderPage();});
  el.querySelectorAll('[data-st]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.st];if(e.target.value==='number')delete t.style;else{t.style=e.target.value;if(tileSize(t)==='1x1')setSize(t,'2x1');}setDirty(true);renderPage();});}
function render(){renderTabs();renderPage();renderLed();}

/* entity picker */
function setList(r){ents=(r.entities||[]).slice().sort((a,b)=>a.id<b.id?-1:a.id>b.id?1:0);entMap={};ents.forEach(e=>entMap[e.id]=e);
  $('dkListInfo').textContent=r.updated?(ents.length+' entities, list from '+new Date(r.updated*1000).toLocaleString()):'No list yet - press "Update from HA"';}
async function loadEnts(refresh){if(ents&&!refresh)return;$('dkEnts').innerHTML='<p class="dk-empty">'+(refresh?'Fetching the entity list from Home Assistant...':'Loading...')+'</p>';$('dkListUpdate').disabled=true;
  try{setList(await api('/api/ha/entities'+(refresh?'?refresh=1':'')));if(deck){renderPage();renderLed();}}catch(e){$('dkEnts').innerHTML='<p class="dk-empty">'+esc(e.message)+'<br>Check the Home Assistant settings above.</p>';throw e;}finally{$('dkListUpdate').disabled=false;}}
$('dkListUpdate').onclick=async()=>{try{await loadEnts(true);renderEnts();}catch(e){}};
function renderDomains(){$('dkDomains').innerHTML=DOMS.map(d=>'<span class="chip'+(d[0]===dom?' on':'')+'" data-d="'+d[0]+'">'+d[1]+'</span>').join('');$('dkDomains').querySelectorAll('[data-d]').forEach(c=>c.onclick=()=>{dom=c.dataset.d;renderDomains();renderEnts();});}
function renderEnts(){if(!ents)return;if(!ents.length){$('dkEnts').innerHTML='<p class="dk-empty">No entity list on the device yet. Press "Update from HA".</p>';return;}const q=$('dkQ').value.trim().toLowerCase();let list=ents.filter(e=>(!dom||domainOf(e.id)===dom)&&(!q||e.id.toLowerCase().includes(q)||(e.name||'').toLowerCase().includes(q)));
  if(!dom&&!q){const pri=e=>{const d=domainOf(e.id);return TOGGLE.includes(d)||['scene','script','button','input_button'].includes(d)?0:(d==='sensor'?1:2);};list=list.slice().sort((a,b)=>pri(a)-pri(b));}
  const shown=list.slice(0,250);$('dkEnts').innerHTML=shown.length?shown.map(e=>'<div class="dk-ent" data-id="'+esc(e.id)+'"><div><b>'+esc(e.name||e.id)+'</b><small>'+esc(e.id)+'</small></div><span class="st">'+esc(e.state)+(e.unit?' '+esc(e.unit):'')+'</span></div>').join('')+(list.length>shown.length?'<p class="dk-empty">'+(list.length-shown.length)+' more - refine the search</p>':''):'<p class="dk-empty">Nothing matches.</p>';
  $('dkEnts').querySelectorAll('[data-id]').forEach(r=>r.onclick=()=>pick(r.dataset.id));}
async function openPicker(i){pickFor=i;$('dkQ').value='';$('dkPicker').classList.add('show');renderDomains();try{await loadEnts();renderEnts();$('dkQ').focus();}catch(e){}}
function closePicker(){$('dkPicker').classList.remove('show');}
const LEDDOM=['input_boolean','input_number','light'];
function renderLed(){const id=deck&&deck.led&&deck.led.entity;const en=id&&entMap[id];$('dkLedEnt').textContent=id?((en&&en.name?en.name+' - ':'')+id):'Not linked';$('dkLedClear').disabled=!id;}
$('dkLedPick').onclick=()=>{if(deck)openPicker(-2);};
$('dkLedClear').onclick=()=>{if(!deck)return;delete deck.led;setDirty(true);renderLed();};
function pick(id){if(pickFor===-2){if(!LEDDOM.includes(domainOf(id))){alert('Pick an input_boolean, input_number or light.');return;}deck.led={entity:id};setDirty(true);closePicker();renderLed();return;}
  const p=pages()[cur];const type=autoType(domainOf(id));
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

$('dkHeader').onchange=async e=>{const b=new URLSearchParams();b.set('header',e.target.checked?'1':'0');try{await api('/api/deck/prefs',{method:'POST',body:b});}catch(x){alert(x.message);}if(deck)renderPage();};

/* save */
$('dkSave').onclick=async()=>{try{await api('/api/deck',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(deck)});setDirty(false);}catch(e){alert('Not saved: '+e.message);}};
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});

(async function init(){haStatus();icRefresh();setInterval(()=>{if(visible())haStatus();},3000);
  await loadPc();ori=ori||'landscape';
  try{const r=await api('/api/deck');deck=r.deck;deck.pages=deck.pages.filter(p=>p.type!=='metrics');$('dkHeader').checked=r.header!==false;render();}catch(e){$('dkPage').innerHTML='<p class="dk-empty">'+esc(e.message)+'</p>';}
  try{await loadEnts();}catch(e){}})();
})();)JS";

#endif  // DECK_WEB_H
