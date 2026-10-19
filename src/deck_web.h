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
.dk-sw{gap:6px;flex-wrap:wrap}.dk-sw input[type=color]{width:40px;height:30px;padding:2px;border:1px solid var(--line-2);border-radius:8px;background:var(--card);cursor:pointer}
.dk-swb{width:24px;height:24px;padding:0;border:2px solid transparent;border-radius:50%;cursor:pointer}.dk-swb.on{border-color:var(--ink)}
.dk-more textarea{width:100%;min-height:58px;font:12px var(--mono);background:var(--card);color:var(--ink);border:1px solid var(--line-2);border-radius:var(--r-md);resize:vertical}
.dk-grid{display:grid;gap:6px;padding:6px;width:100%;max-width:560px;margin:12px 0 6px;background:var(--card-2);border:1px solid var(--line);border-radius:var(--r-md);user-select:none}.dk-grid.pt{max-width:300px}
.dk-cell{border:1px dashed var(--line);border-radius:8px}
.dk-gt{position:relative;display:flex;flex-direction:column;justify-content:space-between;gap:4px;min-width:0;min-height:0;overflow:hidden;padding:7px 8px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--ink);cursor:grab;touch-action:none}
.dk-gt.sel{outline:2px solid var(--accent);outline-offset:1px}.dk-gt.row{flex-direction:row;align-items:center;justify-content:flex-start;gap:6px;padding:2px 8px}.dk-gt.ic b{display:none}.dk-gt.drag{z-index:5;opacity:.85;cursor:grabbing;box-shadow:0 8px 20px rgba(0,0,0,.35)}
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
<div class="card" id="dkPrnCard"><div class="card-head"><h2 class="card-title">Printers</h2><span class="tag" id="dkPrnTag">Bambu Lab, LAN</span><button type="button" class="btn" id="dkPrnTog" style="margin-left:auto;padding:5px 10px" aria-expanded="false">Show</button></div>
<div id="dkPrnBody" hidden><p class="field-hint">The printer must be in LAN mode or have LAN access on. Serial and access code are stored only on the device. With a plug (an HA switch), the printer connects and its pages show only while the plug is on. Printers and pages apply on "Save to device" below; Save serial and code applies at once.</p>
<div id="dkPrn"></div><div class="dk-row"><button type="button" class="btn" id="dkPrnAdd">+ Add printer</button></div></div></div>
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
async function loadPc(){try{const c=await api('/api/config');pcm=(c.metrics||[]).slice().sort((a,b)=>a.id-b.id);pcs=c.pcs||[];if(!ori&&c.display)ori=c.display.rotation%2?'landscape':'portrait';}catch(e){pcm=[];pcs=[];}}
/* Known PCs (first = default); a metric tile's PC and its metric list. */
let pcs=[];
const pcOf=t=>pcs.find(x=>x.id===(t.pc||''))||(!t.pc&&pcs[0])||null;
const mlist=t=>{const p=pcOf(t);return p?(p.metrics||[]).slice().sort((a,b)=>a.id-b.id):pcm;};
function pcSel(t,i){if(pcs.length<2&&!t.pc)return '';const cur=pcOf(t);return '<select data-pc="'+i+'">'+pcs.map(x=>'<option value="'+esc(x.id)+'"'+(cur===x?' selected':'')+'>'+esc(x.name)+(x.online?'':' (offline)')+'</option>').join('')+(cur?'':'<option selected>Unknown PC</option>')+'</select>';}
function pcList(){if(!pcs.length)return '';return '<p class="field-hint">PCs: '+pcs.map((x,j)=>esc(x.name)+(j?'':' (default)')+' - '+(x.online?'online':'offline')+' <a href="#" data-fpc="'+esc(x.id)+'">forget</a>').join(', ')+'</p>';}
function metricRow(t,i){const ml=mlist(t),m=ml.find(x=>x.id===t.metric);const col=t.color||PAL[((t.metric||1)-1)%8];
  const opts=ml.map(x=>'<option value="'+x.id+'"'+(x.id===t.metric?' selected':'')+'>'+esc(x.name)+' ('+esc(Math.round(x.value*10)/10)+' '+esc(x.unit)+')</option>').join('')+(m?'':'<option value="'+t.metric+'" selected>#'+t.metric+' (not in the last packet)</option>');
  return '<div class="dk-tile'+(i===sel?' sel':'')+'"><div class="dk-ic" style="background:'+col+'">PC</div><div class="dk-tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(m?m.name:'Metric')+'"><div class="dk-tsub">'+(pcs.length>1&&pcOf(t)?esc(pcOf(t).name)+' ':'PC ')+'metric #'+t.metric+(m?' - '+esc(Math.round(m.value*10)/10)+' '+esc(m.unit):'')+'</div></div>'+
  '<div class="dk-tact">'+pcSel(t,i)+'<select data-m="'+i+'">'+opts+'</select><select data-st="'+i+'">'+[['number','Number'],['chart','Chart'],['bar','Bar']].map(o=>'<option value="'+o[0]+'"'+((t.style||'number')===o[0]?' selected':'')+'>'+o[1]+'</option>').join('')+'</select>'+sizeSel(t,i)+'<button type="button" class="btn'+(i===more?' on':'')+'" data-mo="'+i+'" title="More options">&#8943;</button><button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div>'+(i===more?'<div class="dk-more">'+whenField(t,i)+colorField(t,i)+'</div>':'')+'</div>';}
/* Tile colours: a picker, presets, Auto = the key left out of deck.json. */
const SW=['#F6C56B','#FFB35C','#FF8A7A','#E66767','#D55181','#B3A6FF','#9085E9','#7DB5FF','#3987E5','#5CC9A7','#00A35C','#C98500'];
const TXT=['#FFFFFF','#E6E8EC','#1D2129'];
const autoCol=t=>t.type==='metric'?PAL[((t.metric||1)-1)%8]:(COL[domainOf(t.entity)]||'#7DB5FF');
/* WCAG contrast against a dark tile (Midnight, Glass) and a light one (Daylight). */
const lum=h=>{const v=[1,3,5].map(i=>parseInt(h.substr(i,2),16)/255).map(c=>c<=0.03928?c/12.92:Math.pow((c+0.055)/1.055,2.4));return 0.2126*v[0]+0.7152*v[1]+0.0722*v[2];};
const contrast=(a,b)=>{const x=lum(a),y=lum(b);return (Math.max(x,y)+0.05)/(Math.min(x,y)+0.05);};
function readHint(c){if(!c)return '';const w=[];if(contrast(c,'#1A1D23')<3)w.push('Midnight and Glass');if(contrast(c,'#FFFFFF')<3)w.push('Daylight');return w.length?' <b>Hard to read on '+w.join(' and ')+'.</b>':'';}
function colRow(t,i,k,label,auto,hint,sw){const c=(t[k]||auto).toLowerCase(),on=(t[k]||'').toLowerCase();
  return '<div class="field wide"><label class="field-label">'+label+'</label><div class="dk-row dk-sw"><input type="color" data-co="'+i+'" data-ck="'+k+'" value="'+c+'" title="Any colour">'+
    (sw||SW).map(s=>'<button type="button" class="dk-swb'+(on===s.toLowerCase()?' on':'')+'" data-cs="'+i+'" data-ck="'+k+'" data-v="'+s+'" style="background:'+s+'" title="'+s+'"></button>').join('')+
    '<button type="button" class="btn'+(t[k]?'':' on')+'" data-ca="'+i+'" data-ck="'+k+'">Auto</button></div><p class="field-hint" style="margin:4px 0 0">'+hint+'</p></div>';}
function colorField(t,i){const tx=TXT.concat(SW),theme='Auto uses the theme text colour.';
  if(t.type==='clock')return colRow(t,i,'color','Time colour','#E6E8EC',theme+readHint(t.color),tx)+colRow(t,i,'textColor','Date colour','#E6E8EC',theme+readHint(t.textColor),tx);
  const m=t.type==='metric';
  const what=m?'the name, and the chart or bar line unless Chart colour is set':t.type==='toggle'?'the icon, and the whole tile while it is on':t.type==='slider'?'the icon and the level fill':'the icon';
  let h=colRow(t,i,'color','Colour',autoCol(t),'Colours '+what+'. Auto picks it from the '+(m?'metric':'entity type')+'.');
  if(m)h+=colRow(t,i,'chartColor','Chart colour',t.color||autoCol(t),'The chart or bar line. Auto follows Colour.');
  return h+colRow(t,i,'textColor','Text colour','#E6E8EC',(m?'The value. ':'')+theme+readHint(t.textColor),tx);}
/* The clock tile: no entity or actions, just its size and colours. */
/* Bambu printers: deck.printers is saved with the deck; serial and code go
   straight to the device and are never read back. */
