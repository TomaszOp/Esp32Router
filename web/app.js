// Esp32Router – panel core: API, login, menu, routing (#/page), unsaved-changes bar,
// configuration forms. Page scripts are in pages/<id>.js and register through App.page().
'use strict';

// ---- language versions (doc/dev/jezyki.md) ----
// Texts in the code and in HTML are Polish; for another language the dictionary /i18n/<lang>.json is loaded
// (key = Polish text). Missing key -> Polish text. {0}, {1}... – values inserted into the text.
let i18nDict = null;
/** Translates a Polish text; {0}, {1}... are replaced with args. */
function t(text, ...args) {
  const s = (i18nDict && i18nDict[text]) || text;
  return args.length ? s.replace(/\{(\d+)\}/g, (m, i) => (args[i] === undefined ? m : args[i])) : s;
}
// Text for the dictionary, translated later (when displayed) by t() – e.g. menu labels, maps of API values.
const tn = (text) => text;

const App = (() => {
  const $ = (id) => document.getElementById(id);
  const pages = {};
  let statusTimer = null;
  let lastStatus = null;

  // Menu: groups as in hardware routers. later = page of a later stage.
  const MENU = [
    { group: null, items: [['stan', tn('Stan')]] },
    { group: tn('Sieć'), items: [['internet', tn('Internet (STA)')], ['wifi', tn('Wi-Fi (AP)')], ['dhcp', tn('LAN / DHCP')], ['klienci', tn('Klienci')], ['ddns', tn('DDNS')]] },
    { group: tn('Routing'), items: [['nat', tn('NAT')], ['firewall', tn('Firewall')], ['pf', tn('Przekierowanie portów')]] },
    { group: tn('DNS'), items: [['dns', tn('DNS')], ['filtr', tn('Filtrowanie')]] },
    { group: tn('System'), items: [['ogolne', tn('Ogólne')], ['uslugi', tn('Usługi')], ['konfiguracja', tn('Konfiguracja')],
      ['firmware', tn('Firmware')], ['haslo', tn('Hasło panelu')], ['diagnostyka', tn('Diagnostyka')]] },
  ];

  // Builds elements without innerHTML (device data, e.g. an SSID, is never interpreted as HTML).
  function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') el.className = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else if (v === true) el.setAttribute(k, '');
      else if (v !== false && v != null) el.setAttribute(k, v);
    }
    for (const c of children.flat()) {
      if (c != null && c !== false) el.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return el;
  }

  /** HTTP request to the device API (POST when body is given); returns { ok, status, data }. */
  async function api(path, body, raw) {
    const opts = { method: body === undefined ? 'GET' : 'POST', headers: {} };
    if (body !== undefined) {
      opts.headers['X-Requested-With'] = 'esp32router';
      if (raw) {
        opts.body = body;
        opts.headers['Content-Type'] = 'application/octet-stream';
      } else {
        opts.body = JSON.stringify(body);
        opts.headers['Content-Type'] = 'application/json';
      }
    }
    let res;
    try {
      res = await fetch(path, opts);
    } catch (e) {
      return { ok: false, status: 0, data: { error: t('Brak połączenia z urządzeniem') } };
    }
    let data = {};
    try { data = await res.json(); } catch (e) { /* brak treści */ }
    if (res.status === 401 && path !== '/api/login') {
      showLogin();
    }
    return { ok: res.ok, status: res.status, data };
  }

  // Console command through /api/cmd; the result as a message.
  async function cmd(line, quiet) {
    const r = await api('/api/cmd', { cmd: line });
    const text = (r.data.output || r.data.error || '').trim();
    if (!quiet) toast(text || (r.ok ? 'OK' : t('Błąd')), !(r.ok && r.data.ok));
    refreshStatus();
    return r;
  }

  let toastTimer = null;
  /** Short message at the bottom of the screen (error = red, longer). */
  function toast(text, error) {
    const t = $('toast');
    t.textContent = text;
    t.className = 'toast' + (error ? ' error' : '');
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, error ? 7000 : 4000);
  }

  /** Message under a form (ok = green, otherwise red). */
  function message(el, text, ok) {
    el.textContent = text;
    el.className = 'msg ' + (ok ? 'ok' : 'error');
  }

  /** Uptime in seconds -> "1 d 2 h 3 min". */
  function formatUptime(s) {
    const d = Math.floor(s / 86400), hh = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
    return (d ? d + ' d ' : '') + hh + ' h ' + m + ' min';
  }

  /** Bytes -> "12 KB" / "1.5 MB". */
  function formatBytes(b) {
    return b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB';
  }

  // ---- login ----

  /** Shows the login screen and stops status refreshing. */
  function showLogin() {
    clearInterval(statusTimer);
    $('app').hidden = true;
    $('login').hidden = false;
    $('login-pass').value = '';
    $('login-pass').focus();
  }

  /** Shows the panel after login: menu, status every 5 s, current page. */
  async function showApp() {
    $('login').hidden = true;
    $('app').hidden = false;
    buildMenu();
    await refreshStatus();
    clearInterval(statusTimer);
    statusTimer = setInterval(refreshStatus, 5000);
    route();
  }

  // ---- header and unsaved-changes bar ----

  /** Reads /api/status, updates the header and the unsaved-changes bar; returns the status or null. */
  async function refreshStatus() {
    const r = await api('/api/status');
    if (!r.ok) return null;
    const s = r.data;
    lastStatus = s;
    $('hdr-host').textContent = s.device.hostname;
    $('hdr-version').textContent = s.device.version;
    const net = $('hdr-net');
    net.textContent = s.sta.connected ? 'Internet: ' + s.sta.ip : t('Internet: brak');
    net.className = 'pill ' + (s.sta.connected ? 'ok' : 'bad');
    $('hdr-clients').textContent = t('Klienci: {0}', s.ap.clients);
    $('hdr-time').textContent = s.device.time;
    $('default-pass').hidden = !s.default_password;
    $('unsaved').hidden = !s.unsaved;
    $('unsaved-text').textContent = t('Niezapisane zmiany: {0}', s.unsaved);
    const page = pages[current];
    if (page && page.onStatus && $('content').isConnected) page.onStatus(s);
    return s;
  }

  /** Saves all changes (Save in the bar). */
  async function saveAll() {
    const r = await api('/api/save', {});
    if (r.ok) {
      const w = r.data.written || [];
      toast(t('Zapisano: {0}; bez zmian: {1}', w.length ? w.map((m) => m + '.json').join(', ') : t('nic'), r.data.unchanged),
        !r.data.ok);
    }
    await refreshStatus();
    route();
  }

  /** Discards unsaved changes (Revert in the bar). */
  async function revertAll() {
    if (!confirm(t('Porzucić wszystkie niezapisane zmiany?'))) return;
    const r = await api('/api/revert', {});
    if (r.ok) toast(t('Przywrócono zapisaną konfigurację'));
    await refreshStatus();
    route();
  }

  // ---- menu and routing ----

  let current = null;

  /** Builds the menu (labels translated). */
  function buildMenu() {
    const menu = $('menu');
    menu.textContent = '';
    for (const g of MENU) {
      if (g.group) menu.append(h('div', { class: 'group' }, t(g.group)));
      for (const [id, label, later] of g.items) {
        menu.append(h('a', { href: '#/' + id, 'data-page': id }, t(label), later ? h('span', { class: 'later' }, ' ' + t('(etap {0})', later)) : null));
      }
    }
  }

  // Page: pages/<id>.html (content) and pages/<id>.js (logic), fetched on the first visit.
  const htmlCache = {};
  const scriptLoaded = {};

  /** Loads pages/<id>.js once. */
  function loadScript(id) {
    if (!scriptLoaded[id]) {
      scriptLoaded[id] = new Promise((resolve) => {
        const el = document.createElement('script');
        el.src = '/pages/' + id + '.js';
        el.onload = resolve;
        el.onerror = resolve; // page without logic
        document.body.append(el);
      });
    }
    return scriptLoaded[id];
  }

  /** Shows the page from the address (#/id): HTML, translation, forms, page init(). */
  async function route() {
    const id = (location.hash.replace(/^#\//, '') || 'stan').split('?')[0];
    current = id;
    for (const a of document.querySelectorAll('#menu a')) a.classList.toggle('active', a.dataset.page === id);
    $('menu').classList.remove('open');
    const content = $('content');
    const item = MENU.flatMap((g) => g.items).find((i) => i[0] === id);
    if (!item || item[2]) {
      content.textContent = '';
      content.append(h('div', { class: 'card' }, h('h2', {}, item ? t(item[1]) : t('Nie ma takiej strony')),
        h('p', { class: 'muted' }, item ? t('W przygotowaniu (etap {0}).', item[2]) : '')));
      return;
    }
    if (htmlCache[id] === undefined) {
      const res = await fetch('/pages/' + id + '.html');
      htmlCache[id] = res.ok ? await res.text() : '<div class="card"><p class="msg error">Nie udało się wczytać strony</p></div>';
    }
    await loadScript(id);
    if (current !== id) return; // meanwhile navigated to another page
    content.innerHTML = htmlCache[id]; // content from the firmware (our files); device data is inserted as text
    translate(content);
    await bindConfigForms(content);
    if (current !== id) return;
    const page = pages[id];
    if (page && page.init) page.init(content, lastStatus);
  }

  /** Registers a page script: { init(root, status), onStatus(status)? }. */
  function page(id, def) { pages[id] = def; }

  // Parameters from the address, e.g. #/pf?target=192.168.4.10
  function params() {
    const q = location.hash.split('?')[1] || '';
    return Object.fromEntries(new URLSearchParams(q));
  }

  // Table: headers + rows (arrays of cells); empty list = message.
  function table(headers, rows, empty) {
    if (!rows.length) return h('p', { class: 'muted' }, empty || t('Brak'));
    return h('div', { class: 'tbl' }, h('table', {}, h('thead', {}, h('tr', {}, headers.map((t) => h('th', {}, t)))),
      h('tbody', {}, rows.map((r) => h('tr', {}, r.map((c) => h('td', {}, c)))))));
  }

  // ---- configuration form (GET/POST /api/config/<module>) ----
  // spec: [{ path, label, options?: [[value, label]], help? }]
  async function configForm(container, module, spec, opts) {
    opts = opts || {};
    const card = opts.inline ? container : h('div', { class: 'card' }, opts.title ? h('h2', {}, opts.title) : null);
    if (!opts.inline) container.append(card);
    const r = await api('/api/config/' + module);
    if (!r.ok) {
      card.append(h('p', { class: 'msg error' }, r.data.error || t('Błąd odczytu')));
      return;
    }
    const fields = Object.fromEntries(r.data.fields.map((f) => [f.path, f]));
    const inputs = {};
    const form = opts.inline && container.tagName === 'FORM' ? container : h('form', {});
    for (const s of spec) {
      const f = fields[s.path];
      if (!f) continue;
      const id = 'f-' + module + '-' + s.path.replace(/\W/g, '-');
      let input;
      if (f.type === 'bool') {
        input = h('input', { type: 'checkbox', id });
        input.checked = f.value;
      } else if (s.options) {
        input = h('select', { id }, s.options.map(([v, l]) => h('option', { value: v }, l)));
        input.value = f.value;
      } else {
        input = h('input', { id, type: f.secret ? 'password' : (f.type === 'number' ? 'number' : 'text') });
        input.value = f.value;
        if (f.secret) input.placeholder = f.set ? t('(bez zmian)') : t('(brak)');
      }
      inputs[s.path] = { input, field: f };
      const badges = [
        f.live ? null : h('span', { class: 'badge', title: t('Zmiana działa po restarcie usługi lub urządzenia') }, t('po restarcie')),
        f.source === 'zmienione, niezapisane' ? h('span', { class: 'badge changed' }, t('niezapisane')) : null, // source – code from the API
      ];
      const help = [s.help, f.hint].filter(Boolean).join(' · ');
      const row = f.type === 'bool'
        ? [h('label', { class: 'check', for: id }, input, s.label, badges), help ? h('div', { class: 'help' }, help) : null]
        : [h('label', { for: id }, s.label, badges), input, help ? h('div', { class: 'help' }, help) : null];
      if (s.slot) {
        s.slot.classList.add('form-row');
        s.slot.append(...row.filter(Boolean));
      } else {
        form.append(h('div', { class: 'form-row' }, row));
      }
    }
    const msg = h('p', { class: 'msg' });
    form.append(h('button', { type: 'submit' }, opts.button || t('Zastosuj')), msg);
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const changes = {};
      for (const [path, { input, field }] of Object.entries(inputs)) {
        let v = field.type === 'bool' ? input.checked : input.value;
        if (field.type === 'number') v = Number(v);
        if (field.secret && v === '') continue; // empty password = unchanged
        if (v !== field.value) changes[path] = v;
      }
      if (!Object.keys(changes).length) {
        message(msg, t('Brak zmian'), true);
        return;
      }
      const res = await api('/api/config/' + module, changes);
      const results = (res.data && res.data.results) || [];
      const lines = results.map((x) => (x.ok ? '✓ ' : '✗ ') + x.path + ': ' +
        (x.ok ? (x.live ? t('działa od razu') : t('zadziała po restarcie')) : x.message.trim()));
      message(msg, lines.join('\n') + (res.data.ok ? '\n' + t('Zapisz zmiany, żeby były trwałe.') : ''), res.ok && res.data.ok);
      for (const x of results) if (x.ok && inputs[x.path]) inputs[x.path].field.value = changes[x.path];
      refreshStatus();
      if (opts.onSaved) opts.onSaved(res.data);
    });
    if (form !== card) card.append(form);
  }

  // After a device restart: wait until the panel responds again, then reload.
  function waitForReboot(msgEl, text) {
    message(msgEl, text + ' ' + t('Czekam na urządzenie...'), true);
    let tries = 0;
    const timer = setInterval(async () => {
      tries++;
      try {
        const r = await fetch('/api/session', { cache: 'no-store' });
        if (r.ok && tries > 2) {
          clearInterval(timer);
          location.reload();
        }
      } catch (e) { /* jeszcze się uruchamia */ }
      if (tries > 60) {
        clearInterval(timer);
        message(msgEl, t('Urządzenie nie odpowiada – odśwież stronę ręcznie.'), false);
      }
    }, 2000);
  }

  // Reasons of a failed STA connection (ESP-IDF Wi-Fi codes); translated by t().
  const STA_REASONS = {
    201: tn('nie znaleziono sieci'), 212: tn('nie znaleziono sieci'),
    210: tn('sieć ma niezgodne zabezpieczenia'), 211: tn('sieć ma niezgodne zabezpieczenia'),
    202: tn('złe hasło'), 15: tn('złe hasło'), 204: tn('złe hasło'),
    203: tn('sieć odrzuciła połączenie'), 205: tn('sieć odrzuciła połączenie'),
    2: tn('przekroczony czas uwierzytelniania'), 200: tn('utracono sygnał sieci'),
  };
  const staReason = (sta) => (STA_REASONS[sta.last_error_code] ? t(STA_REASONS[sta.last_error_code]) : sta.last_error || t('nieznany błąd'));

  // Page element by its data-id attribute.
  const part = (root, id) => root.querySelector('[data-id="' + id + '"]');

  // Configuration form described in HTML:
  //   <form data-config="global"> <div data-field="hostname" data-label="Nazwa"></div> ... </form>
  // data-options="value:label,…" (list), data-help="…" (hint). The button and message are added automatically.
  function bindConfigForms(root) {
    const done = [];
    for (const el of root.querySelectorAll('[data-config]')) {
      const spec = [...el.querySelectorAll('[data-field]')].map((f) => ({
        path: f.dataset.field,
        label: f.dataset.label || f.dataset.field,
        help: f.dataset.help,
        options: f.dataset.options ? f.dataset.options.split(',').map((o) => o.split(':')) : null,
        slot: f,
      }));
      done.push(configForm(el, el.dataset.config, spec, { button: el.dataset.button, inline: true }));
    }
    return Promise.all(done);
  }

  /** Starts the panel: event handlers, session check, language, login screen or panel. */
  function start() {
    $('login-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const r = await api('/api/login', { password: $('login-pass').value });
      if (r.ok) {
        message($('login-msg'), '', true);
        showApp();
      } else {
        message($('login-msg'), r.data.error || t('Błąd logowania'), false);
      }
    });
    $('logout').addEventListener('click', async () => {
      await api('/api/logout', {});
      showLogin();
    });
    $('menu-toggle').addEventListener('click', () => $('menu').classList.toggle('open'));
    $('unsaved-save').addEventListener('click', saveAll);
    $('unsaved-revert').addEventListener('click', revertAll);
    $('lang').addEventListener('click', switchLanguage);
    window.addEventListener('hashchange', route);
    api('/api/session').then(async (r) => {
      await loadLanguage(r.ok ? r.data.language : 'pl');
      if (r.ok && r.data.auth) showApp();
      else showLogin();
    });
  }

  // ---- language ----

  let language = 'pl';

  // Translates ready HTML (skeleton, pages): texts and description attributes; key = Polish text
  // with whitespace collapsed. data-options: the labels after the colon are translated.
  function translate(root) {
    if (!i18nDict) return;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
      const key = n.nodeValue.replace(/\s+/g, ' ').trim();
      if (key && i18nDict[key]) n.nodeValue = n.nodeValue.replace(/\S[\s\S]*\S|\S/, i18nDict[key]);
    }
    for (const el of root.querySelectorAll('[data-label],[data-help],[data-button],[data-options],[placeholder],[title],[aria-label]')) {
      for (const a of ['data-label', 'data-help', 'data-button', 'placeholder', 'title', 'aria-label']) {
        const v = el.getAttribute(a);
        if (v && i18nDict[v.replace(/\s+/g, ' ').trim()]) el.setAttribute(a, i18nDict[v.replace(/\s+/g, ' ').trim()]);
      }
      const o = el.getAttribute('data-options');
      if (o) el.setAttribute('data-options', o.split(',').map((p) => p.split(':')).map(([v, l]) => v + ':' + t(l)).join(','));
    }
  }

  /** Sets the language; for a language other than pl loads the dictionary and translates the page. */
  async function loadLanguage(lang) {
    language = lang === 'en' ? 'en' : 'pl';
    document.documentElement.lang = language;
    // Current language highlighted (a bare "EN" looked like the current language).
    $('lang').replaceChildren(...['pl', 'en'].flatMap((l, i) => [i ? ' | ' : '',
      l === language ? h('b', {}, l.toUpperCase()) : l.toUpperCase()]).filter(Boolean));
    $('lang').title = language === 'pl' ? 'Język: polski – przełącz na English' : 'Language: English – switch to Polski';
    if (language === 'pl') return;
    try {
      const res = await fetch('/i18n/' + language + '.json');
      if (res.ok) i18nDict = await res.json();
    } catch (e) { /* bez słownika – tekst polski */ }
    translate(document.body);
  }

  // PL/EN switch in the header: changes global.language (immediately, permanent after Save) and reloads the panel.
  async function switchLanguage() {
    const r = await api('/api/config/global', { language: language === 'pl' ? 'en' : 'pl' });
    if (r.ok && r.data.ok) location.reload();
    else toast(t('Błąd'), true);
  }

  return { start, page, h, api, cmd, toast, message, configForm, refreshStatus, formatUptime, formatBytes,
    saveAll, revertAll, status: () => lastStatus, route, currentPage: () => current, params, table, waitForReboot, part, staReason };
})();
