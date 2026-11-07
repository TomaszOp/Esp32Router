// Services: start / stop / restart and autostart (global.json services.*).
'use strict';

App.page('uslugi', {
  async init(root) {
    const { h, api, cmd, toast } = App;
    const body = App.part(root, 'services');
    async function load() {
      const r = await api('/api/services');
      if (!r.ok) return;
      body.textContent = '';
      for (const s of r.data.services) {
        const auto = h('input', { type: 'checkbox' });
        auto.checked = s.autostart;
        auto.addEventListener('change', async () => {
          const res = await api('/api/config/global', { ['services.' + s.name]: auto.checked });
          toast(res.data.ok ? t('Autostart {0}: {1} (po Zapisz)', s.name, auto.checked ? t('tak') : t('nie')) : t('Błąd'), !res.data.ok);
          App.refreshStatus();
        });
        const action = (verb) => async () => {
          if (verb !== 'start' && (s.name === 'web' || s.console) &&
              !confirm(verb === 'stop' ? t('Zatrzymać usługę {0}?', s.name) : t('Zrestartować {0}?', s.name))) return;
          await cmd('service ' + verb + ' ' + s.name);
          setTimeout(load, 800);
        };
        body.append(h('tr', {},
          h('td', {}, s.name),
          h('td', {}, h('span', { class: 'dot ' + (s.running ? 'on' : 'off') }), s.running ? t('działa') : t('zatrzymana'),
            h('div', { class: 'muted' }, s.details)),
          h('td', {}, auto),
          h('td', {},
            h('button', { class: 'small', onclick: action('start'), disabled: s.running }, 'Start'),
            h('button', { class: 'small secondary', onclick: action('stop'), disabled: !s.running }, 'Stop'),
            h('button', { class: 'small secondary', onclick: action('restart') }, 'Restart'))));
      }
    }
    await load();
  },
});
