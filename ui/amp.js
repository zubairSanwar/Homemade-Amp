/* ======================================================================
   Live control surface for the C++ amp. Contract is ui/SPEC.md.

   Everything here is a GET against http://127.0.0.1:8080, served by the amp
   itself (`make gui`), so same-origin - no CORS, no base URL needed.

     /state    once on load, to draw the knobs where the amp actually is
     /knob     name + float, on every knob move (throttled)
     /switch   name + value=0|1 for states, name alone for one-shot actions
     /results  [peakIn, peakOut] polled for the meters
     /quit     shuts the amp down (the power switch)
   ====================================================================== */

/* ---------- parameter tables ----------
   min/max/def are the real server-side clamps from src/amp/amp.cpp. The server
   clamps anyway, so these only exist to stop the UI showing a value the amp is
   not using.

   `curve` warps the knob's visual travel: displayed = min + (max-min)*frac^curve.
   Needed where the range is wide but the default sits near the bottom - gain
   defaults to 1.0 out of 20, which on a linear knob looks stuck at zero. */
const PANEL = [
  { id:'gain', name:'Gain', pilot:false,
    params:[
      { name:'adj_gain',   label:'Gain',   min:0, max:20, def:1,     step:.1,   dec:1, curve:2.5 },
      { name:'adj_smooth', label:'Smooth', min:0, max:1,  def:0.002, step:.001, dec:3, curve:3 }
    ] },

  { id:'distortion', name:'Distortion', sw:'distortion', on:false,
    params:[
      { name:'adj_drive',      label:'Drive', min:1, max:30, def:1, step:.1,  dec:1, curve:2 },
      { name:'adj_distortion', label:'Level', min:0, max:1,  def:1, step:.01, pct:true }
    ] },

  { id:'eq', name:'Tone', sw:'eq', on:true,
    params:[
      { name:'adj_eq_low',  label:'Bass',   min:-15, max:15, def:0, step:.5, dec:1, unit:' dB', signed:true },
      { name:'adj_eq_mid',  label:'Mid',    min:-15, max:15, def:0, step:.5, dec:1, unit:' dB', signed:true },
      { name:'adj_eq_high', label:'Treble', min:-15, max:15, def:0, step:.5, dec:1, unit:' dB', signed:true }
    ] },

  { id:'delay', name:'Delay', sw:'delay', on:false,
    params:[
      { name:'adj_delay',      label:'Time', min:.1, max:2,   def:.3, step:.01, dec:2, unit:' s' },
      { name:'adj_delay_fade', label:'Fade', min:0,  max:.95, def:.6, step:.01, dec:2 }
    ],
    buttons:[{ name:'delay_reset', label:'Clear' }] },

  { id:'reverb', name:'Reverb', sw:'reverb', on:false,
    params:[
      { name:'adj_room_size', label:'Room', min:0, max:.9,  def:.5, step:.01, pct:true },
      { name:'adj_rev_mix',   label:'Mix',  min:0, max:1,   def:.3, step:.01, pct:true },
      { name:'adj_rev_damp',  label:'Damp', min:0, max:.95, def:.4, step:.01, pct:true }
    ] },

  /* speaker sim: a switch, nothing to dial */
  { id:'cabinet', name:'Cabinet', sub:'Speaker sim', sw:'cabinet', on:true, params:[], slot:'tail', jewel:true },

  /* master has no bypass - the jewel is just the pilot light */
  { id:'master', name:'Master', pilot:true,
    params:[{ name:'adj_master', label:'Volume', min:0, max:1, def:1, step:.01, pct:true }] }
];

