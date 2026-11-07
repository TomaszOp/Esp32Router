// Internet (STA): connection state, upstream network with the "Connect" result, scan.
'use strict';

(() => {
  const { h, api, part } = App;

  const reasonText = App.staReason;
  let state = null;

  function stateText(sta) {
    if (sta.connected) {
      return t('Połączono z „{0}”, adres {1}', sta.ssid, sta.ip) + (sta.rssi !== undefined ? ', ' + t('sygnał {0} dBm', sta.rssi) : '');
    }
    if (!sta.ssid) return t('Sieć nadrzędna nie jest ustawiona');
    return t('Brak połączenia z „{0}”', sta.ssid) + (sta.last_error_code ? ' – ' + t('{0} (ESP ponawia próby)', reasonText(sta)) : '');
  }

  // After "Connect": check the state for up to 20 s and show the result.
  function watchConnect(ssid, msg) {
    const started = Date.now();
    const timer = setInterval(async () => {
      if (App.currentPage() !== 'internet' || !msg.isConnected) {
        clearInterval(timer);
        return;
      }
      const s = await App.refreshStatus();
      if (!s) return;
      const sta = s.sta;
      const elapsed = Date.now() - started;
      if (sta.connected && sta.ssid === ssid) {
        clearInterval(timer);
        App.message(msg, t('Połączono z „{0}”, adres {1}. Zapisz, żeby było trwałe.', ssid, sta.ip), true);
      } else if (sta.last_error_code && elapsed > 3000) {
        clearInterval(timer);
        App.message(msg, t('Nie udało się połączyć z „{0}”: {1}. ESP nadal ponawia próby.', ssid, reasonText(sta)), false);
      } else if (elapsed > 20000) {
        clearInterval(timer);
        App.message(msg, t('Brak połączenia po 20 s. ESP nadal próbuje – sprawdź nazwę sieci i hasło.'), false);
      }
    }, 2000);
  }

  App.page('internet', {
    onStatus(s) {
      if (state && s) state.textContent = stateText(s.sta);
    },
    init(root, s) {
      const sta = s ? s.sta : {};
      state = part(root, 'state');
      state.textContent = stateText(sta);

      const ssidIn = part(root, 'ssid'), passIn = part(root, 'pass'), msg = part(root, 'sta-msg');
      ssidIn.value = sta.ssid || '';
      part(root, 'sta-form').addEventListener('submit', async (e) => {
        e.preventDefault();
        const body = { ssid: ssidIn.value };
        if (passIn.value !== '') body.pass = passIn.value;
        const r = await api('/api/wifi/sta', body);
        App.message(msg, (r.data.output || r.data.error || '').trim(), r.ok && r.data.ok);
        App.refreshStatus();
        if (r.ok && r.data.ok && body.ssid) watchConnect(body.ssid, msg);
      });

      const scanBtn = part(root, 'scan'), scanMsg = part(root, 'scan-msg'), list = part(root, 'networks');
      scanBtn.addEventListener('click', async () => {
        scanBtn.disabled = true;
        App.message(scanMsg, t('Skanowanie... (kilka sekund)'), true);
        const r = await api('/api/wifi/scan');
        scanBtn.disabled = false;
        list.textContent = '';
        if (!r.ok) {
          App.message(scanMsg, r.data.error || t('Błąd skanowania'), false);
          return;
        }
        App.message(scanMsg, t('Znaleziono sieci: {0}', r.data.networks.length), true);
        for (const n of r.data.networks.sort((a, b) => b.rssi - a.rssi)) {
          list.append(h('tr', {}, h('td', {}, n.ssid), h('td', {}, n.rssi + ' dBm'), h('td', {}, n.channel), h('td', {}, n.auth),
            h('td', {}, h('button', { class: 'small', type: 'button', onclick: () => {
              ssidIn.value = n.ssid;
              passIn.value = '';
              passIn.placeholder = n.open ? t('(sieć otwarta)') : t('hasło do {0}', n.ssid);
              passIn.focus();
            } }, t('Wybierz')))));
        }
      });
    },
  });
})();
