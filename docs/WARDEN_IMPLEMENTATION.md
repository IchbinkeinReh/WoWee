# Warden Implementation

**Status**: The module is loaded, decrypted, verified, unpacked, relocated, its imports bound and its entry point run under Unicorn. Its own packet handler answers `HASH_REQUEST` when no table does; every cheat check is answered by `WardenCheckResponder` (see Check Responses), and those answers are graded against AzerothCore's own check list by `test_warden_check_grader`. Verified on a live AzerothCore 3.3.5a realm on 2026-10-22.
**WoW Version**: 3.3.5a (build 12340); the handler also has paths for 1.12.1 and Turtle WoW-derived realms

---

## Overview

Warden is WoW's client integrity checking system. The server sends encrypted modules
containing native x86 code; the client is expected to load and execute them, then
return check results.

Wowee runs the x86 module through Unicorn Engine CPU emulation - in an emulated
environment with Windows API hooks, without Wine or a Windows OS. The check replies
are built by the client rather than by the module: memory reads are served from a
`WoW.exe` image on disk. Hash requests are answered from pre-computed `.cr` files
where one matches, and otherwise by the module itself.

---

## Loading Pipeline (8 steps)

```
1. MD5       - Verify module checksum matches server challenge
2. RC4       - Decrypt module payload
3. RSA-2048  - Verify module signature (Blizzard's key, or the realm's - see Crypto Layer below)
4. zlib      - Decompress module
5. Parse     - Unpack Warden's own image format (copy/skip pair stream, or the native header Turtle-derived realms use)
6. Relocate  - Apply base relocations to load address
7. Bind      - Resolve imports (Windows API stubs + Warden callbacks)
8. Init      - Call module entry point via Unicorn Engine
```

No step's failure stops the load: `WardenModule::load` logs it and continues with
what it has. A module that did not unpack into an image is not emulated at all, and
the handler answers from its own code.

---

## Unicorn Engine Execution

The module entry point is called inside an Unicorn x86 emulator with:

- Executable memory mapped at the module's load address
- A simulated stack
- Windows API interception for calls the module makes

Hooked APIs are `VirtualAlloc`, `VirtualFree`, `GetTickCount`, `Sleep`,
`GetCurrentThreadId`, `GetCurrentProcessId` and `ReadProcessMemory`; any other import
is auto-stubbed to return 0 (logged at debug level by name). Each stub pops the
stdcall argument count of the API it stands in for - every import the 3.3.5a module
names is listed in `WardenEmulator::hookAPI` - since a stub that pops the wrong count
returns into a shifted stack. DLL names are matched without regard to case. Each call
is bounded at ten seconds and puts `ESP` back afterwards.

The module's ABI, as the 3.3.5a module uses it:

- **Entry point** - thiscall, with `ECX` pointing at the client-services object. It
  returns the module's own object.
- **Objects** - the first dword of each is its function table.
- **Client services** (the module calls these, thiscall, callee pops):
  `[0] SendPacket(packet, size)`, `[1] CheckModule(name, out)`,
  `[2] LoadModule(key, data, size)`, `[3] AllocateMemory(size)`, `[4] FreeMemory(ptr)`,
  `[5] SetRC4Data(state, size)`, `[6] GetRC4Data(buffer, size*)`.
- **Module exports** (called with `ECX` = the module object):
  `[0] GenerateRC4Keys(data, size)`, `[1] Unload(keys)`,
  `[2] PacketHandler(packet, size, bytesConsumed*)`, `[3] Tick(ms)`.
- Once keyed by `GenerateRC4Keys` with the session key, the module ciphers its own
  traffic: it is handed packets as they arrive and sends them encrypted. It hands its
  cipher state over through `SetRC4Data` only when it is unloaded - two RC4 states,
  client->server first, each `[256 S-box][i][j]`.

A module whose signature did not verify is run anyway, so a private server's own
module gets its chance. When it faults, that is logged as a warning rather than an
error (`WardenEmulator::setFailuresExpected`), since it is the expected outcome.

---

## Module Cache

After download, the raw (encrypted) module is written to disk:

```
~/.local/share/wowee/warden_cache/<MD5>.wdn     (%APPDATA%\wowee\warden_cache on Windows)
```

Nothing reads it back: the client answers every `MODULE_USE` with `MODULE_MISSING`,
so the module is downloaded and loaded again each session.