const PEDALS = [
  { id:'gate', name:'Noise gate', sw:'gate', on:true, enamel:['#e7dcc0','#d6c8a4','#b8a97f','#39332a'],
    params:[{ name:'adj_gate', label:'Threshold', min:-90, max:0, def:-50, step:1, dec:0, unit:' dB' }] },

  { id:'wah', name:'Wah', sw:'wah', on:false, enamel:['#8e9aa0','#6f7c84','#4e5a62','#17201f'],
    params:[
      { name:'wah_sensitivity', label:'Sens', min:.5, max:30, def:6, step:.1, dec:1, curve:2 },
      { name:'wah_resonance',   label:'Reso', min:1,  max:8,  def:3, step:.1, dec:1 },
      { name:'wah_mix',         label:'Mix',  min:0,  max:1,  def:1, step:.01, pct:true }
    ],
    note:'Sens above ~6 pins the filter at the top of its sweep, which gives less movement, not more.' },

  { id:'tremolo', name:'Tremolo', sw:'tremolo', on:false, enamel:['#8fc3b0','#6ba894','#4a8372','#17332b'],
    params:[
      { name:'tremolo_rate',  label:'Rate',  min:.1, max:20, def:5,  step:.1,  dec:1, unit:' Hz', curve:1.6 },
      { name:'tremolo_depth', label:'Depth', min:0,  max:1,  def:.8, step:.01, pct:true }
    ],
    note:'Depth below ~0.5 is barely audible - 0.5 is only about 6 dB of swing.' },

  /* Momentary, not latched: the amp cannot report looper state yet (recording /
     overdub / loopLen are audio-thread-owned and not in /state), and at 60 s the
     audio thread closes the loop by itself. A lit button would lie. See SPEC.md. */
  { id:'loop', name:'Looper', sw:'loop', on:false, enamel:['#d4725c','#b9533c','#8e3a27','#2e0f09'],
    tempo:true,
    buttons:[
      { name:'loop_tap',     label:'Tap',     tempo:true },
      { name:'loop_record',  label:'Record', key:' ' },
      { name:'loop_overdub', label:'Overdub' },
      { name:'loop_reset',   label:'Erase'   }
    ],
    note:'Tap four times to set the tempo, then Record counts you in for 3 beats and '
       + 'snaps the loop to whole beats. Without a tempo it records the moment you press. '
       + 'The space bar presses Record, so both hands can stay on the guitar. '
       + 'No lamp: the amp does not report looper state yet.' }
];

/* ---------- local mirror of every value ---------- */
const state = {};
const knobs = {};                      // name -> { paint }
[...PANEL, ...PEDALS].forEach(fx=>{
  state[fx.id] = { on:true, params:{} };
  (fx.params||[]).forEach(p => state[fx.id].params[p.name] = p.def);
});

/* ---------- transport ---------- */
let online = false, stopped = false;

const dotEl = document.getElementById('dot');
const msgEl = document.getElementById('lastMsg');

function bus(text, isError){
  msgEl.textContent = text;
  msgEl.classList.toggle('err', !!isError);
}
function setOnline(up){
  if(up === online) return;
  online = up;
  dotEl.classList.toggle('ok', up);
  document.querySelector('.rig').classList.toggle('offline', !up);
}

// Returns the response body on success, null on any failure. The amp replies to a
// bad request with 4xx AND a plain-text reason, so show that rather than a status.
async function api(path){
  if(stopped) return null;
  try{
    const res  = await fetch(path);
    const body = (await res.text()).trim();
    setOnline(true);
    if(!res.ok){ bus(body || ('HTTP ' + res.status), true); return null; }
    return body;
  }catch(err){
    setOnline(false);
    bus('no connection to the amp', true);
    return null;
  }
}

/* Knob moves fire on every pointermove. Throttle per parameter, but always send a
   trailing value so wherever you let go of the knob is where the amp ends up. */
/* false = send once when you let go of the knob.
   true  = send continuously while turning, so you hear it move as you dial.
   Live is the usual choice for audio and costs nothing - every setter is a
   lock-free atomic store, and gain/drive/wah are smoothed in C++ so a moving
   knob cannot click. Flip this and keep whichever sounds right. */
const SEND_WHILE_TURNING = false;

