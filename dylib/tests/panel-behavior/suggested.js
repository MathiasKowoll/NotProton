// The launch options measured for a game: one switch, shown only for a game the
// file names, writing all of them or removing all of them.
'use strict';
const fs = require('fs');
const path = require('path');
const { panel, walk, details, runner, FORMS } = require('./harness');

const emit = process.argv[2];
if (!emit) { console.error('usage: suggested.js <emit>'); process.exit(2); }
const file = JSON.parse(fs.readFileSync(path.join(__dirname, '../../feats/suggested-options.json'), 'utf8'));

const doc = lang => ({ document: { documentElement: { lang } } });
let failed = 0;
for (const form of Object.keys(FORMS)) {
  const t = runner(form);
  for (const [lang, zh] of [['en', false], ['zh-CN', true]]) {
    const { render: P, written } = panel(emit, form, doc(lang));
    const nodes = (appid, opts) => walk(P({ details: details(opts + ' %command%', { unAppID: appid }) }));
    const section = ns => ns.find(x => x.type === 'Section' && x.props.label ===
      (zh ? '为此游戏推荐' : 'Suggested for this game'));
    const row = (appid, opts) => {
      const s = section(nodes(appid, opts));
      return s && walk(s.props.children).find(x => x.type === 'Toggle');
    };
    const last = () => (written.length ? written[written.length - 1].opts : '');
    t.ok(!section(nodes(287700, '')), `${lang}: no section for a game the file does not name`);
    for (const [appid, entry] of Object.entries(file)) {
      const want = Object.entries(entry.options);
      const all = want.map(([k, v]) => `${k}=${v}`).join(' ');
      const r = row(+appid, '');
      t.ok(r && r.props.label === (zh && entry.summary_zh || entry.summary) && r.props.checked === false,
           `${lang}: ${entry.title} offers its summary, off`);
      written.length = 0;
      r.props.onChange(true);
      t.ok(want.every(([k, v]) => last().includes(`${k}=${v}`)), `${lang}: on writes ${all} (${last()})`);
      const on = row(+appid, last().replace(/ ?%command%$/, ''));
      t.ok(on.props.checked === true, `${lang}: ${entry.title} reads as on once written`);
      written.length = 0;
      on.props.onChange(false);
      t.ok(!want.some(([k]) => last().includes(k + '=')), `${lang}: off removes them all (${last()})`);
      // Anything else in the launch options is the user's and stays.
      written.length = 0;
      row(+appid, 'MTL_HUD_ENABLED=1').props.onChange(true);
      t.ok(last().includes('MTL_HUD_ENABLED=1'), `${lang}: other options stay (${last()})`);
      const [k0] = want[0];
      t.ok(row(+appid, all.replace(new RegExp(k0 + '=\\S+'), k0 + '=other')).props.checked === false,
           `${lang}: one differing value reads as off`);
    }
  }
  // The file in Application Support, read at Steam start, replaces the built-in
  // suggestion for each game it names and adds games, without a rebuild. An entry
  // the panel could not write as it stands is ignored, and so is a broken file.
  const withFile = (text, appid, opts) => {
    const { render: P, written } = panel(emit, form, doc('en'), { suggested: text });
    const ns = walk(P({ details: details((opts || '') + ' %command%', { unAppID: appid }) }));
    const s = ns.find(x => x.type === 'Section' && x.props.label === 'Suggested for this game');
    return { row: s && walk(s.props.children).find(x => x.type === 'Toggle'), written };
  };
  const entry = (options, extra) => Object.assign({ summary: 'From the file', options }, extra || {});
  const [someId, someEntry] = Object.entries(file)[0];
  let got = withFile(JSON.stringify({ [someId]: entry({ NOTPROTON_PAD_XINPUT: '1' }) }), +someId);
  t.ok(got.row && got.row.props.label === 'From the file', 'the file replaces a built-in entry');
  got.row.props.onChange(true);
  const opts = got.written[got.written.length - 1].opts;
  t.ok(opts.includes('NOTPROTON_PAD_XINPUT=1') &&
       !Object.keys(someEntry.options).some(k => k !== 'NOTPROTON_PAD_XINPUT' && opts.includes(k + '=')),
       `and writes only its own options (${opts})`);
  got = withFile(JSON.stringify({ 4242: entry({ DSB_DISABLED: '1' }) }), 4242);
  t.ok(got.row && got.row.props.label === 'From the file', 'the file adds a game the build does not name');
  t.ok(withFile(JSON.stringify({ 4242: entry({ DSB_DISABLED: '1' }) }), +someId).row.props.label ===
       someEntry.summary, 'a game the file does not name keeps the built-in entry');
  for (const [what, text] of [
    ['a variable outside the list', JSON.stringify({ [someId]: entry({ DYLD_INSERT_LIBRARIES: '/tmp/x' }) })],
    ['WINEDLLOVERRIDES', JSON.stringify({ [someId]: entry({ WINEDLLOVERRIDES: 'dinput8=n' }) })],
    ['a value needing quotes', JSON.stringify({ [someId]: entry({ DSB_DISABLED: '1; rm' }) })],
    ['an empty value', JSON.stringify({ [someId]: entry({ DSB_DISABLED: '' }) })],
    ['a number', JSON.stringify({ [someId]: entry({ DSB_DISABLED: 1 }) })],
    ['no options', JSON.stringify({ [someId]: entry({}) })],
    ['options as a list', JSON.stringify({ [someId]: entry(['NOTPROTON_PAD_XINPUT']) })],
    ['no summary', JSON.stringify({ [someId]: { options: { DSB_DISABLED: '1' } } })],
    ['a blank summary', JSON.stringify({ [someId]: entry({ DSB_DISABLED: '1' }, { summary: ' ' }) })],
    ['a summary_zh that is not text', JSON.stringify({ [someId]: entry({ DSB_DISABLED: '1' }, { summary_zh: 1 }) })],
    ['a broken file', '{"' + someId + '": {'],
    ['a list', '[1, 2]'],
    ['text', 'not json'],
  ]) {
    const r = withFile(text, +someId).row;
    t.ok(r && r.props.label === someEntry.summary, `${what} in the file is ignored for the built-in entry`);
  }
  t.ok(!withFile(JSON.stringify({ 4242: entry({ WINEDLLOVERRIDES: 'x=n' }) }), 4242).row,
       'an ignored entry for a new game shows nothing');
  failed += t.failed;
}
console.log(failed ? `\n${failed} failure(s)` : '\nboth shapes pass');
process.exit(failed ? 1 : 0);