The same directory holds `<MD5>.cr` files - pre-computed challenge/response entries
for a module (a 17-byte header carrying the module's nine check opcodes, then 68-byte
entries of seed, reply and the next RC4 key pair; `include/game/warden_cr_file.hpp`).
`WardenHandler::loadWardenCRFile` reads one on `MODULE_USE`, and a `HASH_REQUEST` whose seed matches an entry is
answered from it. Two more sources are used unless `WOWEE_WARDEN_ANSWER_HASH=0` - the
switch exists because answering is what makes a server send cheat checks, and a wrong
check answer gets the account banned. With no file, the module every TrinityCore/AzerothCore realm sends
to 3.3.5a (`79C0768D657977D697E10BAD956CCED1`) is answered from a built-in table
(`knownWardenCR`, the constants AzerothCore publishes).

A `HASH_REQUEST` no table answers is handed to the module. `WardenModule::answerHashRequest`
keys it from the session key (`GenerateRC4Keys`), gives its `PacketHandler` the request
encrypted as the server sends it, reads its encrypted `HASH_RESULT` back, then unloads
it - which is when a module hands the client its new cipher state through `SetRC4Data`.
The client installs that state as is (`WardenCrypto::replaceState`). The module's exports
and client services are thiscall: the object pointer travels in `ECX`, and the first
dword of each object is its function table.

---

## Crypto Layer

| Algorithm | Purpose |
|-----------|---------|
| RC4 | Encrypt/decrypt Warden traffic (separate in/out ciphers) |
| MD5 | Module identity hash |
| SHA1 | HMAC, check hashes and SHA1Randx key derivation |
| RSA-2048 | Module signature verification |

The signature is checked against Blizzard's retail modulus (`kRetailModulus` in
`src/game/warden_module.cpp`, the same across 1.12.1, 2.4.3 and 3.3.5a). A server that
signs its own module can name its key as `wardenRsaModulus` in the expansion profile -
512 hex characters for the 256-byte modulus, little-endian as it sits in the client
binary - and the module is checked against that instead. A mismatch is logged as a
warning and loading continues.

The module ends in `[4 "NGIS"][256 signature]`. The signature covers what comes before
the tag; it and the modulus are little-endian, and the recovered block is the SHA1 of
the signed data plus `"MAIEV.MOD"` in its low 20 bytes, then `0xBB` padding.

---

## Opcodes

- `SMSG_WARDEN_DATA` = 0x2E6 - server sends module + checks
- `CMSG_WARDEN_DATA` = 0x2E7 - client sends results

Sub-opcodes inside the decrypted payload (`include/game/warden_constants.hpp`):

| Direction | Value | Name |
|-----------|-------|------|
| S->C | 0x00 | `MODULE_USE` (module hash, key, size) |
| S->C | 0x01 | `MODULE_CACHE` (module data chunk) |
| S->C | 0x02 | `CHEAT_CHECKS_REQUEST` |
| S->C | 0x03 | `MODULE_INITIALIZE` (no reply) |
| S->C | 0x05 | `HASH_REQUEST` (16-byte seed) |
| C->S | 0x00 | `MODULE_MISSING` |
| C->S | 0x01 | `MODULE_OK` |
| C->S | 0x02 | `CHEAT_CHECKS_RESULT` |
| C->S | 0x04 | `HASH_RESULT` |

---

## Check Responses

Check type opcodes differ per module; they are read from the `.cr` file header (or the
built-in table) and XORed with the request's last byte. `buildWardenCheckResponse`
(`src/game/warden_check_responder.cpp`) answers each one - on the spot, or on a
background thread when the request holds a code scan:

| Check type | Reply |
|------------|-------|
| `READ_MEMORY` (MEM) | `0x00` + bytes read from the `WoW.exe` image, or `0xE9` if unmapped |
| `FIND_MEM_IMAGE_CODE_BY_HASH` / `FIND_CODE_BY_HASH` (PAGE_A/B) | `0x4A` if an HMAC-SHA1 pattern search of the image finds it, else `0xE9` |
| `HASH_CLIENT_FILE` (MPQ) | `0x00` + SHA1 of the file via the asset manager, else `0x01` |
| `GET_LUA_VARIABLE` (LUA) | `0x01` (not found) |
| `FIND_MODULE_BY_NAME` (MODULE) | `0x4A` for standard system DLLs, `0xE9` otherwise |
| `FIND_DRIVER_BY_NAME` (DRIVER) | `0xE9` (not found) |
| `API_CHECK` (PROC) | `0x01` (not found) |
| `CHECK_TIMING_VALUES` (TIMING) | `0x01` + millisecond tick count |

"Not found" is `0xE9` because that is the only value TrinityCore and AzerothCore pass;
vmangos and CMaNGOS take anything but `0x4A` as not found. The reply is
`[0x02][uint16 length][uint32 checksum][results]`, the checksum being the
XOR of the five words of SHA1(results).