const PSHOW=[['hero','Progress'],['nozzle','Nozzle'],['bed','Bed'],['chamber','Chamber'],['filament','Filament'],['layer','Layer'],['light','Light'],['ams','AMS unit'],['slots','All slots'],['summary','Summary']];
let prnStat={};
const prnName=id=>{const d=(deck.printers||[]).find(x=>x.id===id);return d?(d.name||d.id):id;};
function prnTileName(t){const s=PSHOW.find(x=>x[0]===(t.show||'hero'));return prnName(t.printer)+' '+(s?s[1]:'')+(t.show==='nozzle'&&t.nozzle==='left'?' L':'');}
function printerRow(t,i){const ps=deck.printers||[];
  return '<div class="dk-tile'+(i===sel?' sel':'')+'"><div class="dk-ic" style="background:#F5B83D">3D</div><div class="dk-tmain"><b>Printer</b><div class="dk-row" style="margin-top:4px"><select data-pt="'+i+'">'+ps.map(d=>opt(d.id,d.name||d.id,d.id===t.printer)).join('')+'</select><select data-psh="'+i+'">'+PSHOW.map(([v,l])=>opt(v,l,(t.show||'hero')===v)).join('')+'</select>'+(t.show==='nozzle'?'<select data-pnz="'+i+'">'+opt('right','Right / only',t.nozzle!=='left')+opt('left','Left',t.nozzle==='left')+'</select>':'')+(t.show==='ams'?'<select data-pau="'+i+'">'+[1,2,3,4,5,6].map(n=>opt(n,'Unit '+n,(t.unit||1)===n)).join('')+'</select>':'')+'</div></div>'+
  '<div class="dk-tact">'+sizeSel(t,i)+'<button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div></div>';}
function prnLine(d,s){if(!s.secret)return 'Serial and access code not saved';if(!s.powered)return d.plug?'Off (plug off)':'Off';
  return (s.link||'')+(s.err?' - '+s.err:'')+(s.state?' - '+s.state.toLowerCase():'')+(s.state==='RUNNING'?' '+s.progress+'%':'');}
async function printersStatus(){if(!prnOn())return;try{const r=await api('/api/printers');prnStat={};(r.printers||[]).forEach(s=>prnStat[s.id]=s);renderPrinters();}catch(e){}}
/* The card folds to one line: printer count and how many are online. */
function prnFold(open){$('dkPrnBody').hidden=!open;$('dkPrnTog').textContent=open?'Hide':'Show';$('dkPrnTog').setAttribute('aria-expanded',open);try{localStorage.setItem('dkPrnOpen',open?'1':'');}catch(e){}}
function prnTag(){const ps=deck.printers||[];const on=ps.filter(d=>(prnStat[d.id]||{}).link==='online').length;$('dkPrnTag').textContent=ps.length?ps.length+' printer'+(ps.length>1?'s':'')+', '+on+' online':'Bambu Lab, LAN';}
function renderPrinters(){const el=$('dkPrn');if(!el)return;$('dkPrnCard').classList.toggle('hidden',!prnOn());const ps=deck.printers||[];prnTag();
  if(ps.some((d,i)=>document.activeElement&&document.activeElement.closest&&document.activeElement.closest('[data-prb="'+i+'"]')))return;
  el.innerHTML=ps.map((d,i)=>{const s=prnStat[d.id]||{};
    return '<div class="dk-more" data-prb="'+i+'" style="margin-bottom:10px">'+[['id','Id (letters, digits, _)','h2c'],['name','Name','H2C'],['ip','IP address','192.168.0.54'],['plug','Plug (HA switch, optional)','switch.printer_plug']].map(([k,l,ph])=>'<div class="field"><label class="field-label">'+l+'</label><input type="text" data-pr="'+i+'" data-prk="'+k+'" value="'+esc(d[k]||'')+'" placeholder="'+ph+'" spellcheck="false"></div>').join('')+
    '<div class="field"><label class="field-label">Serial</label><input type="text" id="dkPrS'+i+'" placeholder="'+(s.secret?'saved - leave empty to keep':'printer serial number')+'" autocomplete="off" spellcheck="false"></div>'+
    '<div class="field"><label class="field-label">Access code</label><input type="password" id="dkPrC'+i+'" placeholder="'+(s.secret?'saved - leave empty to keep':'LAN access code')+'" autocomplete="off"></div>'+
    '<div class="field wide"><div class="dk-row"><button type="button" class="btn" data-prs="'+i+'">Save serial and code</button><button type="button" class="btn" data-prp="'+i+'">Create printer page</button><button type="button" class="btn btn-danger" data-prx="'+i+'">Remove</button></div><div class="dk-status"><span class="dk-dot '+(s.link==='online'?'ok':s.link==='failed'?'err':s.powered?'busy':'')+'"></span><span>'+esc(prnLine(d,s))+'</span></div></div></div>';}).join('')||'<p class="dk-empty">No printers yet.</p>';
  el.querySelectorAll('[data-pr]').forEach(x=>x.onchange=()=>{const d=ps[+x.dataset.pr],k=x.dataset.prk,v=x.value.trim();
    if(k==='id'&&v!==d.id){deck.pages.forEach(p=>{if(p.printer===d.id)p.printer=v;p.tiles.forEach(t=>{if(t.type==='printer'&&t.printer===d.id)t.printer=v;});});}
    if(v||k==='id')d[k]=v;else delete d[k];setDirty(true);renderPage();});
  el.querySelectorAll('[data-prs]').forEach(b=>b.onclick=async()=>{const i=+b.dataset.prs,d=ps[i],q=new URLSearchParams();q.set('id',d.id);q.set('serial',$('dkPrS'+i).value.trim());q.set('code',$('dkPrC'+i).value.trim());
    if(dirty&&!confirm('Save the deck first, or the device will not know this printer yet. Store the serial and code anyway?'))return;
    try{await api('/api/printers/secret',{method:'POST',body:q});$('dkPrS'+i).value='';$('dkPrC'+i).value='';printersStatus();}catch(e){alert(e.message);}});
  el.querySelectorAll('[data-prp]').forEach(b=>b.onclick=()=>{const d=ps[+b.dataset.prp];deck.pages.push(...printerPage(d,prnStat[d.id]||{}));cur=pages().length-2;setDirty(true);render();});
  el.querySelectorAll('[data-prx]').forEach(b=>b.onclick=()=>{const d=ps[+b.dataset.prx];if(!confirm('Remove '+(d.name||d.id)+' and its printer tiles?'))return;
    deck.printers=ps.filter(x=>x!==d);if(!deck.printers.length)delete deck.printers;deck.pages.forEach(p=>{if(p.printer===d.id)delete p.printer;p.tiles=p.tiles.filter(t=>!(t.type==='printer'&&t.printer===d.id));});setDirty(true);render();});}
/* Progress and temperatures, with an AMS page stacked below. */
function printerPage(d,s){const id=d.id,u=Date.now().toString(36).slice(-4);
  const t=(show,x,y,w,ex)=>Object.assign({id:show+(ex&&(ex.nozzle||ex.unit)||'')+u,type:'printer',printer:id,show:show,pos:{landscape:{x:x,y:y,w:w,h:1}}},ex||{});
  const tiles=[t('hero',0,0,4)];
  if(s.dual)tiles.push(t('nozzle',0,1,1,{nozzle:'left'}),t('nozzle',1,1,1),t('bed',2,1,1),t('chamber',3,1,1),t('filament',0,2,2),t('layer',2,2,1),t('light',3,2,1));
  else if(s.chamber)tiles.push(t('nozzle',0,1,1),t('bed',1,1,1),t('chamber',2,1,1),t('layer',3,1,1),t('filament',0,2,3),t('light',3,2,1));
  else tiles.push(t('nozzle',0,1,1),t('bed',1,1,1),t('layer',2,1,1),t('light',3,1,1),t('filament',0,2,4));
  let pid='prn_'+id,n=2;while(deck.pages.some(p=>p.id===pid||p.id===pid+'_ams'))pid='prn_'+id+'_'+(n++);
  const ams=[],count=Math.max(1,Math.min(6,Math.floor(+s.ams)||2));
  for(let i=0;i<count;i++)ams.push(t('ams',(i%2)*2,Math.floor(i/2),2,{unit:i+1}));
  if(count<=4)ams.push(t('slots',0,2,4));
  return [{id:pid,name:d.name||id,group:pid,printer:id,tiles:tiles},{id:pid+'_ams',name:(d.name||id)+' AMS',group:pid,printer:id,tiles:ams}];}
function clockRow(t,i){return '<div class="dk-tile'+(i===sel?' sel':'')+'"><div class="dk-ic" style="background:'+(t.color||'#E6E8EC')+'">12</div><div class="dk-tmain"><b>Clock</b><div class="dk-tsub">Time and date (24h and time zone from Time settings)</div></div>'+
  '<div class="dk-tact">'+sizeSel(t,i)+'<button type="button" class="btn'+(i===more?' on':'')+'" data-mo="'+i+'" title="More options">&#8943;</button><button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div>'+(i===more?'<div class="dk-more">'+whenField(t,i)+colorField(t,i)+'</div>':'')+'</div>';}
