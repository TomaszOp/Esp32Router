// Port forwarding: rule table with state; add / edit (POST /api/pf) / enable / disable / delete.
'use strict';

App.page('pf', {
  async init(root) {
    const { h, api, cmd, part } = App;
    const STATES = { aktywna: tn('aktywna'), wylaczona: tn('wyłączona'), blad: tn('błąd') }; // API state -> text (t)
    const f = {};
    for (const k of ['form', 'title', 'name', 'proto', 'ext', 'target', 'targets', 'int', 'src', 'enabled', 'msg']) f[k] = part(root, k);
    let index = -1;

    function resetForm(keepMsg) {
      index = -1;
      f.form.reset();
      f.enabled.checked = true;
      f.title.textContent = t('Nowa reguła');
      if (!keepMsg) f.msg.textContent = '';
    }
    part(root, 'new').addEventListener('click', () => resetForm());

    f.form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const body = { name: f.name.value.trim(), proto: f.proto.value, ext_port: Number(f.ext.value),
        target: f.target.value.trim(), src: f.src.value.replace(/\s/g, ''), enabled: f.enabled.checked };
      if (f.int.value) body.int_port = Number(f.int.value);
      if (index >= 0) body.index = index;
      const r = await api('/api/pf', body);
      App.message(f.msg, (r.data.output || r.data.error || '').trim(), r.ok && r.data.ok);
      if (r.ok && r.data.ok) resetForm(true);
      load();
      App.refreshStatus();
    });

    async function load() {
      const [r, res, cl] = await Promise.all([api('/api/pf'), api('/api/dhcp/reservations'), api('/api/clients')]);
      const d = r.data;
      f.targets.replaceChildren(
        ...(res.data.reservations || []).filter((x) => x.name).map((x) => h('option', { value: x.name }, x.ip)),
        ...(cl.data.clients || []).filter((c) => c.ip).map((c) => h('option', { value: c.ip }, c.name || c.mac)));

      const pending = !d.sta_ip ? t('czeka na IP STA') : t('NAT wyłączony');
      part(root, 'entries').textContent = t('(wpisy {0} / {1})', d.entries, d.max_entries);
      part(root, 'rules').replaceChildren(App.table(['#', t('Nazwa'), t('Protokół'), t('Zewn.'), t('Cel'), t('Wewn.'), t('Źródło'), t('Stan'), ''], (d.rules || []).map((x, i) => [
        i, x.name || '-', x.proto, x.ext_port, x.target + (x.ip && x.ip !== x.target ? ' (' + x.ip + ')' : ''), x.int_port, x.src || t('wszyscy'),
        x.state === 'czeka' ? pending : (x.error || t(STATES[x.state] || x.state)),
        h('span', {},
          h('button', { class: 'small', onclick: () => {
            index = i;
            f.title.textContent = t('Edycja reguły #{0}', i);
            f.name.value = x.name;
            f.proto.value = x.proto;
            f.ext.value = x.ext_port;
            f.target.value = x.target;
            f.int.value = x.int_port;
            f.src.value = x.src || '';
            f.enabled.checked = x.enabled;
            f.name.focus();
          } }, t('Edytuj')),
          h('button', { class: 'small secondary', onclick: async () => {
            await cmd('pf ' + (x.enabled ? 'disable ' : 'enable ') + i);
            load();
          } }, x.enabled ? t('Wyłącz') : t('Włącz')),
          h('button', { class: 'small danger', onclick: async () => {
            if (!confirm(t('Usunąć regułę #{0}?', i))) return;
            await cmd('pf del ' + i);
            resetForm();
            load();
          } }, t('Usuń'))),
      ]), t('Brak reguł')));
    }
    part(root, 'apply').addEventListener('click', async () => { await cmd('pf apply', true); load(); });
    await load();

    // Coming from the client list: target filled in.
    const p = App.params();
    if (p.target) {
      f.target.value = p.target;
      f.ext.focus();
    }
  },
});