const throttleAt = new Map(), throttleVal = new Map();
const SEND_MS = 40;

function sendKnob(name, value){
  const now = performance.now(), last = throttleAt.get(name) || 0;
  if(now - last >= SEND_MS){
    throttleAt.set(name, now);
    api(`/knob?name=${name}&value=${value.toFixed(4)}`)
      .then(ok => { if(ok !== null) bus(`${name} = ${value.toFixed(3)}`); });
  }else{
    if(!throttleVal.has(name)){
      setTimeout(()=>{
        const v = throttleVal.get(name); throttleVal.delete(name);
        throttleAt.set(name, performance.now());
        api(`/knob?name=${name}&value=${v.toFixed(4)}`)
          .then(ok => { if(ok !== null) bus(`${name} = ${v.toFixed(3)}`); });
      }, SEND_MS - (now - last));
    }
    throttleVal.set(name, value);
  }
}

function sendSwitch(name, on){
  api(`/switch?name=${name}&value=${on ? 1 : 0}`)
    .then(ok => { if(ok !== null) bus(`${name} = ${on ? 'on' : 'off'}`); });
}
function sendAction(name){
  api(`/switch?name=${name}`)
    .then(ok => { if(ok !== null) bus(`${name}()`); });
}

/* ---------- formatting + knob taper ---------- */
const fmt = (p,v) => p.pct
  ? Math.round(v*100) + '%'
  : (p.signed && v > 0 ? '+' : '') + v.toFixed(p.dec ?? 1) + (p.unit || '');

const toFrac   = (p,v) => Math.pow(Math.min(1, Math.max(0, (v-p.min)/(p.max-p.min))), 1/(p.curve||1));
const fromFrac = (p,f) => p.min + Math.pow(Math.min(1, Math.max(0, f)), (p.curve||1)) * (p.max-p.min);

/* ---------- knob ----------
   Tick ring and pointer share one convention: 0 = 12 o'clock, -135 deg = 7:30,
   +135 deg = 4:30. The ring opens at the bottom and the label sits in the gap. */
