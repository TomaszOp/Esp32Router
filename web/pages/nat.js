// NAT: on/off (immediately) and connection table usage.
'use strict';

App.page('nat', {
  async init(root) {
    const { api, part } = App;
    const toggle = part(root, 'toggle'), msg = part(root, 'msg');
    const r = await api('/api/nat');
    const n = r.data;
    const used = (n.tcp || 0) + (n.udp || 0) + (n.icmp || 0);
    toggle.checked = n.wanted;
    part(root, 'state').textContent = n.enabled ? t('Działa.')
      : (n.wanted ? t('Włączy się po połączeniu z siecią nadrzędną.') : t('Wyłączony – klienci AP nie mają internetu.'));
    if (n.tcp !== undefined) {
      part(root, 'table').hidden = false;
      part(root, 'used').textContent = used + ' / ' + n.max;
      part(root, 'protos').textContent = n.tcp + ' / ' + n.udp + ' / ' + n.icmp;
      part(root, 'evictions').textContent = n.evictions;
    }
    part(root, 'bar').style.width = Math.min(100, Math.round(100 * used / n.max)) + '%';
    toggle.addEventListener('change', async () => {
      const res = await api('/api/config/nat', { enabled: toggle.checked });
      App.message(msg, res.data.ok ? (toggle.checked ? t('NAT włączony – działa od razu, trwały po Zapisz.') : t('NAT wyłączony – działa od razu, trwały po Zapisz.')) : t('Błąd'),
        res.data.ok);
      App.refreshStatus();
    });
  },
});
