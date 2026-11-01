'use strict';
module.exports = ({ S, str, cmd }) => [
  ['hello', 'Say hello from the example plugin (shows how a plugin adds an MCP tool).', S({ name: str('who to greet') }), a => cmd('hello', a.name)],
];
