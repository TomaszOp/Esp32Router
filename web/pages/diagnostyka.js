// Diagnostics: live logs (WebSocket /ws/log), terminal (/api/cmd), memory and tasks (/api/sys/mem).
'use strict';

(() => {
  let socket = null; // one log connection; closed when leaving the page

  function closeLog() {
    if (socket) {
      socket.onclose = null;
      socket.close();
      socket = null;
    }
  }

  App.page('diagnostyka', {
    async init(root) {
      const { h, api, part } = App;

      // ---- live logs ----
      const log = part(root, 'log'), state = part(root, 'log-state');
      let paused = false;
      closeLog();
      const connect = () => {
        socket = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/log');
        socket.onopen = () => { state.textContent = t('Połączono – nowe linie pojawiają się na bieżąco.'); };
        socket.onmessage = (e) => {
          if (paused) return;
          const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 20;
          log.append(e.data.replace(/\x1b\[[0-9;]*m/g, '')); // without ANSI colors
          if (log.textContent.length > 60000) log.textContent = log.textContent.slice(-40000);
          if (atBottom) log.scrollTop = log.scrollHeight;
        };
        socket.onclose = () => {
          state.textContent = t('Rozłączono – ponowna próba za 3 s.');
          socket = null;
          setTimeout(() => { if (App.currentPage() === 'diagnostyka' && !socket) connect(); }, 3000);
        };
      };
      connect();
      part(root, 'log-pause').addEventListener('click', (e) => {
        paused = !paused;
        e.target.textContent = paused ? t('Wznów') : t('Wstrzymaj');
      });
      part(root, 'log-clear').addEventListener('click', () => { log.textContent = ''; });

      // ---- terminal ----
      const term = part(root, 'term'), input = part(root, 'term-input');
      const history = [];
      let histPos = 0;
      part(root, 'term-form').addEventListener('submit', async (e) => {
        e.preventDefault();
        const line = input.value.trim();
        if (!line) return;
        history.push(line);
        histPos = history.length;
        input.value = '';
        term.append('esp32router> ' + line + '\n');
        const r = await api('/api/cmd', { cmd: line });
        term.append(((r.data.output || r.data.error || '') + '').replace(/\r/g, '') + '\n');
        term.scrollTop = term.scrollHeight;
        App.refreshStatus();
      });
      input.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowUp' && histPos > 0) {
          input.value = history[--histPos];
          e.preventDefault();
        } else if (e.key === 'ArrowDown') {
          histPos = Math.min(history.length, histPos + 1);
          input.value = history[histPos] || '';
          e.preventDefault();
        }
      });

      // ---- memory and tasks ----
      async function loadMem() {
        const r = await api('/api/sys/mem');
        const m = r.data, B = App.formatBytes;
        part(root, 'heap').replaceChildren(App.table([t('Pamięć'), t('Wolne'), t('Całość'), t('Minimum'), t('Największy blok')],
          [['SRAM', m.sram], ['DMA', m.dma], ['PSRAM', m.psram]].filter(([, x]) => x.total).map(([n, x]) => [
            n, B(x.free), B(x.total), B(x.min), B(x.largest)])));
        const f = m.flash;
        part(root, 'flash').replaceChildren(...[
          ['Flash', B(f.chip)],
          [t('Aplikacja'), t('{0} z {1}', B(f.app), B(f.app_partition)) + ' (' + Math.round(100 * f.app / f.app_partition) + '%)'],
          f.fs_total ? ['LittleFS', t('{0} z {1}', B(f.fs_used), B(f.fs_total))] : null,
          ['lwIP', t('{0} gniazd, NAT {1} wpisów, przekierowań {2}', m.lwip.max_sockets, m.lwip.napt, m.lwip.portmap)],
        ].filter(Boolean).flatMap(([k, v]) => [h('dt', {}, k), h('dd', {}, v)]));
        part(root, 'tasks').replaceChildren(App.table([t('Zadanie'), t('Priorytet'), t('Wolny stos (min.)')], m.tasks.map((x) => [
          x.name, x.prio, x.stack_free_min < 512 ? h('span', { class: 'warn-text' }, x.stack_free_min + ' B – ' + t('mało!')) : x.stack_free_min + ' B'])));
      }
      part(root, 'mem-refresh').addEventListener('click', loadMem);
      await loadMem();
    },
  });

  // Leaving the page – end of the log view.
  window.addEventListener('hashchange', () => { if (App.currentPage() !== 'diagnostyka') closeLog(); });
})();
