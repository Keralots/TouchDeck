let _currentManifestUrl = null;
let _monitorPort = null;
let _monitorReader = null;
let _monitorTask = null;
let _monitorBusy = false;

async function loadVersion() {
  const response = await fetch('firmware/latest/VERSION', { cache: 'no-cache' });
  if (!response.ok) throw new Error(`VERSION returned HTTP ${response.status}`);
  const version = (await response.text()).trim();
  if (!/^v?\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/.test(version)) {
    throw new Error('VERSION is empty or invalid');
  }
  return version;
}

function buildManifest(version) {
  const binUrl = new URL(`firmware/latest/TouchDeck-${version}-full.bin`, document.baseURI).href;
  return {
    name: 'TouchDeck',
    version,
    new_install_prompt_erase: true,
    // Allow first boot to expose Improv Serial for WiFi setup.
    new_install_improv_wait_time: 15,
    builds: [{ chipFamily: 'ESP32-S3', parts: [{ path: binUrl, offset: 0 }] }],
  };
}

function renderInstallButton(version) {
  if (_currentManifestUrl) URL.revokeObjectURL(_currentManifestUrl);
  const blob = new Blob([JSON.stringify(buildManifest(version))], { type: 'application/json' });
  _currentManifestUrl = URL.createObjectURL(blob);
  const button = document.createElement('esp-web-install-button');
  button.setAttribute('manifest', _currentManifestUrl);
  const activate = document.createElement('button');
  activate.type = 'button';
  activate.className = 'btn-monitor';
  activate.slot = 'activate';
  activate.textContent = 'Install TouchDeck';
  activate.disabled = _monitorBusy || !!_monitorPort;
  button.appendChild(activate);
  for (const [slot, message] of [
    ['unsupported', 'Your browser does not support Web Serial. Use Chrome or Edge on desktop.'],
    ['not-allowed', 'Web Serial requires a secure context (HTTPS). Open https://touchdeck.stolaris.dev/.'],
  ]) {
    const fallback = document.createElement('span');
    fallback.slot = slot;
    fallback.className = 'unsupported';
    fallback.textContent = message;
    button.appendChild(fallback);
  }
  document.getElementById('install-slot').replaceChildren(button);
}

function showStatus(message, kind = '') {
  const line = document.getElementById('status-line');
  line.textContent = message;
  line.className = `status-line ${kind}`.trim();
}

function showVersion(version) {
  document.getElementById('spec-version').textContent = version;
  document.getElementById('rail-version').textContent = version;
  document.querySelectorAll('[data-version]').forEach(span => { span.textContent = version; });
}

async function init() {
  if (!('serial' in navigator)) document.getElementById('browser-callout').classList.add('show');
  wireMonitor();
  wirePage();
  try {
    const version = await loadVersion();
    showVersion(version);
    renderInstallButton(version);
    showStatus('Ready to install. Connect your deck over USB.', 'ok');
  } catch (err) {
    document.getElementById('spec-version').textContent = 'unavailable';
    document.getElementById('rail-version').textContent = 'unavailable';
    document.getElementById('install-slot').replaceChildren();
    showStatus(`Could not load firmware version (${err.message}). The site may be mid-deploy - try again in a minute.`, 'error');
  }
  if ('customElements' in window) {
    setTimeout(() => {
      if (!customElements.get('esp-web-install-button') && _currentManifestUrl) {
        showStatus('The installer could not load. Check your connection and reload this page.', 'error');
      }
    }, 20000);
  }
}

async function monitorConnect() {
  if (_monitorPort || _monitorBusy) return;
  _monitorBusy = true;
  toggleMonitorButtons();
  let port;
  try {
    try {
      port = await navigator.serial.requestPort();
    } catch (err) {
      if (err.name !== 'NotFoundError') setMonitorStatus(`Could not pick a port: ${err.message}`, 'error');
      return;
    }
    try {
      await port.open({ baudRate: 115200 });
    } catch (err) {
      setMonitorStatus(`Could not open the port: ${err.message}. Close the companion and other monitors, then try again.`, 'error');
      return;
    }
    _monitorPort = port;
    setMonitorStatus('Connected. Reading from device...', 'ok');
    _monitorTask = monitorReadLoop(port);
  } finally {
    _monitorBusy = false;
    toggleMonitorButtons();
  }
}

async function monitorDisconnect() {
  if (!_monitorPort || _monitorBusy) return;
  _monitorBusy = true;
  toggleMonitorButtons();
  setMonitorStatus('Disconnecting...');
  try {
    if (_monitorReader) await _monitorReader.cancel();
    if (_monitorTask) await _monitorTask;
    setMonitorStatus('Disconnected.');
  } catch (err) {
    setMonitorStatus(`Could not disconnect: ${err.message}`, 'error');
  } finally {
    _monitorBusy = false;
    toggleMonitorButtons();
  }
}

async function monitorReadLoop(port) {
  const decoder = new TextDecoder();
  let reader;
  try {
    if (!port.readable) throw new Error('No readable serial stream');
    reader = port.readable.getReader();
    _monitorReader = reader;
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value && value.byteLength) appendMonitorOutput(decoder.decode(value, { stream: true }));
    }
    const tail = decoder.decode();
    if (tail) appendMonitorOutput(tail);
    if (!_monitorBusy) setMonitorStatus('Device disconnected.');
  } catch (err) {
    setMonitorStatus(`Read error: ${err.message}`, 'error');
  } finally {
    if (reader) reader.releaseLock();
    _monitorReader = null;
    try { await port.close(); } catch (_) {}
    if (_monitorPort === port) _monitorPort = null;
    toggleMonitorButtons();
  }
}

