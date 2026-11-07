// Firmware: OTA partitions, .bin upload with progress, restart.
'use strict';

App.page('firmware', {
  async init(root) {
    const { h, api, part } = App;
    const STATES = { nowy: tn('nowy'), 'do potwierdzenia': tn('do potwierdzenia'), poprawny: tn('poprawny'),
      odrzucony: tn('odrzucony'), przerwany: tn('przerwany') }; // API partition state -> text (t)
    const r = await api('/api/sys/ota');
    const row = (title, p) => p ? h('tr', {}, h('td', {}, title), h('td', {}, p.label),
      h('td', {}, p.version ? p.version + ' (' + p.date + ' ' + p.time + ')' : t('(pusta)')),
      h('td', {}, t(STATES[p.state] || p.state) + (p.boot ? ', ' + t('startowa') : ''))) : '';
    part(root, 'partitions').replaceChildren(row(t('Uruchomiona'), r.data.running), row(t('Do aktualizacji'), r.data.next));

    const file = part(root, 'file'), bar = part(root, 'bar'), msg = part(root, 'msg'), btn = part(root, 'upload');
    btn.addEventListener('click', () => {
      if (!file.files.length) {
        App.message(msg, t('Wybierz plik firmware.bin'), false);
        return;
      }
      if (!confirm(t('Wgrać {0} i zrestartować urządzenie?', file.files[0].name))) return;
      btn.disabled = true;
      // XMLHttpRequest – upload progress (fetch does not provide it).
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('X-Requested-With', 'esp32router');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const pct = Math.round(100 * e.loaded / e.total);
          bar.style.width = pct + '%';
          App.message(msg, t('Wysyłanie {0}%', pct), true);
        }
      };
      xhr.onload = () => {
        let data = {};
        try { data = JSON.parse(xhr.responseText); } catch (e) { /* brak treści */ }
        if (xhr.status === 200 && data.ok) {
          App.waitForReboot(msg, t('Wgrano {0} do {1}, restart.', data.version, data.partition));
        } else {
          btn.disabled = false;
          App.message(msg, data.error || t('Błąd wgrywania ({0})', xhr.status), false);
        }
      };
      xhr.onerror = () => {
        btn.disabled = false;
        App.message(msg, t('Przerwane połączenie'), false);
      };
      xhr.send(file.files[0]);
    });

    part(root, 'reboot').addEventListener('click', async () => {
      if (!confirm(t('Zrestartować urządzenie?'))) return;
      await api('/api/reboot', {});
      App.waitForReboot(part(root, 'reboot-msg'), t('Restart.'));
    });
  },
});
