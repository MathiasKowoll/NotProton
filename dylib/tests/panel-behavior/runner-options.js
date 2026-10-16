// The options a game's runner declares, rendered after NotProton's own.
'use strict';
const { panel, walk, details, runner, FORMS } = require('./harness');

const emit = process.argv[2];
if (!emit) { console.error('usage: runner-options.js <emit>'); process.exit(2); }

const OPTIONS = {
  notproton: {
    section: 'Pad',
    options: [
      { label: 'Vibration', name: 'PAD_VIBRATION',
        choices: [{ label: 'Modern', value: '' }, { label: 'Legacy', value: 'legacy' }] },
      { label: 'Quotes "and" back\\slash  </script>', name: 'PAD_ODD',
        choices: [{ label: 'Off', value: '' }] },
    ],
  },
  'notproton-other': { options: [{ label: 'Elsewhere', name: 'OTHER', choices: [{ label: 'x', value: '' }] }] },
};

let failed = 0;
for (const form of Object.keys(FORMS)) {
  const t = runner(form);
  const { render: P, written } = panel(emit, form, undefined, { options: OPTIONS });
  const nodes = (opts, over) => walk(P({ details: details(opts + ' %command%', over) }));
  const rows = ns => ns.filter(x => x.type === 'Section' && /^Pad: |^Elsewhere/.test(x.props.label));
  const last = () => (written.length ? written[written.length - 1].opts : '');

  let r = rows(nodes(''));
  t.ok(r.map(x => x.props.label).join('|') === 'Pad: Vibration|Pad: Quotes "and" back\\slash  </script>',
       'the runner of this tool gets one row per option, its text intact');
  t.ok(r[0].props.children.props.selectedOption === '', 'an unset option shows its empty choice');
  t.ok(rows(nodes('PAD_VIBRATION=legacy'))[0].props.children.props.selectedOption === 'legacy',
       'a set option shows its value');

  r[0].props.children.props.onChange({ data: 'legacy' });
  t.ok(/PAD_VIBRATION=legacy %command%/.test(last()),
       `choosing a value writes it ahead of %command% (${last()})`);
  rows(nodes('PAD_VIBRATION=legacy'))[0].props.children.props.onChange({ data: '' });
  t.ok(!last().includes('PAD_VIBRATION'), `the empty choice removes the variable (${last()})`);

  t.ok(rows(nodes('', { strCompatToolName: 'notproton-none' })).length === 0,
       'a tool whose runner declares nothing gets no rows');

  const bare = panel(emit, form).render;
  t.ok(walk(bare({ details: details(' %command%') })).every(x => !/^Pad: /.test(x.props.label || '')),
       'without the file, the panel is as before');
  failed += t.failed;
}
process.exit(failed ? 1 : 0);