function makeKnob(p, get, set, send, variant){
  const wrap = document.createElement('div'); wrap.className='kw';
  const k = document.createElement('div');
  k.className = 'knob' + (variant==='chicken' ? ' chicken' : '');
  k.tabIndex = 0; k.setAttribute('role','slider'); k.setAttribute('aria-label',p.label);

  const ticks = document.createElement('div'); ticks.className='ticks';
  const N = 11, R = 29;
  for(let i=0;i<N;i++){
    const a = -135 + (270*i)/(N-1);
    const t = document.createElement('i');
    if(i===0 || i===N-1) t.className='maj';
    t.style.transform = `rotate(${a}deg) translateY(-${R}px)`;
    ticks.appendChild(t);
  }
  const arc  = document.createElement('div'); arc.className='arc';
  const body = document.createElement('div'); body.className='body';
  const gloss= document.createElement('div'); gloss.className='gloss';
  const ptr  = document.createElement('div'); ptr.className='ptr';
  const head = document.createElement('div'); head.className='head';
  k.append(ticks, arc, body, gloss, ptr, head);

  const lab = document.createElement('div'); lab.className='k-lab'; lab.textContent = p.label;
  // The readout doubles as a text box, so a value can be typed instead of dialled.
  const val = document.createElement('input');
  val.className='k-val'; val.type='text'; val.inputMode='decimal'; val.spellcheck=false;
  val.setAttribute('aria-label', p.label + ' value');
  wrap.append(k, lab, val);

  const clamp = v => Math.min(p.max, Math.max(p.min, v));
  const quant = v => clamp(parseFloat((Math.round((v-p.min)/p.step)*p.step + p.min).toFixed(6)));

  function paint(){
    const v = get();
    k.style.setProperty('--frac', toFrac(p,v));
    if(document.activeElement !== val) val.value = fmt(p,v);   // don't fight the typist
    k.setAttribute('aria-valuemin',p.min); k.setAttribute('aria-valuemax',p.max);
    k.setAttribute('aria-valuenow',v); k.setAttribute('aria-valuetext',`${p.label} ${fmt(p,v)}`);
  }
  // sendNow=false updates the UI only; the caller sends later (drag release).
  function commit(v, sendNow = true){
    v = quant(v);
    if(v === get()) return false;
    set(v); paint();
    if(sendNow) send?.(v);
    return true;
  }

  /* ---- typed entry ----
     Edited in the unit shown: percent knobs take 0-100, the rest take their own
     unit. Out of range snaps to the nearer limit rather than being refused, and
     anything unparseable just puts the current value back. */
  const toEdit   = v => p.pct ? v*100 : v;
  const fromEdit = x => p.pct ? x/100 : x;

  function applyTyped(){
    const raw = val.value.trim().replace(/[%\s]/g,'').replace(/[a-zA-Z]+$/,'');
    const num = parseFloat(raw);
    if(!isFinite(num)){ paint(); return; }          // gibberish: put it back
    const wanted = fromEdit(num);
    const limited = clamp(wanted);                  // past a limit: snap to it
    if(limited !== wanted) bus(`${p.label} limited to ${fmt(p,limited)}`);
    if(!commit(limited)) paint();                   // unchanged: just reformat
  }

  val.addEventListener('focus', ()=>{
    if(val.disabled) return;
    val.value = String(parseFloat(toEdit(get()).toFixed(4)));   // raw number to edit
    val.select();
  });
  val.addEventListener('keydown', e=>{
    if(e.key === 'Enter'){ e.preventDefault(); applyTyped(); val.blur(); }
    else if(e.key === 'Escape'){ e.preventDefault(); paint(); val.blur(); }
    e.stopPropagation();                             // keep it off the knob handler
  });
  val.addEventListener('blur', applyTyped);

  // Drag in knob-travel space, not value space, so a tapered knob still feels even.
  let drag=false, lastY=0, accF=0, moved=false;
  k.addEventListener('pointerdown', e=>{
    if(k.getAttribute('aria-disabled')==='true') return;
    drag=true; moved=false; lastY=e.clientY; accF=toFrac(p,get());
    k.setPointerCapture(e.pointerId); e.preventDefault();
  });
  k.addEventListener('pointermove', e=>{
    if(!drag) return;
    accF = Math.min(1, Math.max(0, accF + ((lastY-e.clientY)/165)*(e.shiftKey?.25:1)));
    lastY = e.clientY;
    if(commit(fromFrac(p,accF), SEND_WHILE_TURNING)) moved = true;
  });
  // one send on release, so the amp lands exactly where the knob was let go
  const end = ()=>{
    if(drag && moved && !SEND_WHILE_TURNING) send?.(get());
    drag=false; moved=false;
  };
  k.addEventListener('pointerup',end); k.addEventListener('pointercancel',end);
  k.addEventListener('wheel', e=>{
    if(k.getAttribute('aria-disabled')==='true') return;
    e.preventDefault(); commit(get() + (e.deltaY<0?1:-1)*p.step*(e.shiftKey?1:4));
  }, {passive:false});
  k.addEventListener('dblclick', ()=> commit(p.def));
  k.addEventListener('keydown', e=>{
    const big=(p.max-p.min)/10;
    const m={ArrowUp:p.step,ArrowRight:p.step,ArrowDown:-p.step,ArrowLeft:-p.step,PageUp:big,PageDown:-big};
    if(e.key in m){ e.preventDefault(); commit(get()+m[e.key]*(e.shiftKey?.25:1)); }
    else if(e.key==='Home'){ e.preventDefault(); commit(p.def); }
  });
  paint();
  return { el:wrap, knob:k, inp:val, paint };
}

