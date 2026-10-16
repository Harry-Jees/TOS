/* SandboxOS console: mirrors the virtual screen and drives it. */
(() => {
  const canvas = document.getElementById('screen');
  const ctx = canvas.getContext('2d');
  const feed = document.getElementById('feed');
  const shots = document.getElementById('shots');
  const sys = document.getElementById('sys');
  const wins = document.getElementById('wins');
  const meta = document.getElementById('meta');
  const agentState = document.getElementById('agent-state');
  const brain = document.getElementById('brain');
  const clock = document.getElementById('clock');
  const stepHint = document.getElementById('step-hint');
  const shellOut = document.getElementById('shell-out');

  let since = 0;
  let lastScreenSeq = -1;
  let screenW = 960, screenH = 600;
  let running = false;
  const shotCache = new Set();

  async function post(path, body) {
    const res = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {}),
    });
    try { return await res.json(); } catch (e) { return { ok: false, error: String(e) }; }
  }

  /* ---------- screen rendering (double-buffered: never blanks the canvas) ---------- */
  ctx.imageSmoothingEnabled = false;
  const img = new Image();
  let liveUrl = null;
  let queued = null;
  img.onload = () => {
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
    if (liveUrl && liveUrl !== img.src) URL.revokeObjectURL(liveUrl);
    liveUrl = img.src;
    if (queued) { const next = queued; queued = null; hiRes(next); }
  };
  function hiRes(url) { img.src = url; }
  async function refreshScreen() {
    try {
      const res = await fetch('/screen.png?t=' + Date.now(), { cache: 'no-store' });
      const blob = await res.blob();
      const url = URL.createObjectURL(blob);
      if (img.complete && !img.src.endsWith(url)) hiRes(url);
      else if (!img.complete) { queued = url; }
      else hiRes(url);
    } catch (e) { /* ignore */ }
  }

  /* ---------- input forwarding ---------- */
  function coords(e) {
    const r = canvas.getBoundingClientRect();
    return {
      x: Math.round((e.clientX - r.left) * (screenW / r.width)),
      y: Math.round((e.clientY - r.top) * (screenH / r.height)),
    };
  }
  function sendMouse(action, e, extra) {
    const { x, y } = coords(e);
    post('/input', { type: 'mouse', action, x, y, button: (e.button === 2 ? 'right' : 'left'), ...(extra || {}) });
  }
  canvas.addEventListener('mousedown', e => { e.preventDefault(); canvas.focus(); sendMouse('down', e); });
  canvas.addEventListener('mouseup', e => sendMouse('up', e));
  canvas.addEventListener('contextmenu', e => e.preventDefault());
  canvas.addEventListener('dblclick', e => sendMouse('double_click', e));
  let lastMove = 0;
  canvas.addEventListener('mousemove', e => {
    const now = performance.now();
    if (now - lastMove < 40) return;
    lastMove = now;
    sendMouse('move', e);
  });
  canvas.addEventListener('wheel', e => {
    e.preventDefault();
    sendMouse('wheel', e, { delta: e.deltaY });
  }, { passive: false });

  const NAMED = new Set(['Enter', 'Backspace', 'Tab', 'Escape', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
    'PageUp', 'PageDown', 'Home', 'End', 'Delete']);
  canvas.addEventListener('keydown', e => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (NAMED.has(e.key)) {
      e.preventDefault();
      const map = { ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right', Delete: 'Backspace' };
      post('/input', { type: 'key', key: map[e.key] || e.key });
      return;
    }
    if (e.key.length === 1) {
      e.preventDefault();
      post('/input', { type: 'key', key: e.key, text: e.key });
    }
  });

  /* ---------- state polling ---------- */
  function renderFeed(events) {
    for (const ev of events) {
      const div = document.createElement('div');
      div.className = 'ev ' + ev.kind;
      const t = new Date(ev.ts * 1000).toLocaleTimeString();
      div.innerHTML = `<span class="k">${t} ${ev.kind}</span>`;
      div.appendChild(document.createTextNode(ev.text));
      feed.appendChild(div);
    }
    if (events.length) {
      while (feed.children.length > 400) feed.removeChild(feed.firstChild);
      feed.scrollTop = feed.scrollHeight;
    }
  }

  function renderShots(list) {
    for (const s of list) {
      if (shotCache.has(s.name)) continue;
      shotCache.add(s.name);
      const a = document.createElement('a');
      a.href = s.url; a.target = '_blank';
      a.innerHTML = `<img src="${s.url}" loading="lazy" /><div class="cap">${s.name}</div>`;
      shots.prepend(a);
      while (shots.children.length > 24) shots.removeChild(shots.lastChild);
    }
  }

  function renderSys(info, openWindows) {
    const d = info.disk, a = info.agent;
    const list = openWindows || [];
    sys.innerHTML = `
      <div><span>agent</span><span>${a.state} (${a.steps} steps)</span></div>
      <div><span>uptime</span><span>${info.uptime_human}</span></div>
      <div><span>windows</span><span>${info.windows}</span></div>
      <div><span>processes</span><span>${info.processes}</span></div>
      <div><span>files / dirs</span><span>${d.files} / ${d.dirs}</span></div>
      <div><span>disk</span><span>${d.used_bytes} / ${d.quota_bytes} B</span></div>`;
    document.getElementById('foot-disk').textContent = `disk ${d.used_bytes}/${d.quota_bytes} B`;
    document.getElementById('foot-port').textContent = info.port ? `console :${info.port}` : '';
    meta.textContent = a.task ? `task: ${a.task}` : 'idle - give the agent something to do';
    agentState.textContent = 'agent: ' + a.state;
    agentState.className = 'pill' + (a.state === 'running' ? ' running' : '');
    stepHint.textContent = a.steps ? `${a.steps} steps` : '';

    wins.innerHTML = list.length
      ? ''
      : '<div><span>no windows open</span><span></span></div>';
    for (const w of list) {
      const div = document.createElement('div');
      div.innerHTML = `<span>#${w.id} ${w.app}${w.focused ? ' *' : ''}</span><span>${w.title}</span>`;
      wins.appendChild(div);
    }
  }

  async function poll() {
    try {
      const res = await fetch('/state?since=' + since, { cache: 'no-store' });
      const data = await res.json();
      if (!data.ok) return;
      since = data.events.length ? data.events[data.events.length - 1].seq : since;
      renderFeed(data.events);
      renderShots(data.shots);
      renderSys(data.sysinfo, data.windows);
      brain.textContent = 'brain: ' + data.provider_name;
      clock.textContent = new Date().toLocaleTimeString();
      screenW = data.screen.w; screenH = data.screen.h;
      // Only touch width/height when they really change: assigning them clears the
      // canvas (that reassignment every poll is what made the screen flicker).
      if (canvas.width !== screenW) canvas.width = screenW;
      if (canvas.height !== screenH) canvas.height = screenH;
      if (data.screen.seq !== lastScreenSeq) {
        lastScreenSeq = data.screen.seq;
        refreshScreen();
      }
      running = data.running;
    } catch (e) { /* server restarting */ }
  }

  /* ---------- actions ---------- */
  document.getElementById('run').onclick = async () => {
    const input = document.getElementById('task');
    const task = input.value.trim();
    if (!task) return;
    const r = await post('/prompt', { task });
    if (!r.ok) alert(r.error || 'could not start');
    else input.value = '';
  };
  document.getElementById('task').addEventListener('keydown', e => {
    if (e.key === 'Enter') document.getElementById('run').click();
  });
  document.getElementById('abort').onclick = () => post('/abort');
  document.getElementById('shell-run').onclick = async () => {
    const input = document.getElementById('shell');
    if (!input.value.trim()) return;
    const r = await post('/shell', { command: input.value });
    shellOut.textContent = r.output || '(no output)';
    input.value = '';
    refreshScreen();
  };
  document.getElementById('shell').addEventListener('keydown', e => {
    if (e.key === 'Enter') document.getElementById('shell-run').click();
  });
  document.querySelectorAll('.quick button[data-task]').forEach(btn => {
    btn.onclick = () => { document.getElementById('task').value = btn.dataset.task; document.getElementById('run').click(); };
  });
  const apps = ['terminal', 'files', 'editor', 'sysmon', 'agent'];
  const holder = document.getElementById('app-buttons');
  apps.forEach(app => {
    const b = document.createElement('button');
    b.textContent = 'open ' + app;
    b.onclick = async () => { await post('/open', { app }); refreshScreen(); };
    holder.appendChild(b);
  });

  poll();
  setInterval(poll, 500);
})();