/* A tile's "more options" panel: confirm, colour, and for buttons the tap action. */
const opt=(v,l,on)=>'<option value="'+esc(v)+'"'+(on?' selected':'')+'>'+esc(l)+'</option>';
const isHttpSensor=t=>t.type==='sensor'&&!!t.http;
const isHttp=t=>!!(t.tap&&t.tap.action==='http');
const isPcCmd=t=>!!(t.tap&&t.tap.action==='pc');
/* PC commands: [cmd, label, icon]; the last three ask on the device by default. */
const PCCMD=[['media_play_pause','Play / pause','play-pause'],['media_next','Next track','skip-next'],['media_prev','Previous track','skip-previous'],['media_stop','Stop','stop'],['volume_up','Volume up','volume-plus'],['volume_down','Volume down','volume-minus'],['volume_mute','Mute','volume-mute'],['lock','Lock','lock'],['sleep','Sleep','power-sleep'],['restart','Restart','restart'],['shutdown','Shut down','power'],['hotkey','Hotkey','keyboard']];
const RISKY=['sleep','restart','shutdown'];
function pcCmdSet(t,cmd){const c=PCCMD.find(x=>x[0]===cmd)||PCCMD[0];const old=PCCMD.find(x=>x[0]===(t.tap&&t.tap.cmd));
  t.tap=Object.assign({action:'pc',cmd:c[0]},t.tap&&t.tap.pc?{pc:t.tap.pc}:{},c[0]==='hotkey'?{keys:(t.tap&&t.tap.keys)||'ctrl+shift+m'}:{});
  if(!t.label||(old&&t.label===old[1]))t.label=c[1];if(!t.icon||(old&&t.icon==='mdi:'+old[2]))t.icon='mdi:'+c[2];
  if(RISKY.includes(c[0]))t.confirm='always';else if(old&&RISKY.includes(old[0])&&t.confirm==='always')delete t.confirm;}
const tapName=a=>a.action==='page'?'Open page '+a.page:a.action==='call-service'?'Service '+a.domain+'.'+a.service:a.action;
const hdrText=h=>Object.keys(h||{}).map(k=>k+': '+h[k]).join('\n');
function moreRow(t,i){const cf=t.confirm||'';
  let h='<div class="dk-more"><div class="field"><label class="field-label">Confirm on the device</label><select data-cf="'+i+'">'+[['','No'],['off','Before turning off'],['always','Before every action']].map(o=>opt(o[0],o[1],cf===o[0])).join('')+'</select></div>';
  if(t.type==='sensor'){
    h+='<div class="field"><label class="field-label">Style</label><select data-st="'+i+'">'+opt('number','Number',t.style!=='chart')+opt('chart','Chart',t.style==='chart')+'</select></div>';
    h+='<div class="field"><label class="field-label">Show as</label><select data-fm="'+i+'" title="Duration turns a number in s, min, h or d into 1h 49m">'+opt('','Value',t.format!=='duration')+opt('duration','Duration (s, min, h, d)',t.format==='duration')+'</select></div>';
    if(t.style==='chart'&&!isHttpSensor(t))h+='<div class="field"><label class="field-label">History hours</label><input type="number" min="1" max="168" step="1" data-sh="'+i+'" value="'+esc(t.hours==null?24:t.hours)+'"></div>';
    [['warn','Warn at'],['crit','Critical at']].forEach(([k,label])=>{h+='<div class="field"><label class="field-label">'+label+'</label><input type="number" step="any" data-th="'+i+'" data-tk="'+k+'" value="'+esc(t[k]==null?'':t[k])+'" placeholder="Unset"></div>';});
    if(!isHttpSensor(t))h+='<p class="field-hint wide" style="margin:0">History defaults to 24 h; longer periods take longer to load from Home Assistant (a chatty sensor over a week can take many seconds). Thresholds use the sensor unit; Critical lower than Warn = low is bad.</p>';
  }
  if(t.type==='action'||isHttpSensor(t)){const tp=t.tap||{},k=isHttp(t)?'http':(t.tap?'keep':'');
    const kk=isPcCmd(t)?'pc':k;
    h+='<div class="field"><label class="field-label">On tap</label><select data-ak="'+i+'">'+opt('',isHttpSensor(t)?'None':'Home Assistant entity',kk==='')+opt('http','HTTP request',kk==='http')+opt('pc','PC command',kk==='pc')+(kk==='keep'?opt('keep',tapName(tp),true):'')+'</select></div>';
    if(kk==='pc'){h+='<div class="field"><label class="field-label">Command</label><select data-pcc="'+i+'">'+PCCMD.map(c=>opt(c[0],c[1],tp.cmd===c[0])).join('')+'</select></div>';
      if(tp.cmd==='hotkey')h+='<div class="field"><label class="field-label">Keys</label><input type="text" data-pck="'+i+'" value="'+esc(tp.keys||'')+'" placeholder="ctrl+shift+m" spellcheck="false"></div><p class="field-hint wide" style="margin:0">Joined with +, up to 5: ctrl, shift, alt, win, a-z, 0-9, f1-f24, space, enter, tab, esc, backspace, delete, insert, home, end, pageup, pagedown, up, down, left, right, printscreen. The PC gets them in its focused window.</p>';
      h+='<div class="field"><label class="field-label">PC</label><select data-pcp="'+i+'">'+opt('','Default PC'+(pcs[0]?' ('+pcs[0].name+')':''),!tp.pc)+pcs.map(x=>opt(x.id,x.name+(x.paired?'':' - not paired'),tp.pc===x.id)).join('')+'</select></div>';
      const tg=pcs.find(x=>x.id===(tp.pc||''))||(!tp.pc&&pcs[0]);
      h+='<p class="field-hint wide" style="margin:0">'+(tg&&tg.paired?'Paired with '+esc(tg.name)+'.':'Pair the PC first: TouchDeck Companion, Device page, "Pair for control".')+' Sleep, Restart and Shut down ask on the device before they run.</p>';}
    if(k==='http'){const body=typeof tp.body==='object'&&tp.body!==null?JSON.stringify(tp.body):(tp.body||'');
      h+='<div class="field"><label class="field-label">Method</label><select data-hm="'+i+'">'+['GET','POST','PUT','PATCH','DELETE'].map(m=>opt(m,m,(tp.method||'GET')===m)).join('')+'</select></div>'+
      '<div class="field wide"><label class="field-label">URL</label><input type="text" data-hu="'+i+'" value="'+esc(tp.url||'')+'" placeholder="http://192.168.0.50/cm?cmnd=Power%20Toggle" spellcheck="false"></div>'+
      '<div class="field"><label class="field-label">Body (optional)</label><textarea data-hb="'+i+'" spellcheck="false">'+esc(body)+'</textarea></div>'+
      '<div class="field"><label class="field-label">Headers (Name: value)</label><textarea data-hh="'+i+'" spellcheck="false" placeholder="Authorization: Bearer ...">'+esc(hdrText(tp.headers))+'</textarea></div>'+
      '<div class="field"><label class="field-label">Content type</label><input type="text" data-hc="'+i+'" value="'+esc(tp.contentType||'')+'" placeholder="automatic" spellcheck="false"></div>'+
      '<div class="field wide"><div class="dk-row"><button type="button" class="btn" data-ht="'+i+'">Test</button><span class="dk-tsub" id="dkHt'+i+'" style="margin:0;white-space:normal;flex:1">Sends the request from the device and shows the answer.</span></div></div>'+
      '<p class="field-hint wide" style="margin:0">Plain http on the local network. A body that starts with { or [ goes out as application/json, other text as text/plain. The request is stored in the deck configuration, which anyone on your network can read through this portal: avoid putting important passwords in it.</p>';}}
  return h+whenField(t,i)+colorField(t,i)+'</div>';}
function parseHdrs(s){const o={};s.split('\n').forEach(l=>{const c=l.indexOf(':');if(c>0){const k=l.slice(0,c).trim(),v=l.slice(c+1).trim();if(k)o[k]=v;}});return o;}
function httpSensorFields(t,i){const h=t.http;
  return '<div class="dk-more">'+[['url','URL','http://192.168.0.10:8080/status'],['path','JSON path','ups.load'],['poll','Poll (s)','30'],['unit','Unit','%'],['detail','Detail line','runtime {ups.runtime_min|min}']].map(([k,label,ph])=>'<div class="field'+(k==='url'||k==='detail'?' wide':'')+'"><label class="field-label">'+label+'</label><input '+(k==='poll'?'type="number" min="5" max="3600" step="1"':'type="text"')+' data-hs="'+i+'" data-hsk="'+k+'" value="'+esc(k==='unit'||k==='detail'?(t[k]||''):h[k]==null?'':h[k])+'" placeholder="'+ph+'" spellcheck="false"></div>').join('')+
    '<p class="field-hint wide" style="margin:0">Detail line: a small line under the name. {path} inserts a value from the same reply, {path|min} shows a number of minutes as 1h 49m (also s, h, d). Test, then + Detail next to a value adds it.</p>'+
    '<div class="field wide"><label class="field-label">Headers (Name: value)</label><textarea data-hsh="'+i+'" spellcheck="false">'+esc(hdrText(h.headers))+'</textarea></div>'+
    '<div class="field wide"><button type="button" class="btn" data-hst="'+i+'">Test</button><div id="dkHs'+i+'" style="max-height:260px;overflow:auto;overflow-wrap:anywhere">Fetch JSON from the device, then click a value to select its path.</div></div></div>';}
