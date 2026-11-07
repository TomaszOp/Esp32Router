// Filtering: domain blocking – on/off, paged list, add/delete, check, file import.
'use strict';

App.page('filtr', {
  async init(root) {
    const { h, api, cmd, part } = App;
    const PAGE = 50;
    let offset = 0;

    async function loadStats() {
      const r = await api('/api/dns/stats');
      const b = r.data.block;
      part(root, 'enabled').checked = b.enabled;
      part(root, 'response').value = b.response;
      part(root, 'summary').textContent = t('Na liście: {0} domen. Zablokowane zapytania: {1}.', b.domains, r.data.queries.blocked) +
        (b.dirty ? ' ' + t('Lista ma niezapisane zmiany.') : '');
      part(root, 'top').replaceChildren(App.table([t('Domena'), t('Blokad')], b.top.map((x) => [x.domain, x.hits]), t('Jeszcze nic nie zablokowano')));
    }

    async function loadList() {
      const q = encodeURIComponent(part(root, 'search').value.trim().toLowerCase());
      const r = await api('/api/dns/block?q=' + q + '&offset=' + offset + '&limit=' + PAGE);
      const d = r.data;
      part(root, 'list').replaceChildren(App.table([t('Domena'), t('Blokad'), ''], d.items.map((x) => [
        x.domain, x.hits,
        h('button', { class: 'small danger', onclick: async () => {
          await cmd('dns block del ' + x.domain, true);
          loadList();
          loadStats();
          App.refreshStatus();
        } }, t('Usuń')),
      ]), d.total ? t('Nic nie pasuje') : t('Lista jest pusta')));
      const pager = part(root, 'pager');
      pager.replaceChildren(
        (d.matches ? t('{0}-{1} z {2}', offset + 1, Math.min(offset + PAGE, d.matches), d.matches) : '0') + ' ' + t('(cała lista: {0})', d.total) + ' ',
        offset > 0 ? h('button', { class: 'small secondary', onclick: () => { offset = Math.max(0, offset - PAGE); loadList(); } }, '◀ ' + t('Poprzednie')) : '',
        offset + PAGE < d.matches ? h('button', { class: 'small secondary', onclick: () => { offset += PAGE; loadList(); } }, t('Następne') + ' ▶') : '');
    }

    async function setOption(changes, text) {
      const r = await api('/api/config/dns', changes);
      App.message(part(root, 'msg'), r.data.ok ? text + ' – ' + t('działa od razu, trwałe po Zapisz.') : t('Błąd'), r.data.ok);
      App.refreshStatus();
    }
    part(root, 'enabled').addEventListener('change', (e) => setOption({ 'block.enabled': e.target.checked },
      e.target.checked ? t('Blokowanie włączone') : t('Blokowanie wyłączone')));
    part(root, 'response').addEventListener('change', (e) => setOption({ 'block.response': e.target.value }, t('Zmieniono odpowiedź')));

    let searchTimer = null;
    part(root, 'search').addEventListener('input', () => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(() => { offset = 0; loadList(); }, 300);
    });
    part(root, 'search-form').addEventListener('submit', (e) => e.preventDefault());

    part(root, 'add-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const d = part(root, 'add').value.trim();
      if (/\s/.test(d)) return;
      const r = await cmd('dns block add ' + d, true);
      App.message(part(root, 'add-msg'), (r.data.output || '').trim() || t('Nieprawidłowa domena'), r.ok && r.data.ok);
      if (r.ok && r.data.ok) part(root, 'add').value = '';
      loadList();
      loadStats();
      App.refreshStatus();
    });

    part(root, 'check-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const d = part(root, 'check').value.trim();
      if (/\s/.test(d)) return;
      const r = await cmd('dns block check ' + d, true);
      App.message(part(root, 'check-msg'), (r.data.output || '').trim() || t('Nieprawidłowa nazwa'), r.ok && r.data.ok);
    });

    part(root, 'import').addEventListener('click', () => {
      const file = part(root, 'file').files[0];
      const msg = part(root, 'import-msg');
      if (!file) {
        App.message(msg, t('Wybierz plik'), false);
        return;
      }
      const replace = part(root, 'replace').checked;
      if (replace && !confirm(t('Zastąpić całą listę zawartością pliku?'))) return;
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/dns/block/import?mode=' + (replace ? 'replace' : 'append'));
      xhr.setRequestHeader('X-Requested-With', 'esp32router');
      xhr.setRequestHeader('Content-Type', 'text/plain');
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) part(root, 'bar').style.width = Math.round(100 * e.loaded / e.total) + '%';
      };
      xhr.onload = () => {
        let d = {};
        try { d = JSON.parse(xhr.responseText); } catch (err) { /* brak treści */ }
        if (xhr.status === 200 && d.ok) {
          App.message(msg, t('Na liście {0} domen (zmiana {1}).', d.domains, (d.added >= 0 ? '+' : '') + d.added) + ' ' +
            (d.too_big ? t('UWAGA: lista jest za duża do zapisania na urządzeniu ({0} KB) – działa do restartu.',
              Math.round(d.file_size / 1024)) : t('Zapisz, żeby była trwała.')), !d.too_big);
        } else {
          App.message(msg, d.error || t('Błąd importu ({0})', xhr.status), false);
        }
        offset = 0;
        loadList();
        loadStats();
        App.refreshStatus();
      };
      xhr.onerror = () => App.message(msg, t('Przerwane połączenie'), false);
      xhr.send(file);
    });

    await Promise.all([loadStats(), loadList()]);
  },
});