/* ---------- meters ---------- */
function makeMeter(root, segs=20, floor=-60, ceil=6){
  const ladder = root.querySelector('[data-ladder]'), num = root.querySelector('.num');
  const cells=[];
  for(let i=0;i<segs;i++){
    const f=i/(segs-1), d=document.createElement('div');
    d.className='seg '+(f>.9?'r':f>.72?'a':'g');
    ladder.appendChild(d); cells.push(d);
  }
  let peak=-100, peakT=0, clipT=0;
  function set(db){
    db=Math.max(-100,db);
    const now=performance.now();
    const lit = db<=floor ? 0 : Math.round(((db-floor)/(ceil-floor))*segs);
    if(db>peak){ peak=db; peakT=now; } else if(now-peakT>900) peak-=.55;
    const pIdx = peak<=floor ? -1 : Math.min(segs-1, Math.round(((peak-floor)/(ceil-floor))*segs)-1);
    for(let i=0;i<segs;i++){
      const on=i<lit;
      cells[i].classList.toggle('on',on);
      cells[i].classList.toggle('peak', i===pIdx && !on);
    }
    if(db>-0.1) clipT=now;                       // output is hard-clamped at 1.0
    root.classList.toggle('clipped', now-clipT<1200);
    num.textContent = db<=-99 ? ' -inf dB ' : (db>0?'+':'') + db.toFixed(1) + ' dB';
  }
  set(-100);
  return { set };
}
const meterIn  = makeMeter(document.getElementById('meterIn'));
const meterOut = makeMeter(document.getElementById('meterOut'));

/* ---------- input channel picker ----------
   The amp meters every input socket before the gate and the effects, so these
   bars move whatever the amp is set to. Watch which one answers when you strum
   and click it - that replaces guessing the channel by listening at startup. */
const inputsEl    = document.getElementById('inputs');
const inputRowsEl = document.getElementById('inputRows');
let chMeters = [], chRows = [];

function buildChannels(count, selected){
  inputRowsEl.textContent = '';
  chMeters = []; chRows = [];
  if(count <= 1){ inputsEl.hidden = true; return; }   // nothing to choose between
  inputsEl.hidden = false;

  for(let c = 0; c < count; c++){
    const row  = document.createElement('div'); row.className = 'in-row';
    const pick = document.createElement('button');
    pick.className='in-pick'; pick.type='button'; pick.textContent = 'CH ' + c;
    const lad  = document.createElement('div'); lad.className='ladder'; lad.setAttribute('data-ladder','');
    const num  = document.createElement('span'); num.className='num'; num.textContent='-99.9 dB';
    row.append(pick, lad, num);
    inputRowsEl.appendChild(row);
    chRows.push(row);
    chMeters.push(makeMeter(row, 14, -60, 0));        // input cannot exceed 0 dBFS
    pick.addEventListener('click', ()=> selectChannel(c));
  }
  markChannel(selected);
}

function markChannel(c){
  chRows.forEach((r,i)=> r.classList.toggle('sel', i === c));
}

function selectChannel(c){
  markChannel(c);                                     // move the highlight at once
  api(`/input?channel=${c}`).then(ok => { if(ok !== null) bus(`input channel ${c}`); });
}

/* ---------- faceplate ---------- */
const faceMain = document.getElementById('faceMain');
const setters = {};                     // switch name -> fn(bool) to update the UI
const hotkeys = {};                     // key -> the button it presses