function sensorTree(out,data,t,i){
  const tree=document.createElement('div');
  const add=(parent,value,path,label,usable)=>{
    if(value!==null&&typeof value==='object'){
      const d=document.createElement('details'),s=document.createElement('summary');d.open=path.length<2;s.textContent=label+(Array.isArray(value)?' []':' {}');d.appendChild(s);
      const inner=document.createElement('div');inner.style.paddingLeft='14px';d.appendChild(inner);parent.appendChild(d);
      Object.entries(value).forEach(([k,v])=>add(inner,v,path.concat(k),k,usable&&k!==''&&!k.includes('.')&&(Array.isArray(value)||!/^\d+$/.test(k))));
    }else{
      const row=document.createElement('div'),b=document.createElement('button');b.type='button';b.className='btn';b.style.whiteSpace='normal';b.textContent=label+': '+JSON.stringify(value);b.disabled=!usable;
      b.onclick=()=>{t.http.path=path.join('.');const input=document.querySelector('[data-hs="'+i+'"][data-hsk="path"]');if(input)input.value=t.http.path;selected.textContent=(t.http.path||'(whole body)')+' = '+JSON.stringify(value);setDirty(true);};row.appendChild(b);
      if(usable&&path.length){const a=document.createElement('button');a.type='button';a.className='btn';a.textContent='+ Detail';a.style.marginLeft='6px';
        a.onclick=()=>{t.detail=(t.detail?t.detail+' ':'')+'{'+path.join('.')+'}';const input=document.querySelector('[data-hs="'+i+'"][data-hsk="detail"]');if(input)input.value=t.detail;setDirty(true);};row.appendChild(a);}
      parent.appendChild(row);
    }
  };
  const selected=document.createElement('div');out.appendChild(selected);add(tree,data,[],'JSON',true);out.appendChild(tree);
}
async function testHttpSensor(t,i){const out=$('dkHs'+i),button=document.querySelector('[data-hst="'+i+'"]');out.textContent='Fetching...';button.disabled=true;
  try{const r=await api('/api/deck/http-test?sensor=1',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'http',method:'GET',url:t.http.url,headers:t.http.headers})});
    for(let n=0;n<90;n++){await new Promise(z=>setTimeout(z,400));if(!out.isConnected)return;const s=await api('/api/deck/http-test?ticket='+r.ticket);
      if(!s.done)continue;out.textContent=(s.code?'HTTP '+s.code:'No answer')+(s.err?' - '+s.err:'');
      if(s.body){try{sensorTree(out,JSON.parse(s.body),t,i);}catch(e){const raw=document.createElement('pre');raw.style.whiteSpace='pre-wrap';raw.textContent=s.body.slice(0,8192);out.appendChild(raw);const hint=document.createElement('div');hint.textContent=s.body.length>=8192?'Preview truncated to 8 KB. Enter the path manually.':'The reply is not valid JSON.';out.appendChild(hint);}}return;
    }out.textContent='No result yet - try again';
  }catch(e){out.textContent=e.message;}finally{button.disabled=false;}}
async function testHttp(t,i){const out=$('dkHt'+i);out.textContent='Sending...';
  try{const r=await api('/api/deck/http-test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(t.tap)});
    for(let n=0;n<30;n++){await new Promise(z=>setTimeout(z,400));const s=await api('/api/deck/http-test?ticket='+r.ticket);
      if(s.done){out.textContent=s.code?('HTTP '+s.code+(s.body?' - '+s.body.slice(0,300):'')):(s.err||'No answer');return;}}
    out.textContent='No result yet - check the URL';}catch(e){out.textContent=e.message;}}
async function api(p,o){const r=await fetch(p,o);const t=await r.text();let j=null;try{j=JSON.parse(t);}catch(e){}if(!r.ok)throw new Error((j&&(j.message||j.error))||t||('HTTP '+r.status));return j;}
const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const domainOf=e=>{const i=(e||'').indexOf('.');return i>0?e.slice(0,i):'';};
function setDirty(v){dirty=v;$('dkMsg').textContent=v?'Unsaved changes':'Saved';}
/* Printers off (Advanced page, window.snPrn from portal.js): printer pages and
   tiles stay in deck.json but are left out here, as the firmware does. */
const prnOn=()=>!!window.snPrn,offT=t=>t.type==='printer'&&!prnOn();
/* "when" tiles: k names the condition (a PC, or an entity and state), neg = shown while it is not met.
   pv holds the preview's answer per condition (default met); a tile it hides is laid out like an off one. */
let pv={};
const whenOf=t=>{const w=t.when;if(w==='pc-on'||w==='pc-off')return {k:'pc:'+((isPcCmd(t)?t.tap.pc:t.pc)||''),neg:w==='pc-off'};
  if(w&&typeof w==='object'){const s=String(w.state||''),n=s[0]==='!';return {k:'e:'+(w.entity||'')+'='+(n?s.slice(1):s),neg:n};}return null;};
const hidT=t=>{if(offT(t))return true;const c=whenOf(t);return !!c&&(pv[c.k]!==false)===c.neg;};
function whenBar(p){const ks=[];p.tiles.forEach(t=>{const c=whenOf(t);if(c&&!offT(t)&&!ks.includes(c.k))ks.push(c.k);});if(!ks.length)return '';
  return '<div class="dk-row" style="margin-bottom:12px"><span class="field-label" style="margin:0">Preview</span>'+ks.map(k=>{let nm;
    if(k.startsWith('pc:')){const pc=pcs.find(x=>x.id===k.slice(3))||(k==='pc:'&&pcs[0]);nm=pc?pc.name:'PC';}else nm=k.slice(2).replace('=',' is ');
    const on=pv[k]!==false,yes=k.startsWith('pc:')?['On','Off']:['Yes','No'];
    return '<span class="dk-tsub" style="margin:0 0 0 8px">'+esc(nm)+'</span>'+[[1,yes[0]],[0,yes[1]]].map(o=>'<button type="button" class="chip page-tab'+(on===!!o[0]?' on':'')+'" data-pv="'+esc(k)+'" data-pvv="'+o[0]+'">'+o[1]+'</button>').join('');}).join('')+'</div>';}
function whenField(t,i){const w=t.when,k=!w?'':typeof w==='string'?w:'entity';
  let h='<div class="field"><label class="field-label">Show</label><select data-wh="'+i+'">'+[['','Always'],['pc-on','Only while the PC is on'],['pc-off','Only while the PC is off'],['entity','Only while an entity is in a state']].map(o=>opt(o[0],o[1],k===o[0])).join('')+'</select></div>';
  if(k==='entity'){const s=String(w.state||''),n=s[0]==='!';
    h+='<div class="field"><label class="field-label">Entity</label><input type="text" data-we="'+i+'" value="'+esc(w.entity||'')+'" placeholder="switch.pc_power" spellcheck="false"></div>'+
    '<div class="field"><label class="field-label">State</label><div class="dk-row"><select data-wn="'+i+'" style="width:auto">'+opt('','is',!n)+opt('!','is not',n)+'</select><input type="text" data-ws="'+i+'" value="'+esc(n?s.slice(1):s)+'" placeholder="on" spellcheck="false"></div></div>';}
  if(k)h+='<p class="field-hint wide" style="margin:0">'+(k==='entity'?'The state as Home Assistant reports it (on, off, home, playing...). Hidden until it is known.':'The PC counts as off about a minute after its companion goes quiet.')+' Its cell is free otherwise: another tile with the opposite condition can take it. Switch the Preview above the grid to place it.</p>';
  return h;}
const pages=()=>deck.pages.filter(p=>p.type!=='metrics'&&(prnOn()||!p.printer));
window.dkPrnSync=()=>{if(deck)render();};
const visible=()=>document.querySelector('.page.active')?.dataset.page==='deck';

/* Home Assistant */
async function haStatus(){try{const s=await api('/api/ha/status');if(document.activeElement!==$('dkHaUrl'))$('dkHaUrl').value=s.url||'';$('dkHaToken').placeholder=s.has_token?'saved - leave empty to keep':'paste the token';const l=s.link;$('dkHaDot').className='dk-dot '+(l==='connected'?'ok':(l==='token rejected'||l==='unreachable')?'err':(l==='not set up'?'':'busy'));$('dkHaState').textContent=l+(s.error?' - '+s.error:'');$('dkHaVer').textContent=s.version?('HA '+s.version):'-';}catch(e){$('dkHaState').textContent='status unavailable';}}
$('dkHaSave').onclick=async()=>{const b=new URLSearchParams();b.set('url',$('dkHaUrl').value.trim());b.set('token',$('dkHaToken').value.trim());try{await api('/api/ha/config',{method:'POST',body:b});$('dkHaToken').value='';haStatus();}catch(e){alert(e.message);}};
$('dkHaForget').onclick=async()=>{if(!confirm('Remove the Home Assistant address and token from the device?'))return;try{await api('/api/ha/forget',{method:'POST'});haStatus();}catch(e){alert(e.message);}};

/* pages and tiles */
/* Grid and positions are per orientation (ori is the one being edited).
   The editor counts positions and spans in half cells (deck.json: 1.5). */
const DEF={landscape:[4,3],portrait:[3,4]};
const grid=p=>{const l=(p.layouts&&p.layouts[ori])||{};return [gi(l.cols,DEF[ori][0]),gi(l.rows,DEF[ori][1])];};
const tpos=t=>(t.pos&&t.pos[ori])||{};
const cl=(v,a,b)=>Math.min(b,Math.max(a,v==null?a:Math.round(v)));
/* Like the firmware's parser: a non-integer takes the default, an integer is clamped to 1..6. */
const gi=(v,d)=>Number.isInteger(v)?Math.min(6,Math.max(1,v)):d;
/* Like the firmware: a number rounds to the nearest half cell and is clamped, anything else takes the default. */
const hv=(v,d,a,b)=>typeof v==='number'&&isFinite(v)?Math.min(b,Math.max(a,Math.round(v*2))):d;
const tdim=t=>{const q=tpos(t);return [hv(q.w,2,1,12),hv(q.h,2,1,12)];};
const tileSize=t=>tdim(t).map(v=>String(v/2)).join('x');
const cell=(x,y,w,h)=>{const q={x:x/2,y:y/2};if(w!==2)q.w=w/2;if(h!==2)q.h=h/2;return q;};
function setPos(t,q){t.pos=t.pos||{};if(Object.keys(q).length)t.pos[ori]=q;else delete t.pos[ori];if(!Object.keys(t.pos).length)delete t.pos;}
function setSize(t,v){const [w,h]=v.split('x').map(Number);const q=Object.assign({},tpos(t));delete q.w;delete q.h;if(w!==1)q.w=w;if(h!==1)q.h=h;setPos(t,q);}
const sizeSel=(t,i)=>{const s=tileSize(t),o=['1x1','1.5x1','2x1','3x1','1x1.5','1.5x1.5','1x2','2x2'];if(!o.includes(s))o.push(s);return '<select data-s="'+i+'">'+o.map(v=>'<option'+(v===s?' selected':'')+'>'+v+'</option>').join('')+'</select>';};
/* Where each tile lands, by the firmware's rule: explicit cells first, then
   the rest flow into the first free place that fits. null = not shown. */
function layout(p){const [gc,gr]=grid(p),c=gc*2,r=gr*2;const used=[...Array(r)].map(()=>Array(c).fill(false));const at=p.tiles.map(()=>null);
  const fits=(x,y,w,h)=>{if(x+w>c||y+h>r)return false;for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)if(used[j][i])return false;return true;};
  const put=(k,x,y,w,h)=>{for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)used[j][i]=true;at[k]={x:x,y:y,w:w,h:h};};
  p.tiles.forEach((t,k)=>{if(hidT(t))return;const q=tpos(t),[w,h]=tdim(t);if(typeof q.x==='number'&&typeof q.y==='number'){const x=hv(q.x,0,0,30),y=hv(q.y,0,0,30);if(fits(x,y,w,h))put(k,x,y,w,h);}});
  p.tiles.forEach((t,k)=>{if(at[k]||hidT(t))return;const [w,h]=tdim(t);for(let y=0;y<r&&!at[k];y++)for(let x=0;x<c&&!at[k];x++)if(fits(x,y,w,h))put(k,x,y,w,h);});
  return at;}
