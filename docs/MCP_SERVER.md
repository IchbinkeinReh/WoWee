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
| `--mcp-allow=LIST` | let it do things towards other players: `chat`, `emote`, `mail`, `trade`, `auction`, `group`, `guild`, `duel`, `players`, `friends`, `tickets`, or `all` of these. `settings`, which is none of `all`: keep the settings it changed on the realm (below). None by default |
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
| `state` | where the client is (login, char_select, entering_world, in_world, dead, ghost), whether a loading screen is up (`loading`: the realm has the character in the world, and the player does not see it yet), the player, its place and the transport it is on, movement flags, camera, clock, weather, auras, target, recent errors, the cursors of the logs |
| `actions` | what can be done now, each with the tool and the arguments that do it |
| `entities`, `entity` | what is about, nearest first, with filters; one in full, with where a click would pick it |
| `terrain`, `look_around`, `raycast`, `path` | the ground, the ways that are open, what a line meets, the way `walk_to` would take - out of the faces the character itself is moved against, those of game objects (lifts, ships, doors) where they are now among them, by name |
| `ui`, `ui_at` | windows, dialogs, tooltips, error texts; one window's texts and controls; what is under a point |
| `inventory`, `spellbook`, `action_bars`, `quest_log`, `character`, `map` | asked of the interface's own functions |
| `log`, `events`, `chat`, `combat_log` | since a cursor: pass the `next` of the last answer as `since`. `chat` also lists, as kind `FRAME` with its `window`, a line the interface wrote into a chat window itself (`/played`, `print`, an addon's) - once, not again for the chat and combat log events it already has |
| `sounds` | the sounds going on, and those begun in the last seconds: file, place, volume, whether heard |
| `screenshot` | the frame, a part of it or one window, as JPEG or PNG; with `boxes` the mean colour and luma of each box under `means` (`picture=false`: the means alone) |
| `wait` | for an event, a window, a text, a chat line or a state, or for a time |
| `server_info` | what the server may do and what it has refused |
| `rt_lights` | the ray traced point lights near the camera as the last frame had them: kind, model, place, final strength, reach, a fire's visible flame share, kept by the light budget or merged into its source's one light, or why it was dropped (`dropped`: too faint, reach too short, too far, out of view); `sources`: per source its lights listed and kept and `why` it gives fewer this frame (no flames or glow shown, model lights off by their tracks, no emitter on, split parts summed or joined with a light in the same place, merged by the budget, dropped); the eye adaptation's exposure |

**To act** (these press something, one at a time):

| Tool | |
|---|---|
| `move`, `jump`, `stop`, `turn`, `walk_to`, `camera` | the movement keys, the turn keys or a drag, the wheel. `walk_to` plans a way over the loaded ground and walks it by keys, jumping or stepping back where it is stuck |
| `target`, `interact` | a click on an entity in the world, where the client's own picker finds it - turned to, and looked at from elsewhere, where it is out of the window; `interact` walks up to it first (a fishing bobber it clicks where it floats). `target` with `which` presses the key bound to it, or types the slash command where none is |
| `click_ui`, `hover`, `mouse` | the mouse on a frame by name, or by pixels. A button of a dialog (StaticPopup1-4) is pressed only with `popup` naming the dialog's `which` (`state` lists it under popups), and not when another dialog shows |
| `key`, `binding`, `type_text`, `slash` | a key, the key a command is bound to, text into an edit box, a slash command through the chat box |
| `key_down`, `key_up` | a key pressed and left held (the call answers at once), and let go of again; `stop` and a failed call let go of every held key too |
| `use_action`, `use_item` | an action button by its key or a click, an item in its bag |
| `record` | the client's own recording, as `/record` makes it: start, stop, status; the answer has the file |
| `gm` | a game master's command for the own character, whispered to itself |
| `enter_world`, `quit` | from the character selection; out of the game. Logging in is left to the user (or to `WOWEE_AUTO_LOGIN`), also after `/logout` |
| `enter_realm` | at the realm list (the world server refused the connection): the realm chosen before, or the one named, entered as its Enter Realm button does, then the wait for the character list |
| `lua` | only with `--mcp-lua` |

Every acting tool answers with what came of it: the interface's events, error and info messages, system chat lines,
open dialogs, refused packets and script errors since it began. A script error comes in parts: `handler`, `source`,
`line`, `message`, `stack`; so does one of the `lua` tool's chunk, under `error`.

### How acts are done

A press is an event in SDL's queue *and* the held state the game reads once a frame (`core::Input`), both put there
before the frame's events are read - the two ways a device's press goes. Nothing is called that a key would have
called: key bindings, protected actions, edit boxes, the click picking and the camera see a press and cannot tell. While
the server uses the mouse, the real pointer is kept out; somebody who moves it over the focused window, or clicks, has
it back.

Two things are no act of a player's and are not pressed: `gm` sends its whisper by the interface's own
`SendChatMessage`, so that no dialog or edit box is in its way, and selects the character itself first, but for a
command that only tells (`on` says otherwise), with what was selected selected again after; `record` starts and stops
the recorder.

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
- No tool reads a file or runs a program, and none is given the name of one to write: a recording goes where `/record`
  puts it, under a name of the client's. Every string of an answer is held against the account's name, the streaming
  key and the token, and blanked where it has one. No pictures are taken of the login and set-up screens.
- Nothing is pressed on the screens for logging in, realms, new characters and set-up (`enter_realm` calls the realm
  list's own Enter Realm; no key or click reaches it and nothing is typed).
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
.unaura .learn .unlearn ID|all`, `.additem ID` (`.additem OWNNAME ID COUNT` for a count), `.morph ID`, `.go xyz|creature|gameobject|...`, `.tele PLACE`, `.gm
on|off|fly|visible`, `.modify ...`, `.quest add|complete|remove|reward ID`, `.npc info|near`, `.gobject
info|near|target`, `.cast ID|self|back`, `.lookup` (not `player`), `.server info|motd`, `.instance listbinds|unbind`,
`.guild create "NAME"` (or with the own name before the quoted guild name; the character leads it), `.guild delete
"NAME"` only for the guild the character is in, `.arena create "NAME" 2|3|5` (or with the own name before the quoted
team name; the character captains it), `.help`, `.commands`. None of them with another player, or their pet, selected; and with anything else selected but the
character itself - a creature, a mount, its own pet - only those that tell and do nothing: `.npc info|near`, `.gobject
info|near|target`, `.gps`, `.lookup`, `.distance`, `.help`, `.commands`, `.server`. Selected is what the client has
selected and what the last `CMSG_SET_SELECTION` told the realm, both. The packet is where it is decided, so `.aura` by
`slash`, a macro or an addon is refused like `gm`'s; `gm` selects the character itself before (above) and is judged by
that.

Never sent, whatever is allowed: deleting, creating or renaming characters, destroying items, abandoning a pet (a
charmed creature - an enslaved demon - is let go of: it is the realm's again),
unlearning a profession, the game masters' and developers' own packets. Quitting from inside the game is refused; the
`quit` tool does it.

### Settings

What its doing changes of the CVars, key bindings, chat windows and the action bars shown is not refused, so that it can
be tried. It is told: `state` has `settings_changed`, a list of `cvar`, `old`, `new` and `when` since the server started
(a key binding as `key KEY`, the action bars as `SetActionBarToggles`), and `settings_not_sent_up`, the kinds of account
data that are held back for it. What the client and the interface set themselves behind the loading screen of a world is
neither.

Without `--mcp-allow=settings` none of it is kept on the realm: `CMSG_UPDATE_ACCOUNT_DATA` of the CVars (0, 1), the key
bindings (2, 3) and the chat windows (7) is not sent, nor `CMSG_SET_ACTIONBAR_TOGGLES` (macros, 4 and 5, are). A kind
once changed by the server's doing is not sent for as long as the client goes, a person's changes after it with it. While
the client goes it stays as it was changed, over a logout too; at the next start the realm's copy is taken again (its time
here is put to 0), and the account's CVars are put back as they were when the client ends.

### Gaps

- Whose doing something is, is who acted last. If a person plays along and presses a key, what an earlier act of the
  server's leads to afterwards is counted as theirs; and what they do without a key or button of their own, as the
  server's.
- Spells without a target can land on others (areas, auras), and objects somebody else put down can be used - a
  spell aimed at one of them, or at a player's corpse, is held to be aimed at that player.
- A group or a duel somebody else began is not left by the server: it can decline and cancel, and not accept.
- What of the settings lies on this computer alone stays changed: `settings.cfg`, and after a crash the account's CVars
  the realm has no value of. A key binding or chat window changed within a second of a real key press counts as a
  person's. What a person changes of a kind the server's doing changed before is not kept either.
- Where the realm has nothing of a kind yet, what was changed stays here until it is changed again.
- Names are redacted from texts, not from pictures. A picture of the world shows the names over heads.
- The token protects against other programs of *other* users and against web pages; a program of the same user can
  read the file.
- The rules are written for 3.3.5a. On the older expansions casts are judged by the selected target.
- Whoever may call it can spend the character's money at vendors, sell, drop quests and die.

A packet log at info level, opcode and size of every packet both ways (no contents): start the client with
`WOWEE_PACKET_LOG=1`.
