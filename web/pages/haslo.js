// Panel password: change (POST /api/passwd); other sessions are logged out.
'use strict';

App.page('haslo', {
  init(root) {
    const { part } = App;
    const form = part(root, 'form'), msg = part(root, 'msg');
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      if (part(root, 'new').value !== part(root, 'new2').value) {
        App.message(msg, t('Nowe hasła nie są takie same'), false);
        return;
      }
      const r = await App.api('/api/passwd', { old: part(root, 'old').value, new: part(root, 'new').value });
      App.message(msg, r.ok ? t('Hasło zmienione. Inne sesje zostały wylogowane.') : (r.data.error || t('Błąd')), r.ok);
      if (r.ok) {
        form.reset();
        App.refreshStatus();
      }
    });
  },
});