/* Pin every placed tile where it shows, so a drag changes only what moved.
   A hidden tile loses its old cell, or it could claim space before a pinned one. */
function pinAll(p,at){p.tiles.forEach((t,k)=>{if(hidT(t))return;const a=at[k];if(a){setPos(t,cell(a.x,a.y,a.w,a.h));return;}const q=Object.assign({},tpos(t));delete q.x;delete q.y;setPos(t,q);});}
const free=(at,k,q,c,r)=>q.x>=0&&q.y>=0&&q.x+q.w<=c&&q.y+q.h<=r&&at.every((o,j)=>j===k||!o||o.x>=q.x+q.w||o.x+o.w<=q.x||o.y>=q.y+q.h||o.y+o.h<=q.y);
const ovl=(a,b)=>{const w=Math.min(a.x+a.w,b.x+b.w)-Math.max(a.x,b.x),h=Math.min(a.y+a.h,b.y+b.h)-Math.max(a.y,b.y);return w>0&&h>0?w*h:0;};
const garea=a=>(a.y+1)+'/'+(a.x+1)+'/span '+a.h+'/span '+a.w;
/* Tile k dropped at q onto other tiles: the one it covers takes k's old place
   (anchored at a corner of it, so a bigger tile can grow into free cells). A
   same-size tile covered at least half snaps q onto it. Mirrors edCheck() in deck_ui.cpp. */
function swapPlan(at,k,q,c,r){const me=at[k];let j=-1,best=0,hits=0;
  at.forEach((o,i)=>{if(i===k||!o)return;const v=ovl(o,q);if(!v)return;hits++;if(v>best){best=v;j=i;}});
  if(j<0)return null;const o=at[j];
  if(o.w===me.w&&o.h===me.h&&best*2>=me.w*me.h)return {x:o.x,y:o.y,j:j,to:{x:me.x,y:me.y,w:o.w,h:o.h}};
  if(hits>1)return null;const rest=at.map((v,i)=>i===j?null:v);
  for(const y of [me.y,me.y+me.h-o.h])for(const x of [me.x,me.x+me.w-o.w]){const to={x:x,y:y,w:o.w,h:o.h};if(!ovl(to,q)&&free(rest,k,to,c,r))return {x:q.x,y:q.y,j:j,to:to};}
  return null;}
function gtile(t,k,a){const m=t.type==='metric',mm=m&&mlist(t).find(x=>x.id===t.metric),en=entMap[t.entity],d=domainOf(t.entity);
  const ck=t.type==='clock',pr=t.type==='printer',col=t.color||(m?PAL[((t.metric||1)-1)%8]:ck?'#E6E8EC':pr?'#F5B83D':(COL[d]||'#7DB5FF')),cp=m||ck||pr?0:tileCp(t);
  const nm=ck?'Clock':pr?prnTileName(t):t.label||(m?(mm?mm.name:'Metric #'+t.metric):(en&&en.name)||t.entity||'Tile');
  return '<div class="dk-gt'+(k===sel?' sel':'')+(a&&a.h<2?' row':'')+(a&&a.h<2&&a.w<2?' ic':'')+'" data-g="'+k+'"'+(a?' style="grid-area:'+(a.y+1)+'/'+(a.x+1)+'/span '+a.h+'/span '+a.w+'"':'')+'><i style="color:'+col+'"'+(cp?'':' class="tx"')+'>'+(cp?String.fromCodePoint(cp):(m?'PC':ck?'12':pr?'3D':esc((d||'?').slice(0,2).toUpperCase())))+'</i><b>'+esc(nm)+'</b>'+(a?'<span class="dk-rs" data-rs="'+k+'"></span>':'')+'</div>';}
/* Drag a tile to a free place or onto another tile, which moves to the dragged
   tile's old place (swapPlan, shown live); the corner handle resizes. A press without movement selects the tile. */
function gdrag(e,p,at,k,size){if(e.button>0)return;e.preventDefault();e.stopPropagation();
  const n=e.currentTarget,tile=size?n.parentElement:n,g=$('dkGrid'),gh=$('dkGhost'),[gc,gr]=grid(p),c=gc*2,r=gr*2;
  const b=g.getBoundingClientRect(),cs=getComputedStyle(g),pad=parseFloat(cs.paddingLeft)||0,gap=parseFloat(cs.columnGap)||0;
  const sx=(b.width-2*pad+gap)/c,sy=(b.height-2*pad+gap)/r;
  const a=at[k],[tw,th]=tdim(p.tiles[k]),tb=tile.getBoundingClientRect();
  const ox=a?e.clientX-tb.left:sx/2,oy=a?e.clientY-tb.top:sy/2;
  let moved=false,tgt=null,shown=-1;n.setPointerCapture(e.pointerId);
  n.onpointermove=ev=>{const dx=ev.clientX-e.clientX,dy=ev.clientY-e.clientY;if(!moved&&Math.abs(dx)+Math.abs(dy)<6)return;moved=true;tile.classList.add('drag');
    if(size)tgt={x:a.x,y:a.y,w:cl(Math.round((ev.clientX-b.left-pad)/sx)-a.x,1,c-a.x),h:cl(Math.round((ev.clientY-b.top-pad)/sy)-a.y,1,r-a.y)};
    else{tile.style.transform='translate('+dx+'px,'+dy+'px)';if(tw>c||th>r){tgt=null;gh.hidden=true;return;}
      tgt={x:cl((ev.clientX-ox-b.left-pad)/sx,0,c-tw),y:cl((ev.clientY-oy-b.top-pad)/sy,0,r-th),w:tw,h:th};}
    tgt.j=-1;tgt.ok=free(at,k,tgt,c,r);
    if(!tgt.ok&&!size&&a){const s=swapPlan(at,k,tgt,c,r);if(s){tgt.x=s.x;tgt.y=s.y;tgt.j=s.j;tgt.to=s.to;tgt.ok=true;}}
    if(shown!==tgt.j){const q=shown>=0&&g.querySelector('[data-g="'+shown+'"]');if(q)q.style.gridArea=garea(at[shown]);shown=tgt.j;const s=shown>=0&&g.querySelector('[data-g="'+shown+'"]');if(s)s.style.gridArea=garea(tgt.to);}
    gh.hidden=false;gh.className='dk-ghost'+(tgt.ok?'':' bad');gh.style.gridArea=(tgt.y+1)+'/'+(tgt.x+1)+'/span '+tgt.h+'/span '+tgt.w;};
  n.onpointerup=n.onpointercancel=ev=>{n.onpointermove=n.onpointerup=n.onpointercancel=null;
    if(!moved){sel=k;renderPage();const row=document.querySelector('.dk-tile.sel');if(row)row.scrollIntoView({block:'nearest',behavior:'smooth'});return;}
    if(ev.type==='pointerup'&&tgt&&tgt.ok){pinAll(p,at);if(tgt.j>=0)setPos(p.tiles[tgt.j],cell(tgt.to.x,tgt.to.y,tgt.to.w,tgt.to.h));setPos(p.tiles[k],cell(tgt.x,tgt.y,tgt.w,tgt.h));sel=k;setDirty(true);}
    renderPage();};}
