// Status: device overview, refreshed together with the header (every 5 s).
'use strict';

(() => {
  const { h } = App;
  let tiles = null;

  function kv(pairs) {
    return h('dl', { class: 'kv' }, pairs.filter(Boolean).flatMap(([k, v]) => [h('dt', {}, k), h('dd', {}, v)]));
  }

  function draw(s) {
    if (!tiles || !s) return;
    const sta = s.sta, ap = s.ap, m = s.memory;
    tiles.textContent = '';
    tiles.append(
      h('div', { class: 'card' }, h('h2', {}, t('Urządzenie')), kv([
        [t('Nazwa'), s.device.hostname],
        [t('Wersja'), s.device.version],
        [t('Czas pracy'), App.formatUptime(s.device.uptime)],
        [t('Czas'), s.device.time + (s.device.time_synced ? '' : ' ' + t('(bez NTP)'))],
      ])),
      h('div', { class: 'card' }, h('h2', {}, h('span', { class: 'dot ' + (sta.connected ? 'on' : 'off') }), t('Internet (STA)')), kv([
        [t('Sieć'), sta.ssid || t('(nie ustawiono)')],
        [t('Stan'), sta.connected ? t('połączono') : t('brak połączenia') + (sta.last_error_code ? ' (' + App.staReason(sta) + ')' : '')],
        sta.connected && [t('Adres IP'), sta.ip + ' / ' + sta.mask],
        sta.connected && [t('Brama'), sta.gateway],
        sta.connected && ['DNS', sta.dns],
        sta.rssi !== undefined && [t('Sygnał'), t('{0} dBm, kanał {1}', sta.rssi, sta.channel)],
        s.ddns && s.ddns.running && ['DDNS', (s.ddns.public_ip ? t('publiczny IP {0}', s.ddns.public_ip) : t('działa')) +
          (s.ddns.accounts.some((x) => x.fatal) ? ' – ' + t('błąd konta') : '')],
      ])),
      h('div', { class: 'card' }, h('h2', {}, t('Wi-Fi (AP)')), kv([
        [t('Sieć'), ap.ssid + (ap.open ? ' ' + t('(OTWARTA)') : '')],
        [t('Adres'), ap.ip + ' / ' + ap.mask],
        [t('Klienci'), ap.clients + ' / ' + ap.max_clients],
        ['NAT', s.nat ? t('włączony') : t('wyłączony')],
        s.firewall && ['Firewall', s.firewall.running
          ? t('działa, reguł {0}, zablokowano {1}', s.firewall.rules, s.firewall.blocked) : t('zatrzymany')],
      ])),
      h('div', { class: 'card' }, h('h2', {}, t('Pamięć')), kv([
        [t('SRAM wolne'), t('{0} z {1}', App.formatBytes(m.sram_free), App.formatBytes(m.sram_total))],
        [t('SRAM minimum'), App.formatBytes(m.sram_min)],
        [t('PSRAM wolne'), m.psram_total ? t('{0} z {1}', App.formatBytes(m.psram_free), App.formatBytes(m.psram_total)) : t('brak')],
      ])),
      h('div', { class: 'card' }, h('h2', {}, t('Usługi')), kv(s.services.map((x) => [
        h('span', {}, h('span', { class: 'dot ' + (x.running ? 'on' : 'off') }), x.name),
        (x.running ? t('działa') : t('zatrzymana')) + (x.details ? ' –' + x.details : ''),
      ])), h('a', { href: '#/uslugi' }, t('Zarządzaj usługami'))));
  }

  App.page('stan', {
    init(root, s) {
      tiles = App.part(root, 'tiles');
      draw(s);
    },
    onStatus: draw,
  });
})();