function appendMonitorOutput(text) {
  const out = document.getElementById('monitor-output');
  const atBottom = out.scrollHeight - out.clientHeight - out.scrollTop < 4;
  out.appendChild(document.createTextNode(text));
  // Bound the log buffer during long monitor sessions.
  if (out.textContent.length > 200000) out.textContent = out.textContent.slice(-150000);
  if (atBottom) out.scrollTop = out.scrollHeight;
  setMonitorBufferButtons(true);
}

function monitorExport() {
  const text = document.getElementById('monitor-output').textContent;
  if (!text) return;
  const ts = new Date().toISOString().replace(/[:.]/g, '-').replace('Z', '');
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = `touchdeck-serial-${ts}.txt`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function monitorClear() {
  document.getElementById('monitor-output').textContent = '';
  setMonitorBufferButtons(false);
}

function setMonitorBufferButtons(hasContent) {
  document.getElementById('monitor-export').disabled = !hasContent;
  document.getElementById('monitor-clear').disabled = !hasContent;
}

function setMonitorStatus(message, kind = '') {
  const line = document.getElementById('monitor-status');
  line.textContent = message;
  line.className = `status-line ${kind}`.trim();
}

function toggleMonitorButtons() {
  document.getElementById('monitor-connect').disabled = _monitorBusy || !!_monitorPort || !('serial' in navigator);
  document.getElementById('monitor-disconnect').disabled = _monitorBusy || !_monitorPort;
  const install = document.querySelector('#install-slot [slot="activate"]');
  if (install) install.disabled = _monitorBusy || !!_monitorPort;
}

function wireMonitor() {
  if (!('serial' in navigator)) {
    document.getElementById('monitor-connect').disabled = true;
    setMonitorStatus('Web Serial is unavailable in this browser - use desktop Chrome or Edge.', 'warn');
  } else {
    document.getElementById('monitor-connect').addEventListener('click', monitorConnect);
  }
  document.getElementById('monitor-disconnect').addEventListener('click', monitorDisconnect);
  document.getElementById('monitor-export').addEventListener('click', monitorExport);
  document.getElementById('monitor-clear').addEventListener('click', monitorClear);
}

function wirePage() {
  const items = [...document.querySelectorAll('.toc-item')];
  const headings = [...document.querySelectorAll('.panel-head h2')];
  const setActive = id => items.forEach(item => {
    const active = item.dataset.target === id;
    item.classList.toggle('is-active', active);
    if (active) item.setAttribute('aria-current', 'step');
    else item.removeAttribute('aria-current');
  });
  if (headings.length) setActive(headings[0].id);
  if ('IntersectionObserver' in window) {
    const observer = new IntersectionObserver(entries => {
      const visible = entries.filter(entry => entry.isIntersecting).sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top);
      if (visible.length) setActive(visible[0].target.id);
    }, { rootMargin: '-40px 0px -65% 0px', threshold: 0 });
    headings.forEach(heading => observer.observe(heading));
  }
  const tabs = [...document.querySelectorAll('.os-tab')];
  const selectTab = tab => {
    tabs.forEach(item => {
      const active = item === tab;
      item.classList.toggle('is-active', active);
      item.setAttribute('aria-selected', String(active));
      item.tabIndex = active ? 0 : -1;
    });
    document.querySelectorAll('[data-os-panel]').forEach(panel => {
      const active = panel.dataset.osPanel === tab.dataset.os;
      panel.classList.toggle('is-active', active);
      panel.hidden = !active;
    });
  };
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => selectTab(tab));
    tab.addEventListener('keydown', event => {
      let next;
      if (event.key === 'ArrowRight') next = tabs[(index + 1) % tabs.length];
      if (event.key === 'ArrowLeft') next = tabs[(index + tabs.length - 1) % tabs.length];
      if (event.key === 'Home') next = tabs[0];
      if (event.key === 'End') next = tabs[tabs.length - 1];
      if (!next) return;
      event.preventDefault();
      selectTab(next);
      next.focus();
    });
  });
  document.querySelectorAll('[data-cmd]').forEach(block => {
    const button = block.querySelector('.cmd-copy');
    button.addEventListener('click', async () => {
      const text = [...block.querySelector('pre').childNodes]
        .filter(node => !node.classList?.contains('pr')).map(node => node.textContent).join('').trim();
      try {
        await navigator.clipboard.writeText(text);
        button.textContent = 'copied';
        button.classList.add('copied');
      } catch (_) {
        button.textContent = 'retry';
      }
      setTimeout(() => { button.textContent = 'copy'; button.classList.remove('copied'); }, 1400);
    });
  });
  document.querySelectorAll('.showcase img[data-fallback]').forEach(img => {
    const fallback = () => {
      if (!img.dataset.fallback) return;
      img.src = img.dataset.fallback;
      delete img.dataset.fallback;
      img.alt = 'TouchDeck screen preview';
      const note = document.createElement('span');
      note.className = 'preview-note';
      note.textContent = 'Still preview. Animation recording is not available yet.';
      img.parentElement.querySelector('figcaption').appendChild(note);
    };
    img.addEventListener('error', fallback, { once: true });
    if (img.complete && img.naturalWidth === 0) fallback();
  });
}

init();