PANEL.forEach(fx=>{
  const grp = document.createElement('section'); grp.className='grp';
  const row = document.createElement('div'); row.className='grp-knobs';
  const myKnobs=[];

  (fx.params||[]).forEach(p=>{
    const kn = makeKnob(p,
      ()=> state[fx.id].params[p.name],
      v => state[fx.id].params[p.name] = v,
      v => sendKnob(p.name, v),
      'porcelain');
    row.appendChild(kn.el); myKnobs.push(kn); knobs[p.name] = kn;
  });

  (fx.buttons||[]).forEach(b=>{
    const col=document.createElement('div'); col.className='btn-col';
    const btn=document.createElement('button'); btn.className='pb'; btn.type='button'; btn.textContent=b.label;
    btn.addEventListener('click',()=>{
      btn.classList.add('held'); setTimeout(()=>btn.classList.remove('held'),140);
      sendAction(b.name);
    });
    col.appendChild(btn); row.appendChild(col);
  });

  const toggleOnly = !(fx.params||[]).length && !(fx.buttons||[]).length;
  if(toggleOnly) grp.classList.add('toggle-only');

  const foot = document.createElement('div'); foot.className='grp-foot';
  const name = document.createElement('span'); name.className='grp-name'; name.textContent=fx.name;

  let lamp=null, sw=null;
  if(fx.pilot){
    lamp = document.createElement('span'); lamp.className='jewel on'; lamp.title='Power';
  }else if(fx.sw){
    lamp = document.createElement('span'); lamp.className = fx.jewel ? 'jewel' : 'lamp';
    sw = document.createElement('button'); sw.className='sw';
    sw.setAttribute('role','switch'); sw.setAttribute('aria-label', fx.name+' on');
  }

  if(toggleOnly){
    const swRow = document.createElement('div'); swRow.className='sw-row';
    if(sw) swRow.append(sw); if(lamp) swRow.append(lamp);
    foot.append(swRow, name);
    if(fx.sub){
      const sub = document.createElement('span'); sub.className='grp-sub'; sub.textContent = fx.sub;
      foot.appendChild(sub);
    }
  }else{
    foot.appendChild(name);
    if(sw) foot.appendChild(sw);
    if(lamp) foot.appendChild(lamp);
  }
  grp.append(row, foot);
  (fx.slot === 'tail' ? document.getElementById('faceTail') : faceMain).appendChild(grp);

  // Updates the UI only. Sending is done by the click handler, so applying /state
  // at load cannot bounce a request straight back at the amp.
  function render(on){
    state[fx.id].on = on;
    grp.classList.toggle('off', !on);
    sw?.setAttribute('aria-checked', String(on));
    if(sw) lamp?.classList.toggle('on', on);
    myKnobs.forEach(k => { k.knob.setAttribute('aria-disabled', String(!on)); k.inp.disabled = !on; });
  }
  if(fx.sw) setters[fx.sw] = render;
  render(fx.sw ? !!fx.on : true);          // /state overwrites this once connected

  sw?.addEventListener('click', ()=>{
    const on = !state[fx.id].on;
    render(on); sendSwitch(fx.sw, on);
  });
});

