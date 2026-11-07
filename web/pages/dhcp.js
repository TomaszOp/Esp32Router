// LAN / DHCP: leases (Reserve / Release) and reservations (add / edit / delete).
'use strict';

(() => {
  const { h, api, cmd, part } = App;
  const TYPES = { oferta: tn('oferta'), rezerwacja: tn('rezerwacja'), dynamiczny: tn('dynamiczny') }; // API type -> text (t)
  const validName = (s) => /^[A-Za-z0-9-]{0,32}$/.test(s);
  const remaining = (s) => (s >= 3600 ? Math.floor(s / 3600) + ' h ' + Math.floor(s / 60) % 60 + ' min' : Math.floor(s / 60) + ' min');
  const reserve = (mac, ip, name) => cmd('dhcp reserve ' + mac + (ip ? ' ' + ip : '') + (name ? ' ' + name : ''), true);

  App.page('dhcp', {
    async init(root) {
      const form = part(root, 'res-form'), macIn = part(root, 'mac'), ipIn = part(root, 'ip'), nameIn = part(root, 'name');
      const msg = part(root, 'res-msg');
      let editing = null; // edited reservation

      form.addEventListener('reset', () => { editing = null; msg.textContent = ''; });
      form.addEventListener('submit', async (e) => {
        e.preventDefault();
        const mac = macIn.value.trim().toLowerCase(), ip = ipIn.value.trim(), name = nameIn.value.trim();
        if (!validName(name)) {
          App.message(msg, t('Nazwa: 0-32 znaki a-z A-Z 0-9 -'), false);
          return;
        }
        // MAC of an edited reservation changed: delete the old one, restore it on error.
        const old = editing && editing.mac !== mac ? editing : null;
        if (old) await cmd('dhcp unreserve ' + old.mac, true);
        const r = await reserve(mac, ip, name);
        if (!(r.ok && r.data.ok) && old) await reserve(old.mac, old.ip, old.name);
        App.message(msg, (r.data.output || r.data.error || '').trim(), r.ok && r.data.ok);
        if (r.ok && r.data.ok) {
          editing = null;
          macIn.value = ipIn.value = nameIn.value = '';
        }
        load();
      });

      async function load() {
        const [l, r] = await Promise.all([api('/api/dhcp/leases'), api('/api/dhcp/reservations')]);
        part(root, 'pool').textContent = t('Pula {0} – {1}', l.data.pool_start, l.data.pool_end) +
          (l.data.running ? '' : ' ' + t('(usługa dhcp zatrzymana)'));
        part(root, 'leases').replaceChildren(App.table([t('Nazwa'), 'MAC', 'IP', t('Pozostało'), t('Typ'), ''], (l.data.leases || []).map((x) => [
          x.host || '-', x.mac, x.ip, remaining(x.remaining), t(TYPES[x.type] || x.type),
          h('span', {},
            x.type === 'dynamiczny' ? h('button', { class: 'small', onclick: async () => {
              await reserve(x.mac, x.ip, validName(x.host) ? x.host : '');
              load();
            } }, t('Zarezerwuj')) : null,
            h('button', { class: 'small secondary', onclick: async () => {
              if (!confirm(t('Zwolnić dzierżawę {0}? Klient dostanie adres na nowo przy odnowieniu.', x.ip))) return;
              await cmd('dhcp release ' + x.mac);
              load();
            } }, t('Zwolnij'))),
        ]), t('Brak aktywnych dzierżaw')));

        const res = r.data.reservations || [];
        part(root, 'res-count').textContent = '(' + res.length + ' / ' + r.data.max + ')';
        part(root, 'reservations').replaceChildren(App.table([t('Nazwa'), 'MAC', 'IP', ''], res.map((x) => [
          x.name || '-', x.mac, x.ip,
          h('span', {},
            h('button', { class: 'small', onclick: () => {
              editing = x;
              macIn.value = x.mac;
              ipIn.value = x.ip;
              nameIn.value = x.name;
              macIn.focus();
            } }, t('Edytuj')),
            h('button', { class: 'small danger', onclick: async () => {
              if (!confirm(t('Usunąć rezerwację {0}?', x.mac))) return;
              await cmd('dhcp unreserve ' + x.mac);
              load();
            } }, t('Usuń'))),
        ]), t('Brak rezerwacji')));
      }
      await load();
    },
  });
})();
