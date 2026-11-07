// DDNS: state (public IP, accounts), check and update, account fields depending on the service.
'use strict';

(() => {
  // Which fields to show for a service and a short instruction.
  const PROVIDERS = {
    duckdns: { fields: ['hostname', 'secret'], hint: tn('duckdns.org: nazwa = subdomena (np. mojrouter), token ze strony konta.') },
    dynu: { fields: ['hostname', 'username', 'secret'], hint: tn('dynu.com: pełna nazwa hosta, login i hasło (albo hasło IP Update).') },
    noip: { fields: ['hostname', 'username', 'secret'],
      hint: tn('noip.com: pełna nazwa hosta, login i hasło albo klucz DDNS. Darmowy host trzeba potwierdzać co 30 dni (e-mail od No-IP).') },
    freedns: { fields: ['hostname', 'secret'], hint: tn('freedns.afraid.org: token z adresu "Dynamic Update" (wersja 2); nazwa tylko informacyjnie.') },
    desec: { fields: ['hostname', 'secret'], hint: tn('desec.io: pełna nazwa (np. mojrouter.dedyn.io) i token.') },
    custom: { fields: ['hostname', 'username', 'secret', 'custom_url'],
      hint: tn('Własny serwis (np. zgodny z dyndns2): adres z polami {host} {ip} {user} {pass}; login włącza logowanie Basic.') },
  };
  const ALL = ['hostname', 'username', 'secret', 'custom_url'];
  let root = null;

  function applyProvider(form) {
    const n = form.dataset.account;
    const sel = form.querySelector('[data-field="' + n + '.provider"] select');
    if (!sel) return;
    const p = PROVIDERS[sel.value] || PROVIDERS.custom;
    for (const f of ALL) {
      const row = form.querySelector('[data-field="' + n + '.' + f + '"]');
      if (row) row.hidden = !p.fields.includes(f);
    }
    App.part(root, 'hint-' + n).textContent = t(p.hint);
  }

  function draw(d) {
    if (!root || !d) return;
    const { h } = App;
    const rows = [
      [t('Usługa'), d.running ? t('działa') : t('zatrzymana (System → Usługi → ddns)')],
      [t('Źródło adresu'), t({ public: tn('publiczny IP'), provider: tn('wykrywa serwis'), sta: tn('adres STA') }[d.ip_source] || d.ip_source)],
    ];
    if (d.ip_source === 'public') {
      rows.push([t('Publiczny IP'), (d.public_ip || '-') + (d.last_check_s >= 0 ? ' ' + t('(sprawdzono {0} s temu)', d.last_check_s) : '')]);
      if (d.check_error) rows.push([t('Błąd sprawdzania'), d.check_error]);
      if (d.fallback) rows.push([t('Uwaga'), t('sprawdzanie nie działa – aktualizacje przez serwis (max raz na godzinę)')]);
      if (d.running) rows.push([t('Następne sprawdzenie'), t('za {0} s', d.next_check_s)]);
    }
    App.part(root, 'state').replaceChildren(...rows.flatMap(([k, v]) => [h('dt', {}, k), h('dd', {}, v)]));
    for (const a of d.accounts) {
      const el = App.part(root, 'result-' + a.slot);
      if (!el) continue;
      el.textContent = a.empty ? t('(puste)') : !a.enabled ? t('(wyłączone)') : a.problem ? '– ' + t('niekompletne: {0}', a.problem)
        : a.result ? '– ' + t('{0}, ostatnio OK: {1}', a.result, a.last_ok) : '– ' + t('jeszcze nie aktualizowane');
      el.className = a.fatal ? 'warn-text' : 'muted';
    }
  }

  async function load() {
    const r = await App.api('/api/ddns');
    if (r.ok) draw(r.data);
  }

  async function run(cmd) {
    const msg = App.part(root, 'msg');
    App.message(msg, t('Proszę czekać...'), true);
    const r = await App.cmd(cmd, true);
    const out = (r.data.output || r.data.error || '').replace(/\r/g, '').replace(/^(Proszę czekać|Please wait)\.\.\.\n/, '').trim();
    App.message(msg, out, r.ok && r.data.ok);
    load();
  }

  App.page('ddns', {
    async init(r, s) {
      root = r;
      for (const form of root.querySelectorAll('form[data-account]')) {
        applyProvider(form);
        const sel = form.querySelector('select');
        if (sel) sel.addEventListener('change', () => applyProvider(form));
      }
      App.part(root, 'check').addEventListener('click', () => run('ddns test'));
      App.part(root, 'update-all').addEventListener('click', () => run('ddns update'));
      draw(s && s.ddns);
      await load();
    },
    onStatus(s) { if (s && s.ddns) draw(s.ddns); },
  });
})();
