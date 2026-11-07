// Clients: connected devices with the "Reserve address" and "Forward port" actions.
'use strict';

App.page('klienci', {
  async init(root) {
    const { h, api, cmd, part } = App;
    const TYPES = { dynamiczny: tn('dynamiczny'), rezerwacja: tn('rezerwacja') }; // API type -> text (t)
    async function load() {
      const r = await api('/api/clients');
      const list = r.data.clients || [];
      part(root, 'count').textContent = '(' + list.length + ')';
      part(root, 'list').replaceChildren(App.table([t('Nazwa'), 'MAC', 'IP', t('Sygnał'), t('Adres'), ''], list.map((c) => [
        c.name || '-', c.mac, c.ip || '-', c.rssi + ' dBm', t(TYPES[c.type] || c.type),
        h('span', {},
          c.type === 'dynamiczny' ? h('button', { class: 'small', onclick: async () => {
            await cmd('dhcp reserve ' + c.mac + ' ' + c.ip + (/^[A-Za-z0-9-]{1,32}$/.test(c.name) ? ' ' + c.name : ''));
            load();
          } }, t('Zarezerwuj adres')) : null,
          c.ip ? h('a', { href: '#/pf?target=' + encodeURIComponent(c.type === 'rezerwacja' && c.name ? c.name : c.ip) },
            h('button', { class: 'small secondary' }, t('Przekieruj port'))) : null),
      ]), t('Brak podłączonych urządzeń')));
    }
    part(root, 'refresh').addEventListener('click', load);
    await load();
  },
});
