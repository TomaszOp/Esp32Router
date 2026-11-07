// Configuration: unsaved changes, backup, restore, factory reset.
'use strict';

App.page('konfiguracja', {
  async init(root) {
    const { h, api, cmd, part } = App;
    const r = await api('/api/config/diff');
    const changes = (r.data && r.data.changes) || [];
    const fmt = (v) => (typeof v === 'string' ? v : JSON.stringify(v));
    part(root, 'diff').replaceChildren(App.table([t('Moduł'), t('Pole'), t('Zapisane'), t('Bieżące'), ''], changes.map((c) => [
      c.module, c.path, fmt(c.old), fmt(c.new), c.live ? '' : h('span', { class: 'badge' }, t('po restarcie'))]),
      t('Brak – konfiguracja w pamięci zgadza się z plikami.')));
    const save = part(root, 'save'), revert = part(root, 'revert');
    save.disabled = revert.disabled = !changes.length;
    save.addEventListener('click', App.saveAll);
    revert.addEventListener('click', App.revertAll);

    const file = part(root, 'file'), msg = part(root, 'restore-msg');
    part(root, 'restore').addEventListener('click', async () => {
      if (!file.files.length) {
        App.message(msg, t('Wybierz plik kopii'), false);
        return;
      }
      if (!confirm(t('Przywrócić konfigurację z pliku? Bieżące ustawienia zostaną zastąpione, urządzenie zrestartuje się.'))) return;
      let backup;
      try {
        backup = JSON.parse(await file.files[0].text());
      } catch (e) {
        App.message(msg, t('Plik nie jest poprawnym JSON'), false);
        return;
      }
      const res = await api('/api/restore', backup);
      if (!res.ok) {
        App.message(msg, res.data.error || t('Błąd przywracania'), false);
        return;
      }
      App.waitForReboot(msg, t('Przywrócono plików: {0}, restart.', res.data.files));
    });

    part(root, 'reset').addEventListener('click', () => {
      if (!confirm(t('Na pewno usunąć całą konfigurację?'))) return;
      cmd('config factory-reset yes', true);
      App.waitForReboot(part(root, 'reset-msg'), t('Reset fabryczny, restart.'));
    });
  },
});