`WardenMemory` loads the `WoW.exe` image the memory and code checks read from, and fakes
`KUSER_SHARED_DATA`. Without it, memory checks answer `0xE9`. Each expansion's
executable has its own folder - the asset extractor caches it, with its DLLs, in
`Data/expansions/<id>/misc` - so one install holds all of them, and the one used is
picked by the server being played:

1. `$WOWEE_INTEGRITY_DIR`, then the active expansion's `Data/expansions/<id>/misc`
   (and Turtle's `overlay/misc`) are looked in first, then the home-directory
   locations, the other expansions' folders and `Data/misc`.
2. Among those, one whose image size is the one known for the build wins - which is
   what tells a vanilla 1.12 executable from Turtle's, both build 5875.
3. Then one whose version resource names the realm's build (`WardenMemory::fileBuildOf`).
4. Then the first in the active expansion's folder, whatever it is.

An executable of another build answers every memory check with the wrong bytes, so
loading one logs which file it is, which build it is, and where the right one goes.
Some values differ between the file and a running client and are patched into the
image: for 3.3.5a, the login state at `0xAC3DAC` is `4`.

A `HASH_REQUEST` that neither a table nor the module answers gets no reply on WotLK/TBC and Turtle
(AzerothCore tolerates the silence and rejects a wrong hash; Turtle-derived realms
close the connection) - `WOWEE_WARDEN_TURTLE_NO_CR=fallback` overrides that for Turtle.
1.12.1 realms are sent a SHA1 of the module image as a fallback.

---

## Testing

`test_warden_check_grader` puts AzerothCore's check list (`tests/warden_azerothcore_checks.inc`:
every memory, module, driver and Lua check and a sample of the code scans) to
`buildWardenCheckResponse`, laid out as AzerothCore's `WardenWin::RequestChecks` sends
them, and grades the reply by its `WardenWin::HandleData`. A wrong answer on a live
server is a ban, so it has to fail here first. The memory checks need the genuine
build 12340 executable:

```bash
WOWEE_TEST_WOW_EXE=~/Library/Application\ Support/wowee/Data/expansions/wotlk/misc/WoW.exe \
    build/bin/test_warden_check_grader
```

Without it they are skipped; the rest run everywhere. vmangos and CMaNGOS realms are not
graded here.

The module itself can be exercised offline too: the module TrinityCore and AzerothCore
send is cached as `79c0768d657977d697e10bad956cced1.wdn` after a first login, and
AzerothCore's `WardenModuleWin.h` publishes its RC4 key, seed, reply and keys - a known
answer for `WardenModule::answerHashRequest`.

---

## Key Files

```
include/game/warden_handler.hpp      - Packet handler interface
src/game/warden_handler.cpp          - handleWardenData, .cr loading
include/game/warden_check_responder.hpp + src/game/warden_check_responder.cpp - check replies
include/game/warden_cr_file.hpp      - .cr reader, built-in TrinityCore/AzerothCore module table
include/game/warden_constants.hpp    - Sub-opcodes, memory ranges, result codes
include/game/warden_module.hpp       - Module loader interface
src/game/warden_module.cpp           - 8-step pipeline
include/game/warden_emulator.hpp     - Emulator interface
src/game/warden_emulator.cpp         - Unicorn Engine executor + API hooks
include/game/warden_crypto.hpp       - Crypto interface
src/game/warden_crypto.cpp           - RC4 / key derivation
include/game/warden_memory.hpp       - PE image + memory patch interface
src/game/warden_memory.cpp           - WoW.exe PE loader, runtime globals patching, code-pattern search
```

---

## Performance

- `PAGE_A`/`PAGE_B` checks brute-force a pattern search over the image and can take
  seconds. A request containing one is built on a background thread (`std::async`) and
  sent from `WardenHandler::update`, so the main loop does not stall.
- Pattern-search results are cached per seed, hash and length (`WardenMemory::codePatternCache_`).

---

## Dependencies

Requires `libunicorn-dev` (Unicorn Engine); CMake defines `HAVE_UNICORN` when it is
found. The client compiles without it and skips emulation. Check replies are built by
`WardenHandler` either way, so they do not depend on it.

---

## References

- [WoWDev Wiki - Warden](https://wowdev.wiki/Warden)
- [WoWDev Wiki - SMSG_WARDEN_DATA](https://wowdev.wiki/SMSG_WARDEN_DATA)
- [TrinityCore Warden](https://github.com/TrinityCore/TrinityCore/tree/3.3.5/src/server/game/Warden)

---

**Last Updated**: 2026-10-22
