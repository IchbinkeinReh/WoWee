# MCP server

The client can let another program on the same machine see the game and play it, over the
[Model Context Protocol](https://modelcontextprotocol.io). It is made for playtests: what it reports is what the client
itself draws and works with, and what it does is done with keys and the mouse, so that a wrong state or a broken path
shows.

It is **off** unless the command line asks for it, on every start. Without the option there is no thread, no socket and
no work per frame; nothing of it is kept in the settings. Desktop only (Windows, Linux, macOS); the Android and iOS
builds do not have it.

## Start

```
wowee --mcp                 port 8642 on 127.0.0.1
wowee --mcp 9000            or --mcp=9000; 0 takes a free port
wowee --mcp --mcp-lua       the `lua` tool and /run as well
wowee --mcp --mcp-allow=duel,trade
wowee --help
```

| Option | |
|---|---|
| `--mcp [PORT]`, `--mcp=PORT` | start the server |
| `--mcp-allow=LIST` | let it do things towards other players: `chat`, `emote`, `mail`, `trade`, `auction`, `group`, `guild`, `duel`, `players`, `friends`, `tickets`, or `all`. None by default |
| `--mcp-lua` | let it run Lua in the interface. Off by default |

An option that cannot be understood keeps the server off; the log says so. Arguments that are none of these are
ignored, as they always were.

At each start a new token is made and written, with the port, to `mcp_server.json` in the config root (`%APPDATA%\wowee`,
`~/.wowee`, `config` beside a portable client, or `WOWEE_CONFIG_ROOT`). The file is readable by this user only and is
removed when the client ends.

## Connect

```
claude mcp add --transport http wowee http://127.0.0.1:8642/mcp --header "Authorization: Bearer TOKEN"
```

or, without an MCP client, with the script (Python's standard library only; it reads the file itself and never prints
the token):

```
python tools/mcp/wowee_mcp.py list
python tools/mcp/wowee_mcp.py call state
python tools/mcp/wowee_mcp.py call entities '{"filter": "npc", "radius": 40}'
python tools/mcp/wowee_mcp.py call walk_to name="Innkeeper Pala"
python tools/mcp/wowee_mcp.py --out shots/a.jpg call screenshot max_width=1280
python tools/mcp/wowee_mcp.py loop < calls.txt        # a line each: TOOL {json}
python tools/mcp/wowee_mcp.py config                  # the `claude mcp add` line
```

The transport is Streamable HTTP with plain JSON answers: `POST /mcp` with one JSON-RPC 2.0 message (or a batch);
`initialize`, `ping`, `tools/list`, `tools/call`. There is no event stream (`GET` is 405) and no session.

## Tools

Places are `.gps` coordinates in yards, which `.go xyz` takes back. Bearings are degrees clockwise from where the
character faces (0 ahead, 90 right). Pixels are the window's, from the top left. Guids are `0x...`.

**To look** (these only read, and run side by side):

| Tool | |
|---|---|
| `state` | where the client is (login, char_select, entering_world, in_world, dead, ghost), the player, its place, movement flags, camera, clock, weather, auras, target, recent errors, the cursors of the logs |
| `actions` | what can be done now, each with the tool and the arguments that do it |
| `entities`, `entity` | what is about, nearest first, with filters; one in full, with where a click would pick it |
| `terrain`, `look_around`, `raycast`, `path` | the ground, the ways that are open, what a line meets, the way `walk_to` would take - out of the faces the character itself is moved against |
| `ui`, `ui_at` | windows, dialogs, tooltips, error texts; one window's texts and controls; what is under a point |
| `inventory`, `spellbook`, `action_bars`, `quest_log`, `character`, `map` | asked of the interface's own functions |
| `log`, `events`, `chat`, `combat_log` | since a cursor: pass the `next` of the last answer as `since` |
| `screenshot` | the frame, a part of it or one window, as JPEG or PNG |
| `wait` | for an event, a window, a text, a chat line or a state, or for a time |
| `server_info` | what the server may do and what it has refused |

**To act** (these press something, one at a time):

| Tool | |
|---|---|
| `move`, `jump`, `stop`, `turn`, `walk_to`, `camera` | the movement keys, the turn keys or a drag, the wheel. `walk_to` plans a way over the loaded ground and walks it by keys, jumping or stepping back where it is stuck |
| `target`, `interact` | a click on an entity in the world, where the client's own picker finds it; `interact` walks up to it first |
| `click_ui`, `hover`, `mouse` | the mouse on a frame by name, or by pixels |
| `key`, `binding`, `type_text`, `slash` | a key, the key a command is bound to, text into an edit box, a slash command through the chat box |
| `use_action`, `use_item` | an action button by its key or a click, an item in its bag |
| `gm` | a game master's command for the own character, whispered to itself |
| `enter_world`, `quit` | from the character selection; out of the game. Logging in is left to the user (or to `WOWEE_AUTO_LOGIN`), also after `/logout` |
| `lua` | only with `--mcp-lua` |

Every acting tool answers with what came of it: the interface's events, error and info messages, system chat lines,
open dialogs and refused packets since it began.

### How acts are done

A press is an event in SDL's queue *and* the held state the game reads once a frame (`core::Input`), both put there
before the frame's events are read - the two ways a device's press goes. Nothing is called that a key would have
called: key bindings, protected actions, edit boxes, the click picking and the camera see a press and cannot tell. While
the server uses the mouse, the real pointer is kept out; somebody who moves it over the focused window, or clicks, has
it back.

Calls are put in a queue by the connection's thread and run by the game's loop, for at most four milliseconds a frame
(always one step of one call). During a loading screen the loop does not run; a call waits, and says so when its time
is up. Pictures are read back from the GPU on the game's thread and cut, shrunk and encoded on the caller's.

The window need not have the keyboard: without it the client draws at most `maxFPSBk` (30) frames a second, and a turn
by keys is six degrees a frame, which is why `turn` finishes with the mouse. Minimised, everything but the interface
works: it is not laid out then, so there is nothing to click in it, and no picture to take.

`walk_to` knows the ground the client has loaded, up to 45 yards ahead at a time, and neither water's depth nor
creatures. In water it swims straight at its goal.

## Security

- The socket is bound to 127.0.0.1 and nothing else (`SO_EXCLUSIVEADDRUSE` on Windows).
- Each request needs the bearer token: 32 bytes of the operating system's randomness, new at every start, compared in
  constant time, never logged.
- `Host` has to be `127.0.0.1:PORT` or `localhost:PORT` (DNS rebinding), and `Origin`, where there is one, local (pages
  in a browser). Both are asked before the token. No CORS headers are sent.
- The HTTP reader takes a request line, headers and a body of given length; chunked bodies, folded headers, bare LFs,
  double `Host` / `Content-Length` / `Authorization` and the like are refused. 16 KB of head, 64 headers, 256 KB of
  body, 8 connections, 10 s for a request, 60 s idle, JSON nested at most 32 deep.
- A call's arguments are held to the schema `tools/list` gives; what it does not name is refused.
- No tool reads a file or runs a program. Every string of an answer is held against the account's name, the streaming
  key and the token, and blanked where it has one. No pictures are taken of the login and set-up screens.
- Nothing is pressed on the screens for logging in, realms, new characters and set-up.
- Lua runs only with `--mcp-lua`; without it `/run`, `/script`, `/dump` and macros that run Lua do nothing while the
  server acts.

### Other players

While the server runs, each packet the client is about to send is asked about (`WorldSocket::send` ->
`mcp/send_guard.hpp`) if the last thing that acted on the client was the server: from its first press until a real key,
mouse button or typed text - also what is sent later, out of a timer, an addon or an answer. Refused unless
`--mcp-allow` names the class: chat of every kind, emotes, mail, trades, auctions, groups and everything that puts the
character with others (dungeon finder, battlegrounds, rolls), guild business, charters, arena teams, the calendar, duels,
spells, items and attacks aimed at another player or their pet, the friends list, tickets and reports. By the whole
family of an opcode's name, with what only asks, declines or cancels let through.

A whisper to the own character goes out. One that is a game master's command has to be on a list: `.gps .revive .die
.dismount .demorph .combatstop .maxskill .cooldown .levelup .respawn .save .distance` bare or with the own name; `.aura
.unaura .learn .unlearn ID|all`, `.additem ID`, `.morph ID`, `.go xyz|creature|gameobject|...`, `.tele PLACE`, `.gm
on|off|fly|visible`, `.modify ...`, `.quest add|complete|remove|reward ID`, `.npc info|near`, `.gobject
info|near|target`, `.cast ID|self|back`, `.lookup` (not `player`), `.server info|motd`, `.instance listbinds|unbind`,
`.help`, `.commands`. None of them with another player, or their pet, selected.

Never sent, whatever is allowed: deleting, creating or renaming characters, destroying items, abandoning a pet,
unlearning a profession, the game masters' and developers' own packets. Quitting from inside the game is refused; the
`quit` tool does it.

### Gaps

- Whose doing something is, is who acted last. If a person plays along and presses a key, what an earlier act of the
  server's leads to afterwards is counted as theirs; and what they do without a key or button of their own, as the
  server's.
- Spells without a target can land on others (areas, auras), and objects somebody else put down can be used.
- A group or a duel somebody else began is not left by the server: it can decline and cancel, and not accept.
- Names are redacted from texts, not from pictures. A picture of the world shows the names over heads.
- The token protects against other programs of *other* users and against web pages; a program of the same user can
  read the file.
- The rules are written for 3.3.5a. On the older expansions casts are judged by the selected target.
- Whoever may call it can spend the character's money at vendors, sell, drop quests and die.