function renderTabs(){const ps=pages();if(cur>=ps.length)cur=Math.max(0,ps.length-1);$('dkTabs').innerHTML=ps.map((p,i)=>'<button type="button" class="chip page-tab'+(i===cur?' on':'')+'" data-i="'+i+'">'+(stackedAt(ps,i)?'&#8595; ':'')+esc(p.name||p.id)+'</button>').join('')+'<button type="button" class="chip page-tab" id="dkAddPage">+ Page</button>';
  $('dkTabs').querySelectorAll('[data-i]').forEach(b=>b.onclick=()=>{cur=+b.dataset.i;sel=-1;more=-1;render();});$('dkAddPage').onclick=()=>{deck.pages.push({id:'p'+Date.now().toString(36),name:'New page',layouts:{landscape:{cols:4,rows:3}},tiles:[]});cur=pages().length-1;setDirty(true);render();};}
/* A page stacked below the one before it (same group) is in its column. */
const stackedAt=(ps,i)=>i>0&&!!ps[i].group&&ps[i].group===ps[i-1].group;
const columnRange=(ps,i)=>{let start=i,end=i+1;while(stackedAt(ps,start))start--;while(end<ps.length&&stackedAt(ps,end))end++;return [start,end];};
const canMovePage=(ps,i,dir)=>{const [start,end]=columnRange(ps,i);return i>start?(dir<0||i+1<end):(dir<0?start>0:end<ps.length);};
function movePage(dir){const ps=pages(),p=ps[cur];if(!p||!canMovePage(ps,cur,dir))return;let ordered=ps.slice();
  if(stackedAt(ps,cur)){const j=cur+dir;[ordered[cur],ordered[j]]=[ordered[j],ordered[cur]];}
  else{const runs=[];ps.forEach((x,i)=>{if(!stackedAt(ps,i))runs.push([]);runs[runs.length-1].push(x);});const i=runs.findIndex(run=>run[0]===p),j=i+dir;[runs[i],runs[j]]=[runs[j],runs[i]];
    /* Preserve every column boundary, including the gap left by the moved column. */
    runs.forEach((run,k)=>{if(!k||!run[0].group||run[0].group!==runs[k-1][0].group)return;if(run.length===1){delete run[0].group;return;}let g=run[0].id,n=0;while(!g||ps.some(x=>x.group===g))g=(run[0].id||'p')+'_'+(++n);run.forEach(x=>x.group=g);});
    ordered=runs.flat();}
  const slots=ps.map(x=>deck.pages.indexOf(x));ordered.forEach((x,i)=>deck.pages[slots[i]]=x);cur=ordered.indexOf(p);setDirty(true);renderTabs();renderPage();}
