'use strict';
module.exports = ({ S, str, cmd }) => [
  ['gta6_xcl', 'Inspect the XCL hosted GTA VI window or send one keyboard-as-gamepad button.', S({ action: { type: 'string', enum: ['status', 'tap'] }, key: str('enter, space, esc, up, down, left, right, tab, p, or backspace'), hold_ms: { type: 'integer', description: 'Key hold time in milliseconds (40–1000)' } }, ['action']), a => {
    if (a.action === 'status') return cmd('gta6 status');
    if (!a.key) throw new Error('key is required for tap');
    return cmd('gta6 tap ' + a.key + (a.hold_ms ? ' ' + a.hold_ms : ''));
  }],
];