/* ---------- pedalboard ---------- */
const board = document.getElementById('board');
PEDALS.forEach(fx=>{
  const el = document.createElement('article'); el.className='ped';
  const [hi, mid, lo, ink] = fx.enamel;
  el.style.setProperty('--enamel-hi',hi); el.style.setProperty('--enamel',mid);
  el.style.setProperty('--enamel-lo',lo); el.style.setProperty('--enamel-ink',ink);

  const head = document.createElement('button'); head.className='ped-head'; head.type='button';
  const nm   = document.createElement('span'); nm.className='ped-name'; nm.textContent=fx.name;
  const lamp = document.createElement('span'); lamp.className='lamp';
  const sw   = document.createElement('button'); sw.className='sw';
  sw.setAttribute('role','switch'); sw.setAttribute('aria-label', fx.name+' on');
  const chev = document.createElement('span'); chev.className='chev';
  head.append(nm, lamp, sw, chev);

  const bodyEl = document.createElement('div'); bodyEl.className='ped-body';
  const clip   = document.createElement('div');
  const plate  = document.createElement('div'); plate.className='ped-plate';
  clip.appendChild(plate); bodyEl.appendChild(clip);

  const myKnobs=[];
  (fx.params||[]).forEach(p=>{
    const kn = makeKnob(p,
      ()=> state[fx.id].params[p.name],
      v => state[fx.id].params[p.name] = v,
      v => sendKnob(p.name, v),
      'chicken');
    plate.appendChild(kn.el); myKnobs.push(kn); knobs[p.name] = kn;
  });

  if(fx.buttons){
    const col=document.createElement('div'); col.className='btn-col';
    fx.buttons.forEach(b=>{
      const btn=document.createElement('button'); btn.className='pb'; btn.type='button';
      btn.textContent = b.label;
      if(b.key){ hotkeys[b.key] = btn; btn.title = b.key === ' ' ? 'Space bar' : b.key; }
      btn.addEventListener('click',()=>{
        btn.classList.add('held'); setTimeout(()=>btn.classList.remove('held'),140);
        sendAction(b.name);
        if(b.tempo) refreshTempo();      // the amp works the BPM out, so read it back
      });
      col.appendChild(btn);
    });
    plate.appendChild(col);
  }

  // the tapped tempo lives on the amp - only ask for it after a tap
  if(fx.tempo){
    const t = document.createElement('div'); t.className='ped-tempo';
    t.innerHTML = 'tempo <b id="loopBpm">--</b> BPM';
    plate.appendChild(t);
  }

  if(fx.note){
    const n = document.createElement('p'); n.className='ped-note'; n.textContent = fx.note;
    plate.appendChild(n);
  }

  function render(on){
    state[fx.id].on = on;
    el.classList.toggle('bypassed', !on);
    el.classList.toggle('open', on);
    head.setAttribute('aria-expanded', String(on));
    sw.setAttribute('aria-checked', String(on));
    lamp.className = 'lamp' + (on ? ' on' : '');
    myKnobs.forEach(k => { k.knob.setAttribute('aria-disabled', String(!on)); k.inp.disabled = !on; });
  }
  setters[fx.sw] = render;
  render(!!fx.on);                         // /state overwrites this once connected

  head.addEventListener('click', ()=>{
    const open = !el.classList.contains('open');
    el.classList.toggle('open', open);
    head.setAttribute('aria-expanded', String(open));
  });
  sw.addEventListener('click', e=>{
    e.stopPropagation();
    const on = !state[fx.id].on;
    render(on); sendSwitch(fx.sw, on);
  });

  el.append(head, bodyEl);
  board.appendChild(el);
});

/* ---------- tapped tempo ----------
   The amp times the taps by counting samples on the audio thread, so the browser
   never needs a clock of its own - it just asks what the amp worked out. */
async function refreshTempo(){
  const body = await api('/state');
  if(body === null) return;
  const el = document.getElementById('loopBpm');
  if(!el) return;
  try{
    const bpm = JSON.parse(body).loop_bpm;
    el.textContent = (bpm && bpm > 0) ? bpm.toFixed(1) : '--';
  }catch(err){ /* leave the last reading up */ }
}

/* ---------- keyboard ----------
   Record is already a toggle on the amp - the first press starts the loop, the
   second closes it - so the space bar just presses the same button. */
