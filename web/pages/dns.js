// DNS: statistics, cache, upstream DNS, local names (static entries), name test.
'use strict';

App.page('dns', {
  async init(root) {
    const { h, api, cmd, part } = App;
    const SOURCES = { statyczny: tn('statyczny'), router: tn('router'), rezerwacja: tn('rezerwacja DHCP'), dzierzawa: tn('dzierżawa DHCP') }; // API source -> text (t)

    async function loadStats() {
      const r = await api('/api/dns/stats');
      const s = r.data, q = s.queries;
      const pct = (n) => (q.total ? ' (' + Math.round(100 * n / q.total) + '%)' : '');
      part(root, 'stats').replaceChildren(...[
        [t('Zapytania'), q.total + (s.running ? '' : ' – ' + t('usługa dns zatrzymana'))],
        [t('Lokalne'), q.local + pct(q.local)], [t('Zablokowane'), q.blocked + pct(q.blocked)],
        [t('Z cache'), q.cache + pct(q.cache)], [t('Przekazane'), q.forwarded + pct(q.forwarded)],
        [t('Timeouty / SERVFAIL'), q.timeouts + ' / ' + q.servfail], ['NXDOMAIN', q.nxdomain],
      ].flatMap(([k, v]) => [h('dt', {}, k), h('dd', {}, v)]));
      part(root, 'cache').textContent = t('{0} / {1} wpisów', s.cache.used, s.cache.size);
      part(root, 'upstream-mode').textContent = s.upstream.auto ? t('Tryb: auto (DNS z DHCP sieci nadrzędnej)') : t('Tryb: stałe serwery');
      part(root, 'upstream').replaceChildren(App.table([t('Serwer'), t('Odpowiedzi'), t('Timeouty'), t('Średnio')], s.upstream.servers.map((x) => [
        x.ip, x.answers, x.timeouts, x.avg_ms + ' ms']), t('Brak serwera (STA bez IP albo bez DNS)')));
      part(root, s.upstream.auto ? 'up-auto' : 'up-manual').checked = true;
      if (!s.upstream.auto) {
        part(root, 'up1').value = s.upstream.servers[0] ? s.upstream.servers[0].ip : '';
        part(root, 'up2').value = s.upstream.servers[1] ? s.upstream.servers[1].ip : '';
      }
    }

    async function loadNames() {
      const r = await api('/api/dns/names');
      part(root, 'names').replaceChildren(App.table([t('Nazwa'), 'IP', t('Źródło'), ''], (r.data.names || []).map((n) => [
        n.name, n.ip, t(SOURCES[n.source] || n.source),
        n.source === 'statyczny' ? h('span', {},
          h('button', { class: 'small', onclick: () => {
            part(root, 'name').value = n.name;
            part(root, 'ip').value = n.ip;
          } }, t('Edytuj')),
          h('button', { class: 'small danger', onclick: async () => {
            if (!confirm(t('Usunąć wpis {0}?', n.name))) return;
            await cmd('dns del ' + n.name);
            loadNames();
          } }, t('Usuń'))) : '',
      ]), t('Brak nazw lokalnych')));
    }

    part(root, 'refresh').addEventListener('click', loadStats);
    part(root, 'cache-clear').addEventListener('click', async () => { await cmd('dns cache clear'); loadStats(); });

    part(root, 'upstream-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const manual = part(root, 'up-manual').checked;
      const servers = [part(root, 'up1').value.trim(), part(root, 'up2').value.trim()].filter(Boolean);
      if (manual && !servers.length) {
        App.message(part(root, 'upstream-msg'), t('Podaj co najmniej jeden serwer'), false);
        return;
      }
      const r = await cmd('dns upstream ' + (manual ? servers.join(' ') : 'auto'), true);
      App.message(part(root, 'upstream-msg'), (r.data.output || r.data.error || '').trim().split('\n')[0], r.ok && r.data.ok);
      loadStats();
    });

    part(root, 'add-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const name = part(root, 'name').value.trim(), ip = part(root, 'ip').value.trim();
      if (/\s/.test(name) || /\s/.test(ip)) {
        App.message(part(root, 'add-msg'), t('Nazwa i adres bez spacji'), false);
        return;
      }
      const r = await cmd('dns add ' + name + ' ' + ip, true);
      App.message(part(root, 'add-msg'), (r.data.output || r.data.error || '').trim() || t('Błąd składni'), r.ok && r.data.ok);
      if (r.ok && r.data.ok) part(root, 'add-form').reset();
      loadNames();
    });

    part(root, 'lookup-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const name = part(root, 'lookup').value.trim();
      if (/\s/.test(name)) return;
      const r = await cmd('dns lookup ' + name, true);
      App.message(part(root, 'lookup-msg'), (r.data.output || '').trim(), r.ok && r.data.ok);
    });

    await Promise.all([loadStats(), loadNames()]);
  },
});
