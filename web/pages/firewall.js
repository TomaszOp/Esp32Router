// Firewall: service state, log, rules (order, counters, editing), templates, recently blocked.
'use strict';

(() => {
  let reload = null;

  App.page('firewall', {
    async init(root) {
      const { h, api, cmd, part } = App;
      const f = {};
      for (const k of ['form', 'title', 'name', 'client', 'clients', 'proto', 'ports', 'dest', 'destip', 'action', 'enabled',
        'warn', 'fmsg']) f[k] = part(root, k);
      const DEST = { internet: tn('internet'), router: tn('router'), any: tn('dowolny') }; // API destination -> text (t)
      const TPL = {
        smtp: { name: 'smtp', proto: 'tcp', ports: '25', dest: 'internet' },
        smb: { name: 'smb', proto: 'tcp', ports: '445', dest: 'any' },
        dns: { name: 'wymus-dns', proto: 'both', ports: '53,853', dest: 'internet' },
        telnet: { name: 'telnet', proto: 'tcp', ports: '23', dest: 'router', client: '' },
      };
      let index = -1;
      let running = false;

      function resetForm(keepMsg) {
        index = -1;
        f.form.reset();
        f.enabled.checked = true;
        f.title.textContent = t('Nowa reguła');
        f.warn.hidden = true;
        if (!keepMsg) f.fmsg.textContent = '';
      }
      function checkWarn() {
        const client = f.client.value.trim();
        f.warn.hidden = !(f.action.value === 'block' && f.dest.value === 'router' && (client === '' || client === 'all') &&
          (f.destip.value.trim() === '' || f.destip.value.trim() === 'any'));
      }
      for (const k of ['client', 'dest', 'action', 'destip']) f[k].addEventListener('input', checkWarn);
      part(root, 'new').addEventListener('click', () => resetForm());

      root.querySelectorAll('[data-tpl]').forEach((b) => b.addEventListener('click', () => {
        const tpl = TPL[b.dataset.tpl];
        resetForm();
        f.name.value = tpl.name;
        f.client.value = tpl.client !== undefined ? tpl.client : 'all';
        f.proto.value = tpl.proto;
        f.ports.value = tpl.ports;
        f.dest.value = tpl.dest;
        f.destip.value = 'any';
        f.action.value = 'block';
        checkWarn();
        (tpl.client !== undefined ? f.client : f.name).focus();
      }));

      f.form.addEventListener('submit', async (e) => {
        e.preventDefault();
        const body = { name: f.name.value.trim(), client: f.client.value.trim() || 'all', proto: f.proto.value,
          ports: f.ports.value.replace(/\s/g, ''), dest: f.dest.value, dest_ip: f.destip.value.trim() || 'any',
          action: f.action.value, enabled: f.enabled.checked };
        if (index >= 0) body.index = index;
        const r = await api('/api/firewall', body);
        App.message(f.fmsg, (r.data.output || r.data.error || '').trim(), r.ok && r.data.ok);
        if (r.ok && r.data.ok) resetForm(true);
        load();
        App.refreshStatus();
      });

      part(root, 'svc').addEventListener('click', async () => {
        await cmd('service ' + (running ? 'stop' : 'start') + ' firewall');
        load();
      });
      part(root, 'log').addEventListener('change', async (e) => {
        const r = await api('/api/config/firewall', { log_blocked: e.target.checked });
        App.message(part(root, 'msg'), r.data.ok ? t('Zmienione – działa od razu, trwałe po Zapisz.') : t('Błąd'), r.data.ok);
        App.refreshStatus();
      });

      function edit(x, i) {
        index = i;
        f.title.textContent = t('Edycja reguły {0} ({1})', i + 1, x.name);
        f.name.value = x.name;
        f.client.value = x.client;
        f.proto.value = x.proto;
        f.ports.value = x.ports;
        f.dest.value = x.dest;
        f.destip.value = x.dest_ip;
        f.action.value = x.action;
        f.enabled.checked = x.enabled;
        checkWarn();
        f.name.focus();
      }

      let clientsLoaded = false;
      async function loadClients() {
        const [res, cl] = await Promise.all([api('/api/dhcp/reservations'), api('/api/clients')]);
        f.clients.replaceChildren(h('option', { value: 'all' }, t('wszyscy klienci')),
          ...(res.data.reservations || []).filter((x) => x.name).map((x) => h('option', { value: x.name }, t('rezerwacja {0}', x.ip))),
          ...(cl.data.clients || []).filter((c) => c.ip).map((c) => h('option', { value: c.ip }, c.name || c.mac)),
          ...(cl.data.clients || []).map((c) => h('option', { value: c.mac }, (c.name || t('klient')) + ' (MAC)')));
        clientsLoaded = true;
      }

      async function load() {
        const r = await api('/api/firewall');
        if (!r.ok || !root.isConnected) return;
        const d = r.data;
        running = d.running;
        part(root, 'dot').className = 'dot ' + (d.running ? 'on' : 'off');
        part(root, 'state').textContent = d.running ? t('Działa.')
          : t('Zatrzymana – cały ruch jest przepuszczany. Autostart: System → Usługi.');
        part(root, 'svc').textContent = d.running ? t('Zatrzymaj') : t('Uruchom');
        part(root, 'log').checked = d.log_blocked;
        part(root, 'extra').textContent = t('Ping z WAN: {0} (System → Ogólne), odrzucone: {1}.', d.wan_ping ? t('dozwolony') : t('blokowany'),
          d.ping_dropped) + ' ' + t('Przekierowania z ograniczonym źródłem: odrzucone {0}.', d.pf_dropped);
        const rules = d.rules || [];
        part(root, 'count').textContent = '(' + rules.length + ' / ' + d.max_rules + ')';
        part(root, 'rules').replaceChildren(App.table(['#', t('Nazwa'), t('Klient'), t('Protokół'), t('Porty'), t('Cel'), t('Adres celu'), t('Akcja'),
          t('Trafienia'), t('Stan'), ''], rules.map((x, i) => [
          i + 1, x.name, x.client + (x.ip && x.ip !== x.client ? ' (' + x.ip + ')' : ''), x.proto, x.ports || '*',
          t(DEST[x.dest] || x.dest), x.dest_ip, x.action === 'block' ? t('blokuj') : t('zezwól'), x.hits,
          !x.enabled ? t('wyłączona') : (x.error || t('aktywna')),
          h('span', {},
            h('button', { class: 'small secondary', title: t('Wyżej'), disabled: i === 0, onclick: async () => {
              await cmd('fw move ' + x.name + ' ' + i, true);
              load();
            } }, '↑'),
            h('button', { class: 'small secondary', title: t('Niżej'), disabled: i === rules.length - 1, onclick: async () => {
              await cmd('fw move ' + x.name + ' ' + (i + 2), true);
              load();
            } }, '↓'),
            h('button', { class: 'small', onclick: () => edit(x, i) }, t('Edytuj')),
            h('button', { class: 'small secondary', onclick: async () => {
              await cmd('fw ' + (x.enabled ? 'disable ' : 'enable ') + x.name);
              load();
            } }, x.enabled ? t('Wyłącz') : t('Włącz')),
            h('button', { class: 'small danger', onclick: async () => {
              if (!confirm(t('Usunąć regułę {0}?', x.name))) return;
              await cmd('fw del ' + x.name);
              resetForm();
              load();
            } }, t('Usuń'))),
        ]), t('Brak reguł – użyj formularza albo szablonu poniżej')));
        part(root, 'recent').replaceChildren(App.table([t('Czas'), t('Reguła'), t('Klient'), t('Cel'), t('Protokół')], (d.recent || []).map((x) => [
          x.time, x.rule, x.src, x.dst, x.proto + (x.port ? '/' + x.port : ''),
        ]), t('Brak')));
        if (!clientsLoaded) loadClients();
      }
      reload = load;
      await load();
    },
    onStatus() {
      if (reload) reload();
    },
  });
})();
