// Wi-Fi (AP): SSID and password of the AP network (immediately), other settings in the form from wifi.html.
'use strict';

App.page('wifi', {
  init(root, s) {
    const ssidIn = App.part(root, 'ssid'), passIn = App.part(root, 'pass'), msg = App.part(root, 'msg');
    ssidIn.value = s ? s.ap.ssid : '';
    App.part(root, 'ap-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      if (!confirm(t('Zmiana sieci AP rozłączy wszystkich klientów AP, także to połączenie.') + '\n\n' +
                   t('Potem połącz się z nową siecią, otwórz panel ponownie i kliknij Zapisz – inaczej po restarcie wróci stara sieć.'))) return;
      const body = { ssid: ssidIn.value };
      if (passIn.value !== '') body.pass = passIn.value;
      // The response may not arrive – the AP restarts meanwhile.
      App.message(msg, t('Zmieniam sieć AP... Połącz się z „{0}”, otwórz panel i kliknij Zapisz.', body.ssid), true);
      const r = await App.api('/api/wifi/ap', body);
      if (r.status !== 0) {
        App.message(msg, (r.data.output || r.data.error || '').trim() +
          (r.ok && r.data.ok ? '\n' + t('Połącz się z nową siecią, otwórz panel i kliknij Zapisz.') : ''), r.ok && r.data.ok);
      }
      App.refreshStatus();
    });
  },
});
