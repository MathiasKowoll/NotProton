// The bridged pad's rows: shown only while games read controllers directly, and
// every choice written as the launch option the launcher reads.
'use strict';
const { panel, walk, details, runner, FORMS } = require('./harness');

const emit = process.argv[2];
if (!emit) { console.error('usage: controllers.js <emit>'); process.exit(2); }

const RAW = 'NOTPROTON_RAW_CONTROLLERS=1';
// The grammar the broker accepts (DSBParseTriggers), for the values the panel
// itself composes: canonical names, upper-case sides, each side once.
const kind = '(resist|weapon|slope|vibrate|rumble)(@[1-8])?';
const side = (s, o) => `(${s}:${kind}(,${s}:${kind}\\+${o})?|${s}:${kind}\\+${o})`;
const grammar = new RegExp(`^(${side('R2', 'L2')}(,${side('L2', 'R2')})?|${side('L2', 'R2')})$`);

let failed = 0;
for (const form of Object.keys(FORMS)) {
  const { render: P, written } = panel(emit, form);
  const t = runner(form);
  const nodes = opts => walk(P({ details: details(opts + ' %command%') }));
  const section = (ns, label) => ns.find(x => x.type === 'Section' && x.props.label === label);
  const inside = (ns, label) => walk(section(ns, label).props.children);
  const dropdowns = (ns, label) => inside(ns, label).filter(x => x.type === 'Dropdown');
  const last = () => (written.length ? written[written.length - 1].opts : '');
  const pick = (opts, label, index, data) => {
    written.length = 0;
    dropdowns(nodes(opts), label)[index].props.onChange({ data });
    return last();
  };
  const triggers = opts => (/NOTPROTON_PAD_TRIGGERS=(\S+)/.exec(opts) || [])[1] || '';

  // Without direct access the pad belongs to Steam: no bridged rows, and the
  // forwarding of Steam's virtual pad rumble can be turned off.
  let r = nodes('');
  t.ok(!section(r, 'Controller the game sees') && !section(r, 'R2 effect'),
       'the bridged rows stay hidden without direct access');
  const steam = opts => nodes(opts).find(x => x.props.label === 'Vibration through Steam Input');
  t.ok(steam('').props.checked === true, 'Steam Input vibration reads as on by default');
  t.ok(steam('NOTPROTON_STEAM_RUMBLE=0').props.checked === false, 'and as off from its 0');
  written.length = 0;
  steam('').props.onChange(false);
  t.ok(last().trim() === 'NOTPROTON_STEAM_RUMBLE=0 %command%', `turning it off writes 0 (${last()})`);
  steam('NOTPROTON_STEAM_RUMBLE=0').props.onChange(true);
  t.ok(!last().includes('NOTPROTON_STEAM_RUMBLE'), `turning it on removes the flag (${last()})`);

  r = nodes(RAW);
  t.ok(!steam(RAW), 'the Steam Input vibration row leaves with direct access');
  t.ok(['Controller the game sees', 'Vibration', 'R2 effect', 'L2 effect'].every(l => section(r, l)),
       'direct access shows the pad, vibration and both trigger sections');

  // Which pad the game sees: one dropdown over two flags.
  const pad = opts => dropdowns(nodes(opts), 'Controller the game sees')[0];
  t.ok(pad(RAW).props.rgOptions.map(o => o.data).join(',') === ',1,0,steam', 'four ways to present the pad');
  t.ok(pad(RAW).props.selectedOption === '', 'automatic by default');
  t.ok(pad(RAW + ' NOTPROTON_PAD_XINPUT=1').props.selectedOption === '1', 'reads the XInput override');
  t.ok(pad(RAW + ' NOTPROTON_PAD_XINPUT=1 NOTPROTON_PAD_STEAM_INPUT=1').props.selectedOption === 'steam',
       'the Steam Input pad wins, as in the run script');
  let w = pick(RAW + ' NOTPROTON_PAD_XINPUT=1', 'Controller the game sees', 0, 'steam');
  t.ok(w.includes('NOTPROTON_PAD_STEAM_INPUT=1') && !w.includes('NOTPROTON_PAD_XINPUT'),
       `picking Steam Input sets its flag and clears the override (${w})`);
  w = pick(RAW + ' NOTPROTON_PAD_STEAM_INPUT=1', 'Controller the game sees', 0, '0');
  t.ok(w.includes('NOTPROTON_PAD_XINPUT=0') && !w.includes('STEAM_INPUT'),
       `picking PlayStation clears the Steam Input flag (${w})`);
  w = pick(RAW + ' NOTPROTON_PAD_XINPUT=0', 'Controller the game sees', 0, '');
  t.ok(w === RAW + ' %command%', `automatic removes both (${w})`);
  const edge = opts => inside(nodes(opts), 'Controller the game sees').find(x => x.type === 'Toggle');
  written.length = 0;
  edge(RAW).props.onChange(true);
  t.ok(last().includes('NOTPROTON_PAD_AS=dualsense'), `the Edge row writes dualsense (${last()})`);
  t.ok(edge(RAW + ' NOTPROTON_PAD_AS=dualsense').props.checked, 'and reads it back');

  w = pick(RAW, 'Vibration', 0, 'haptics');
  t.ok(w.includes('NOTPROTON_PAD_RUMBLE=haptics'), `vibration on the actuators (${w})`);
  w = pick(RAW + ' NOTPROTON_PAD_RUMBLE=haptics', 'Vibration', 0, '');
  t.ok(!w.includes('NOTPROTON_PAD_RUMBLE'), `back to the motors removes it (${w})`);

  // Triggers: per side an effect, and one for while the other trigger is
  // pressed, each with a strength once it is chosen; one value holds both sides.
  const slots = (opts, label) => {
    const ds = dropdowns(nodes(opts), label), held = ds.findIndex(d => /^While /.test(d.props.label));
    const at = (i, l) => (ds[i] && ds[i].props.label === l ? ds[i] : null);
    return { effect: at(0, 'Effect'), strength: at(1, 'Strength'), held: ds[held],
             heldStrength: at(held + 1, 'Strength') };
  };
  const set = (opts, label, slot, data) => {
    written.length = 0;
    slots(RAW + (opts ? ' NOTPROTON_PAD_TRIGGERS=' + opts : ''), label)[slot].props.onChange({ data });
    return triggers(last());
  };
  let z = slots(RAW, 'R2 effect');
  t.ok(z.effect && !z.strength && z.held && !z.heldStrength, 'no strengths while nothing is chosen');
  t.ok(z.held.props.label === 'While L2 is pressed' &&
       slots(RAW, 'L2 effect').held.props.label === 'While R2 is pressed', 'each side names the other trigger');
  t.ok(z.held.props.rgOptions[0].label === 'No change' &&
       z.held.props.rgOptions.slice(1).map(o => o.data).join(',') === 'resist,weapon,slope,vibrate,rumble',
       'the second effect offers no change or any effect');
  z = slots(RAW + ' NOTPROTON_PAD_TRIGGERS=R2:resist@2,R2:weapon@8+L2', 'R2 effect');
  t.ok(z.strength && z.heldStrength && z.effect.props.selectedOption === 'resist' &&
       z.strength.props.selectedOption === '2' && z.held.props.selectedOption === 'weapon' &&
       z.heldStrength.props.selectedOption === '8', 'both effects and strengths read back');
  for (const [opts, label, slot, data, want] of [
    ['', 'R2 effect', 'effect', 'weapon', 'R2:weapon'],
    ['R2:weapon', 'L2 effect', 'effect', 'slope', 'R2:weapon,L2:slope'],
    ['R2:weapon,L2:slope', 'R2 effect', 'strength', '8', 'R2:weapon@8,L2:slope'],
    ['R2:weapon@8,L2:slope', 'R2 effect', 'effect', 'rumble', 'R2:rumble@8,L2:slope'],
    ['R2:weapon@8,L2:slope', 'R2 effect', 'effect', '', 'L2:slope'],
    ['R2:weapon@8,L2:slope@3', 'L2 effect', 'strength', '', 'R2:weapon@8,L2:slope'],
    ['medium', 'L2 effect', 'effect', 'vibrate', 'R2:resist@4,L2:vibrate@4'],
    ['l2:strong', 'R2 effect', 'effect', 'resist', 'R2:resist,L2:resist@6'],
    // The second effect: alone, beside the first, changed, and removed.
    ['', 'R2 effect', 'held', 'weapon', 'R2:weapon+L2'],
    ['R2:resist@2', 'R2 effect', 'held', 'weapon', 'R2:resist@2,R2:weapon+L2'],
    ['R2:resist@2,R2:weapon+L2', 'R2 effect', 'heldStrength', '8', 'R2:resist@2,R2:weapon@8+L2'],
    ['R2:resist@2,R2:weapon@8+L2', 'L2 effect', 'held', 'resist', 'R2:resist@2,R2:weapon@8+L2,L2:resist+R2'],
    ['R2:resist@2,R2:weapon@8+L2', 'R2 effect', 'held', 'slope', 'R2:resist@2,R2:slope@8+L2'],
    ['R2:resist@2,R2:weapon@8+L2', 'R2 effect', 'held', '', 'R2:resist@2'],
    ['R2:resist@2,R2:weapon@8+L2', 'R2 effect', 'effect', '', 'R2:weapon@8+L2'],
    ['l2:rumble+r2', 'L2 effect', 'heldStrength', '3', 'L2:rumble@3+R2'],
  ]) {
    const got = set(opts, label, slot, data);
    t.ok(got === want && grammar.test(got), `${opts || '(none)'} + ${label} ${slot}=${data} -> ${got}`);
  }
  w = pick(RAW + ' NOTPROTON_PAD_TRIGGERS=R2:weapon@8', 'R2 effect', 0, '');
  t.ok(!w.includes('NOTPROTON_PAD_TRIGGERS'), `nothing chosen on either side removes the flag (${w})`);

  // Each trigger says what it will do, in words that follow the choice.
  const hint = opts => {
    const sec = section(nodes(RAW + (opts ? ' NOTPROTON_PAD_TRIGGERS=' + opts : '')), 'R2 effect');
    return walk(sec.props.children).filter(x => x.props && x.props.className === 'MSCXHint')
      .map(x => x.props.children).join('|');
  };
  t.ok(/^Left to the game\./.test(hint('')), `nothing chosen: ${hint('')}`);
  t.ok(/^Even resistance .*; while L2 is pressed, stiff .* click/.test(hint('R2:resist@2,R2:weapon+L2')),
       `both: ${hint('R2:resist@2,R2:weapon+L2')}`);
  t.ok(/^Free until L2 is pressed, then light at first/.test(hint('R2:slope+L2')), `second only: ${hint('R2:slope+L2')}`);
  t.ok(/light motor/.test(hint('R2:rumble')) && /Strength 1 is the lightest/.test(hint('R2:rumble')),
       `rumble on R2: ${hint('R2:rumble')}`);

  // What each side shows: effect@strength, and +effect@strength for the second.
  const shown = opts => ['R2 effect', 'L2 effect'].map(l => {
    const z = slots(RAW + ' ' + opts, l), v = d => (d ? d.props.selectedOption : '');
    const first = z.effect.props.selectedOption ? v(z.effect) + '@' + v(z.strength) : '';
    return first + (z.held.props.selectedOption ? '+' + v(z.held) + '@' + v(z.heldStrength) : '');
  }).join(' ');
  for (const [opts, want] of [
    ['NOTPROTON_PAD_TRIGGERS=medium', 'resist@4 resist@4'],
    ['NOTPROTON_PAD_TRIGGERS=light@7', 'resist@7 resist@7'],
    ['NOTPROTON_PAD_TRIGGERS=R2:weapon@6,L2:rumble', 'weapon@6 rumble@'],
    ['NOTPROTON_PAD_TRIGGERS=R2:max', ' '],
    ['NOTPROTON_PAD_TRIGGERS=R2:weapon@8+L2,L2:slope', '+weapon@8 slope@'],
    ['NOTPROTON_PAD_TRIGGERS=R2:resist@2,R2:weapon@8+L2,L2:rumble,L2:resist@6+R2',
     'resist@2+weapon@8 rumble@+resist@6'],
    ['NOTPROTON_PAD_TRIGGERS=R2:off,R2:weapon+L2', '+weapon@ '],
    ['NOTPROTON_PAD_TRIGGERS=R2:weapon+L2,R2:slope+L2', ' '],
    // The launcher drops a spec it does not understand whole, so the panel
    // reads one bad side, or a side waiting for itself, as nothing on both.
    ['NOTPROTON_PAD_TRIGGERS=R2:weapon@8,L2:max', ' '],
    ['NOTPROTON_PAD_TRIGGERS=R2:weapon+R2', ' '],
    ['NOTPROTON_PAD_TRIGGERS=weapon+L2', ' '],
    ['NOTPROTON_PAD_TRIGGERS=R2:weapon,R2:slope', ' '],
    ['NOTPROTON_PAD_TRIGGERS=R2:resist,', ' '],
    ['NOTPROTON_PAD_TRIGGERS=R2:off+L2,L2:slope', ' '],
    ['NOTPROTON_PAD_TRIGGERS=R2:off', ' '],
    // The broker takes effect names in lower case only, so the panel does too.
    ['NOTPROTON_PAD_TRIGGERS=R2:Weapon', ' '],
  ]) t.ok(shown(opts) === want, `${opts} reads as ${JSON.stringify(shown(opts))}`);

  failed += t.failed;
}
console.log(failed ? `\n${failed} failure(s)` : '\nboth shapes pass');
process.exit(failed ? 1 : 0);