document.addEventListener('keydown', e=>{
  const btn = hotkeys[e.key];
  if(!btn || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
  const el = document.activeElement;
  if(el && (el.tagName === 'INPUT' || el.isContentEditable)) return;   // let typing through
  e.preventDefault();
  if(el instanceof HTMLButtonElement) el.blur();   // or the browser presses that one too
  btn.click();
});

/* ---------- fold all ---------- */
const ta = document.getElementById('toggleAll');
ta.addEventListener('click', ()=>{
  const anyClosed = [...board.children].some(c=>!c.classList.contains('open'));
  [...board.children].forEach(c=>{
    c.classList.toggle('open', anyClosed);
    c.querySelector('.ped-head').setAttribute('aria-expanded', String(anyClosed));
  });
  ta.textContent = anyClosed ? 'Fold all' : 'Unfold all';
});

/* ---------- power = /quit ----------
   Press once to arm, again within 4 s to shut the amp down. There is no way back:
   /quit ends the process, so the page cannot restart it. */
const rigEl  = document.querySelector('.rig');
const pwrBtn = document.getElementById('pwrBtn');
const pwrLab = document.getElementById('pwrLab');
let armT = null;

function disarm(){
  clearTimeout(armT); armT = null;
  pwrBtn.classList.remove('confirm');
  pwrLab.classList.remove('warn');
  pwrLab.textContent = 'Power';
}
pwrBtn.addEventListener('click', async ()=>{
  if(stopped) return;
  if(!armT){
    pwrBtn.classList.add('confirm');
    pwrLab.classList.add('warn');
    pwrLab.textContent = 'Shut down?';
    armT = setTimeout(disarm, 4000);
    return;
  }
  clearTimeout(armT); armT = null;
  await api('/quit');
  stopped = true;
  rigEl.classList.add('dead');
  pwrBtn.classList.remove('on','confirm');
  pwrBtn.setAttribute('aria-pressed','false');
  pwrLab.classList.remove('warn');
  pwrLab.textContent = 'Stopped';
  meterIn.set(-100); meterOut.set(-100);
  setOnline(false);
  bus('amp stopped - restart it with `make gui`');
});

/* ---------- apply /state ---------- */
function applyState(s){
  for(const name in knobs){
    if(typeof s[name] === 'number'){
      const kn = knobs[name];
      // find the owning effect and write through, so the local mirror stays right
      for(const fx of [...PANEL, ...PEDALS]){
        if(fx.params && fx.params.some(p => p.name === name)){
          state[fx.id].params[name] = s[name];
          break;
        }
      }
      kn.paint();
    }
  }
  for(const name in setters){
    if(typeof s[name] === 'boolean') setters[name](s[name]);
  }
  const ser = document.getElementById('faceSer');
  if(s.sample_rate){
    ser.textContent = `${s.sample_rate} Hz / IN ${s.input_channel} OF ${s.input_channels}`;
  }
  if(typeof s.input_channels === 'number') buildChannels(s.input_channels, s.input_channel);
  const bpmEl = document.getElementById('loopBpm');
  if(bpmEl && typeof s.loop_bpm === 'number')
    bpmEl.textContent = s.loop_bpm > 0 ? s.loop_bpm.toFixed(1) : '--';
}

/* ---------- connect ----------
   Startup blocks for roughly 7 s: a 6 s guitar-channel detect, then the server
   binds. So the first fetch is expected to fail - retry rather than give up. */
async function connect(){
  for(let attempt=0; attempt<60; attempt++){
    try{
      const res = await fetch('/state');
      if(res.ok){
        applyState(await res.json());
        setOnline(true);
        bus('connected');
        return true;
      }
    }catch(err){ /* not listening yet */ }
    bus('waiting for the amp... (startup takes about 7 s)');
    await new Promise(r => setTimeout(r, 500));
  }
  bus('could not reach the amp - is it running with `make gui`?', true);
  return false;
}

/* ---------- meter polling ----------
   setTimeout chain, not setInterval, so a slow reply cannot pile requests up.
   /results resets the peaks when read, so only this one loop may poll it. */
const lin2db = v => (v > 0 ? 20*Math.log10(v) : -100);

async function pollMeters(){
  if(!stopped){
    try{
      const res = await fetch('/results');
      if(res.ok){
        // [selectedIn, out, ch0, ch1, ...]
        const [pin, pout, ...chans] = await res.json();
        setOnline(true);
        meterIn.set(lin2db(pin));
        meterOut.set(lin2db(pout));
        for(let c = 0; c < chMeters.length && c < chans.length; c++)
          chMeters[c].set(lin2db(chans[c]));
      }
    }catch(err){
      setOnline(false);
      meterIn.set(-100); meterOut.set(-100);
      chMeters.forEach(m => m.set(-100));
    }
  }
  setTimeout(pollMeters, 50);
}

connect().then(pollMeters);
