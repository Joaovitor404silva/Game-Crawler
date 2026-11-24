'use strict';
module.exports = ({ S, str, cmd }) => [
  ['gta6_xcl', 'Inspect the XCL hosted GTA VI window, send one keyboard-as-gamepad button, or click a normalized client position (0–1000).', S({ action: { type: 'string', enum: ['status', 'tap', 'click'] }, key: str('enter, space, esc, up, down, left, right, a, tab, p, or backspace'), hold_ms: { type: 'integer', description: 'Key hold time in milliseconds (40–1000)' }, x: { type: 'integer', description: 'Normalized client x coordinate, 0–1000, for click' }, y: { type: 'integer', description: 'Normalized client y coordinate, 0–1000, for click' } }, ['action']), a => {
    if (a.action === 'status') return cmd('gta6 status');
    if (a.action === 'click') {
      if (!Number.isInteger(a.x) || !Number.isInteger(a.y) || a.x < 0 || a.x > 1000 || a.y < 0 || a.y > 1000) throw new Error('click requires integer x and y coordinates from 0 to 1000');
      return cmd('gta6 click ' + a.x + ' ' + a.y);
    }
    if (!a.key) throw new Error('key is required for tap');
    return cmd('gta6 tap ' + a.key + (a.hold_ms ? ' ' + a.hold_ms : ''));
  }],
];