function renderPage(){const ps=pages();const el=$('dkPage');if(!ps.length){el.innerHTML='<p class="dk-empty">No deck pages. Add one with "+ Page".</p>';return;}
  const p=ps[cur];const [c,r]=grid(p),land=ori==='landscape',at=layout(p),lost=p.tiles.map((t,k)=>k).filter(k=>!at[k]&&!hidT(p.tiles[k]));let cells=0;at.forEach(a=>{if(a)cells+=a.w*a.h/4;});
  let h='<div class="dk-row" style="margin-bottom:12px"><span class="field-label" style="margin:0">Layout</span>'+[['landscape','Landscape'],['portrait','Portrait']].map(o=>'<button type="button" class="chip page-tab'+(o[0]===ori?' on':'')+'" data-o="'+o[0]+'">'+o[1]+'</button>').join('')+'</div>'+whenBar(p);
  h+='<div class="grid-2"><div class="field"><label class="field-label">Page name</label><input type="text" id="dkPName" value="'+esc(p.name||'')+'"></div><div class="field"><label class="field-label">Grid (columns x rows)</label><div class="dk-row"><select id="dkPCols" style="width:auto">'+(land?[2,3,4,5,6]:[2,3,4]).map(n=>'<option'+(n===c?' selected':'')+'>'+n+'</option>').join('')+'</select><span>x</span><select id="dkPRows" style="width:auto">'+(land?[2,3,4]:[2,3,4,5,6]).map(n=>'<option'+(n===r?' selected':'')+'>'+n+'</option>').join('')+'</select><button type="button" class="btn" id="dkPLeft" style="margin-left:auto;padding:6px 9px">Move left</button><button type="button" class="btn" id="dkPRight" style="padding:6px 9px">Move right</button><button type="button" class="btn btn-danger" id="dkPDel">Delete page</button></div></div></div>';
  if(prnOn()&&(deck.printers||[]).length&&(p.printer||p.tiles.some(t=>t.type==='printer')))h+='<div class="field"><label class="field-label">Printer page</label><select id="dkPPrn" style="width:auto">'+opt('','Always shown',!p.printer)+(deck.printers||[]).map(d=>opt(d.id,'Only while '+(d.name||d.id)+' is on',p.printer===d.id)).join('')+'</select></div>';
  h+='<label class="check-row standalone"><input type="checkbox" id="dkPStack"'+(stackedAt(ps,cur)?' checked':'')+(cur===0?' disabled':'')+'><span class="check-box"></span><span class="check-text"><strong>Below the previous page</strong><span class="ct-hint">Stacked pages form one column: swipe up and down between them, left and right to the next column. Move left/right moves a column head with its column, or swaps a stacked page within its column.</span></span></label>';
  let g='';for(let y=0;y<r;y++)for(let x=0;x<c;x++)g+='<div class="dk-cell" style="grid-area:'+(2*y+1)+'/'+(2*x+1)+'/span 2/span 2"></div>';
  const hd=$('dkHeader').checked?30:0;
  h+='<div class="dk-grid'+(land?'':' pt')+'" id="dkGrid" style="grid-template-columns:repeat('+2*c+',minmax(0,1fr));grid-template-rows:repeat('+2*r+',minmax(0,1fr));aspect-ratio:'+(land?480:320)+'/'+((land?320:480)-hd)+'">'+g+p.tiles.map((t,k)=>at[k]?gtile(t,k,at[k]):'').join('')+'<div class="dk-ghost" id="dkGhost" hidden></div></div>';
  if(lost.length)h+='<p class="dk-cap over">No room on the page for these: drag one to a free place, make it smaller or remove it.</p><div class="dk-tray">'+lost.map(k=>gtile(p.tiles[k],k,null)).join('')+'</div>';
  h+='<p class="field-hint">Drag a tile to move it, or onto another tile, which then moves to where the dragged one was; the corner handle resizes. Both go in half-cell steps. Tap a tile to find it in the list.</p>';
  if(sel>=0&&p.tiles[sel]&&ps.length>1)h+='<div class="dk-row"><span class="dk-tsub" style="margin:0">Move the selected tile to</span><select id="dkMvPg" style="width:auto">'+ps.map((q,i)=>i===cur?'':opt(String(i),q.name||q.id,false)).join('')+'</select><button type="button" class="btn" id="dkMv">Move</button></div>';
  h+='<div class="dk-tiles">'+p.tiles.map((t,i)=>{if(t.type==='metric')return metricRow(t,i);if(t.type==='clock')return clockRow(t,i);if(t.type==='printer')return offT(t)?'':printerRow(t,i);const d=domainOf(t.entity);const en=entMap[t.entity];const nm=en&&en.name?en.name:(t.entity||(isHttpSensor(t)?'HTTP sensor':'No entity'));
    const cp=tileCp(t);
    return '<div class="dk-tile'+(i===sel?' sel':'')+'"><button type="button" class="dk-ic'+(cp?' mdi':'')+'" data-ic="'+i+'" title="'+esc(cp?(t.icon||(en&&en.icon)||'automatic'):'Choose an icon')+'" style="background:'+(t.color||COL[d]||'#7DB5FF')+'">'+(cp?String.fromCodePoint(cp):esc((d||'?').slice(0,2).toUpperCase()))+'</button><div class="dk-tmain"><input type="text" data-l="'+i+'" value="'+esc(t.label||'')+'" placeholder="'+esc(nm)+'"><div class="dk-tsub">'+esc(t.entity||(isHttpSensor(t)?'HTTP sensor'+(t.http.path?': '+t.http.path:''):isHttp(t)?'HTTP '+(t.tap.method||'GET')+' '+(t.tap.url||''):isPcCmd(t)?'PC command: '+((PCCMD.find(c=>c[0]===t.tap.cmd)||[0,t.tap.cmd])[1]):'no entity'))+(en&&en.state?' - '+esc(en.state)+(en.unit?' '+esc(en.unit):''):'')+'</div></div>'+
    '<div class="dk-tact">'+(isHttpSensor(t)?'<span>HTTP sensor</span>':'<select data-t="'+i+'"><option value="toggle"'+(t.type==='toggle'?' selected':'')+'>Switch</option><option value="action"'+(t.type==='action'?' selected':'')+'>Button</option><option value="sensor"'+(t.type==='sensor'?' selected':'')+'>Sensor</option>'+(SLIDER.includes(d)||t.type==='slider'?'<option value="slider"'+(t.type==='slider'?' selected':'')+'>Slider</option>':'')+'</select>')+sizeSel(t,i)+(isHttpSensor(t)?'':'<button type="button" class="btn" data-e="'+i+'">Entity</button>')+'<button type="button" class="btn'+(i===more?' on':'')+'" data-mo="'+i+'" title="More options">&#8943;</button><button type="button" class="btn" data-u="'+i+'" title="Move up">&#8593;</button><button type="button" class="btn" data-x="'+i+'" title="Remove">&#215;</button></div>'+(isHttpSensor(t)&&i===more?httpSensorFields(t,i):'')+(i===more?moreRow(t,i):'')+'</div>';}).join('')+'</div>';
  h+='<p class="dk-cap">'+(+cells.toFixed(2))+' of '+(c*r)+' cells used</p><div class="dk-row" style="margin-top:12px"><button type="button" class="btn btn-accent" id="dkAddTile">+ Add tile from Home Assistant</button><button type="button" class="btn" id="dkAddPc">+ Add PC metric</button><button type="button" class="btn" id="dkAddHttp">+ Add HTTP button</button><button type="button" class="btn" id="dkAddPcCmd">+ Add PC control</button><button type="button" class="btn" id="dkAddClock">+ Add clock</button><button type="button" class="btn" id="dkAddHttpSensor">+ Add HTTP sensor</button>'+(prnOn()&&(deck.printers||[]).length?'<button type="button" class="btn" id="dkAddPrinter">+ Add printer tile</button>':'')+'</div>'+pcList()+'<p class="field-hint">Chart and Bar need a wide tile (2x1 on most grids, a 2x0.5 one fits them in a row between the name and the number, a narrower one shows the number), so picking one makes a 1x1 tile 2x1.</p>';
  el.innerHTML=h;
  $('dkPName').oninput=e=>{p.name=e.target.value;setDirty(true);renderTabs();};
  /* Another page: same size, first free place there (its cells are dropped). */
  if($('dkMv'))$('dkMv').onclick=()=>{const q=ps[+$('dkMvPg').value];if(!q)return;const t=p.tiles.splice(sel,1)[0];
    Object.keys(t.pos||{}).forEach(o=>{delete t.pos[o].x;delete t.pos[o].y;if(!Object.keys(t.pos[o]).length)delete t.pos[o];});if(t.pos&&!Object.keys(t.pos).length)delete t.pos;
    let id=t.id,n=2;while(q.tiles.some(x=>x.id===id))id=t.id+'_'+(n++);t.id=id;q.tiles.push(t);sel=-1;more=-1;setDirty(true);renderPage();};
  if($('dkPPrn'))$('dkPPrn').onchange=e=>{if(e.target.value)p.printer=e.target.value;else delete p.printer;setDirty(true);};
  if($('dkAddPrinter'))$('dkAddPrinter').onclick=()=>{const t={id:'pr'+Date.now().toString(36),type:'printer',printer:deck.printers[0].id,show:'hero'};setSize(t,'2x1');p.tiles.push(t);setDirty(true);renderPage();};
  el.querySelectorAll('[data-pt]').forEach(x=>x.onchange=()=>{p.tiles[+x.dataset.pt].printer=x.value;setDirty(true);renderPage();});
  el.querySelectorAll('[data-psh]').forEach(x=>x.onchange=()=>{const t=p.tiles[+x.dataset.psh];t.show=x.value;if(x.value!=='nozzle')delete t.nozzle;if(x.value==='ams')t.unit=t.unit||1;else delete t.unit;setDirty(true);renderPage();});
  el.querySelectorAll('[data-pnz]').forEach(x=>x.onchange=()=>{const t=p.tiles[+x.dataset.pnz];if(x.value==='left')t.nozzle='left';else delete t.nozzle;setDirty(true);renderPage();});
  el.querySelectorAll('[data-pau]').forEach(x=>x.onchange=()=>{p.tiles[+x.dataset.pau].unit=+x.value;setDirty(true);renderPage();});
  const setGrid=(gc,gr)=>{p.layouts=p.layouts||{};p.layouts[ori]={cols:gc,rows:gr};setDirty(true);renderPage();};
  $('dkPCols').onchange=e=>setGrid(+e.target.value,r);
  $('dkPRows').onchange=e=>setGrid(c,+e.target.value);
  el.querySelectorAll('[data-o]').forEach(b=>b.onclick=()=>{ori=b.dataset.o;sel=-1;renderPage();});
  el.querySelectorAll('[data-g]').forEach(n=>n.onpointerdown=e=>gdrag(e,p,at,+n.dataset.g,false));
  el.querySelectorAll('[data-rs]').forEach(n=>n.onpointerdown=e=>gdrag(e,p,at,+n.dataset.rs,true));
  /* The page and the pages stacked under it move together. */
  $('dkPStack').onchange=e=>{const prev=ps[cur-1];if(!prev)return;let end=cur+1;while(end<ps.length&&stackedAt(ps,end))end++;const run=ps.slice(cur,end);
    if(e.target.checked){const g=prev.group||prev.id;prev.group=g;run.forEach(x=>x.group=g);}else if(run.length>1)run.forEach(x=>x.group=p.id);else delete p.group;setDirty(true);renderTabs();renderPage();};
  $('dkPLeft').disabled=!canMovePage(ps,cur,-1);$('dkPRight').disabled=!canMovePage(ps,cur,1);
  $('dkPLeft').onclick=()=>movePage(-1);$('dkPRight').onclick=()=>movePage(1);
  $('dkPDel').onclick=()=>{if(!confirm('Delete the page "'+(p.name||p.id)+'"?'))return;deck.pages.splice(deck.pages.indexOf(p),1);cur=0;setDirty(true);render();};
  el.querySelectorAll('[data-l]').forEach(x=>x.oninput=e=>{const t=p.tiles[+x.dataset.l];const v=e.target.value.trim();if(v)t.label=v;else delete t.label;setDirty(true);});
  el.querySelectorAll('[data-t]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.t];t.type=e.target.value;if(t.type!=='action')delete t.tap;setDirty(true);renderPage();});
  el.querySelectorAll('[data-s]').forEach(x=>x.onchange=e=>{setSize(p.tiles[+x.dataset.s],e.target.value);setDirty(true);renderPage();});
  el.querySelectorAll('[data-e]').forEach(x=>x.onclick=()=>openPicker(+x.dataset.e));
  el.querySelectorAll('[data-ic]').forEach(x=>x.onclick=()=>openIcons(+x.dataset.ic));
  el.querySelectorAll('[data-u]').forEach(x=>x.onclick=()=>{const i=+x.dataset.u;if(i<1)return;const t=p.tiles.splice(i,1)[0];p.tiles.splice(i-1,0,t);sel=-1;more=-1;setDirty(true);renderPage();});
  el.querySelectorAll('[data-x]').forEach(x=>x.onclick=()=>{p.tiles.splice(+x.dataset.x,1);sel=-1;more=-1;setDirty(true);renderPage();});
  el.querySelectorAll('[data-mo]').forEach(x=>x.onclick=()=>{const i=+x.dataset.mo;more=more===i?-1:i;renderPage();});
  el.querySelectorAll('[data-pv]').forEach(b=>b.onclick=()=>{pv[b.dataset.pv]=b.dataset.pvv==='1';sel=-1;renderPage();});
  el.querySelectorAll('[data-wh]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.wh],v=e.target.value;if(!v)delete t.when;else if(v==='entity')t.when={entity:domainOf(t.entity)?t.entity:'',state:'on'};else t.when=v;setDirty(true);renderPage();});
  el.querySelectorAll('[data-we]').forEach(x=>{x.oninput=e=>{p.tiles[+x.dataset.we].when.entity=e.target.value.trim();setDirty(true);};x.onchange=()=>renderPage();});
  const wst=(t,neg,v)=>{t.when.state=(neg?'!':'')+v;setDirty(true);};
  el.querySelectorAll('[data-wn]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.wn],s=String(t.when.state||'');wst(t,e.target.value==='!',s[0]==='!'?s.slice(1):s);renderPage();});
  el.querySelectorAll('[data-ws]').forEach(x=>{x.oninput=e=>{const t=p.tiles[+x.dataset.ws];wst(t,String(t.when.state||'')[0]==='!',e.target.value.trim());};x.onchange=()=>renderPage();});
  el.querySelectorAll('[data-cf]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.cf];if(e.target.value)t.confirm=e.target.value;else delete t.confirm;setDirty(true);});
  el.querySelectorAll('[data-fm]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.fm];if(e.target.value)t.format=e.target.value;else delete t.format;setDirty(true);});
  el.querySelectorAll('[data-sh]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.sh],v=e.target.value.trim();if(v&&Number.isFinite(+v)){t.hours=Math.max(1,Math.min(168,Math.round(+v)));x.value=t.hours;}else delete t.hours;setDirty(true);});
  el.querySelectorAll('[data-th]').forEach(x=>x.oninput=e=>{const t=p.tiles[+x.dataset.th],k=x.dataset.tk,v=e.target.value.trim();if(v&&Number.isFinite(+v))t[k]=+v;else delete t[k];setDirty(true);});
  el.querySelectorAll('[data-co]').forEach(x=>{x.oninput=e=>{p.tiles[+x.dataset.co][x.dataset.ck]=e.target.value.toUpperCase();setDirty(true);};x.onchange=()=>renderPage();});
  el.querySelectorAll('[data-cs]').forEach(x=>x.onclick=()=>{p.tiles[+x.dataset.cs][x.dataset.ck]=x.dataset.v;setDirty(true);renderPage();});
  el.querySelectorAll('[data-ca]').forEach(x=>x.onclick=()=>{delete p.tiles[+x.dataset.ca][x.dataset.ck];setDirty(true);renderPage();});
  $('dkAddTile').onclick=()=>openPicker(-1);
  $('dkAddHttpSensor').onclick=()=>{p.tiles.push({id:'s'+Date.now().toString(36),type:'sensor',http:{url:'',path:'',poll:30}});more=p.tiles.length-1;setDirty(true);renderPage();};
  el.querySelectorAll('[data-hs]').forEach(x=>{const update=()=>{const t=p.tiles[+x.dataset.hs],k=x.dataset.hsk,v=x.value;if(k==='unit'||k==='detail'){if(v)t[k]=v;else delete t[k];}else if(k==='poll'){t.http.poll=Math.max(5,Math.min(3600,Math.round(+v)||30));x.value=t.http.poll;}else t.http[k]=v.trim();setDirty(true);};if(x.dataset.hsk==='poll')x.onchange=update;else x.oninput=update;});
  el.querySelectorAll('[data-hsh]').forEach(x=>x.oninput=()=>{const h=p.tiles[+x.dataset.hsh].http,v=parseHdrs(x.value);if(Object.keys(v).length)h.headers=v;else delete h.headers;setDirty(true);});
  el.querySelectorAll('[data-hst]').forEach(x=>x.onclick=()=>testHttpSensor(p.tiles[+x.dataset.hst],+x.dataset.hst));
  $('dkAddClock').onclick=()=>{const t={id:'k'+Date.now().toString(36),type:'clock'};setSize(t,'2x1');p.tiles.push(t);setDirty(true);renderPage();};
  $('dkAddPcCmd').onclick=()=>{const t={id:'c'+Date.now().toString(36),type:'action'};pcCmdSet(t,'media_play_pause');p.tiles.push(t);more=p.tiles.length-1;setDirty(true);renderPage();};
  $('dkAddHttp').onclick=()=>{p.tiles.push({id:'h'+Date.now().toString(36),type:'action',label:'HTTP',icon:'mdi:web',tap:{action:'http',method:'GET',url:'http://'}});more=p.tiles.length-1;setDirty(true);renderPage();};
  const tapOf=x=>p.tiles[+(x.dataset.ak||x.dataset.hm||x.dataset.hu||x.dataset.hb||x.dataset.hh||x.dataset.hc||x.dataset.ht)];
  el.querySelectorAll('[data-ak]').forEach(x=>x.onchange=e=>{const t=tapOf(x),v=e.target.value;if(v==='keep')return;if(v==='http')t.tap={action:'http',method:'GET',url:'http://'};else if(v==='pc'){delete t.tap;pcCmdSet(t,'media_play_pause');}else delete t.tap;setDirty(true);renderPage();});
  el.querySelectorAll('[data-pcc]').forEach(x=>x.onchange=e=>{pcCmdSet(p.tiles[+x.dataset.pcc],e.target.value);setDirty(true);renderPage();});
  el.querySelectorAll('[data-pck]').forEach(x=>x.oninput=e=>{p.tiles[+x.dataset.pck].tap.keys=e.target.value.trim().toLowerCase().replace(/\s*\+\s*/g,'+');setDirty(true);});
  el.querySelectorAll('[data-pcp]').forEach(x=>x.onchange=e=>{const a=p.tiles[+x.dataset.pcp].tap;if(e.target.value)a.pc=e.target.value;else delete a.pc;setDirty(true);renderPage();});
  el.querySelectorAll('[data-hm]').forEach(x=>x.onchange=e=>{tapOf(x).tap.method=e.target.value;setDirty(true);renderPage();});
  el.querySelectorAll('[data-hu]').forEach(x=>x.oninput=e=>{tapOf(x).tap.url=e.target.value.trim();setDirty(true);});
  el.querySelectorAll('[data-hb]').forEach(x=>x.oninput=e=>{const a=tapOf(x).tap;if(e.target.value)a.body=e.target.value;else delete a.body;setDirty(true);});
  el.querySelectorAll('[data-hh]').forEach(x=>x.oninput=e=>{const a=tapOf(x).tap,o=parseHdrs(e.target.value);if(Object.keys(o).length)a.headers=o;else delete a.headers;setDirty(true);});
  el.querySelectorAll('[data-hc]').forEach(x=>x.oninput=e=>{const a=tapOf(x).tap,v=e.target.value.trim();if(v)a.contentType=v;else delete a.contentType;setDirty(true);});
  el.querySelectorAll('[data-ht]').forEach(x=>x.onclick=()=>testHttp(tapOf(x),+x.dataset.ht));
  $('dkAddPc').onclick=async()=>{if(!pcs.some(pc=>(pc.metrics||[]).length))await loadPc();const pc=pcs.find(pc=>(pc.metrics||[]).length);if(!pc){alert('No PC metrics yet - start the companion app on the PC.');return;}const metrics=pc.metrics;const usedIds=p.tiles.filter(t=>t.type==='metric'&&(t.pc||pcs[0].id)===pc.id).map(t=>t.metric);const m=metrics.find(x=>!usedIds.includes(x.id))||metrics[0];p.tiles.push(Object.assign({id:'m'+Date.now().toString(36),type:'metric',metric:m.id,style:'number'},pcs.length>1?{pc:pc.id}:{}));setDirty(true);renderPage();};
  el.querySelectorAll('[data-pc]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.pc];t.pc=e.target.value;const ml=mlist(t);if(ml.length&&!ml.find(y=>y.id===t.metric))t.metric=ml[0].id;setDirty(true);renderPage();});
  el.querySelectorAll('[data-fpc]').forEach(x=>x.onclick=async e=>{e.preventDefault();const pc=pcs.find(y=>y.id===x.dataset.fpc);if(!pc||!confirm('Forget the PC "'+pc.name+'"? Its tiles show it as unknown until it sends again.'))return;try{await api('/api/pc/forget?id='+encodeURIComponent(pc.id),{method:'POST'});await loadPc();renderPage();}catch(err){alert(err.message);}});
  el.querySelectorAll('[data-m]').forEach(x=>x.onchange=e=>{p.tiles[+x.dataset.m].metric=+e.target.value;setDirty(true);renderPage();});
  el.querySelectorAll('[data-st]').forEach(x=>x.onchange=e=>{const t=p.tiles[+x.dataset.st];if(e.target.value==='number')delete t.style;else{t.style=e.target.value;if(tileSize(t)==='1x1')setSize(t,'2x1');}setDirty(true);renderPage();});}
function render(){renderTabs();renderPage();renderLed();renderPrinters();}

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
  else{const t=p.tiles[pickFor];t.entity=id;t.type=type;delete t.http;delete t.unit;delete t.detail;delete t.label;delete t.sub;delete t.icon;delete t.tap;}
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

$('dkPrnTog').onclick=()=>prnFold($('dkPrnBody').hidden);
$('dkPrnAdd').onclick=()=>{deck.printers=deck.printers||[];let n=deck.printers.length+1;while(deck.printers.some(d=>d.id==='printer'+n))n++;deck.printers.push({id:'printer'+n,name:'Printer '+n,ip:''});setDirty(true);renderPrinters();};
(async function init(){let open=false;try{open=localStorage.getItem('dkPrnOpen')==='1';}catch(e){}prnFold(open);haStatus();icRefresh();printersStatus();setInterval(()=>{if(visible()){haStatus();printersStatus();}},3000);
  await loadPc();ori=ori||'landscape';
  try{const r=await api('/api/deck');deck=r.deck;deck.pages=deck.pages.filter(p=>p.type!=='metrics');$('dkHeader').checked=r.header!==false;render();}catch(e){$('dkPage').innerHTML='<p class="dk-empty">'+esc(e.message)+'</p>';}
  try{await loadEnts();}catch(e){}})();
})();)JS";

#endif  // DECK_WEB_H
