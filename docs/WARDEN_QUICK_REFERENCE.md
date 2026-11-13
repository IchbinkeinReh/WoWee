# Warden Quick Reference

Warden is WoW's client integrity checking system. Wowee runs the Warden module via Unicorn
Engine CPU emulation - no Wine required - and builds the check replies itself.

---

## How It Works

Warden modules are native x86 Windows DLLs that the server encrypts and delivers at login.

1. Server sends `SMSG_WARDEN_DATA` (0x2E6) naming the module; the client answers `MODULE_MISSING` and the server streams the encrypted module
2. Client verifies and decrypts: MD5 → RC4 → RSA-2048 signature verify → zlib decompress
3. Unpacks the module image: relocations applied, imports resolved (Windows API hooks)
4. Executes entry point via Unicorn Engine x86 emulator
5. `HASH_REQUEST` is answered from a `.cr` file, the built-in table for the TrinityCore/AzerothCore 3.3.5a module, or the module's own handler run in the emulator. `WOWEE_WARDEN_ANSWER_HASH=0` leaves it unanswered instead: answering makes the server send cheat checks, and a wrong check answer is penalised with a ban where an unanswered hash request is tolerated. The answers are graded against AzerothCore's check list by `test_warden_check_grader`
6. Client responds with check results via `CMSG_WARDEN_DATA` (0x2E7), built by `WardenCheckResponder` from the `WoW.exe` of the expansion being played

A signature mismatch does not stop the load. A realm that signs its own module can name its
key as `wardenRsaModulus` in the expansion profile.

---

## Server Compatibility

| Server type | Expected result |
|-------------|-----------------|
| Warden disabled | Works (no Warden packets) |
| AzerothCore / TrinityCore 3.3.5a | Works - hash answered from the built-in table, every stock check graded clean; verified live 2026-10-22. Needs the build 12340 `WoW.exe` |
| Warmane | Should work |
| vmangos / CMaNGOS (1.12, 2.4.3) | Should work; their check lists are not graded |
| Turtle WoW-derived | `HASH_REQUEST` is answered if the module runs in the emulator or a `.cr` file names it; otherwise it goes unanswered |

A wrong check answer gets the account banned rather than an error. To try a server
without risking an account, `WOWEE_WARDEN_ANSWER_HASH=0` leaves the hash request
unanswered, so no checks are sent - servers that tolerate that are unaffected.

---

## Module Cache

Modules are written to disk after download, but never read back - each session downloads
the module again:

```
~/.local/share/wowee/warden_cache/<MD5>.wdn     (%APPDATA%\wowee\warden_cache on Windows)
```

The same directory is where `<MD5>.cr` challenge/response files are read from.

Memory and code checks read the `WoW.exe` of the expansion being played. The asset
extractor caches each expansion's in its own `Data/expansions/<id>/misc`, so one install
holds all of them; `$WOWEE_INTEGRITY_DIR` overrides it. An executable of another build
answers every memory check wrong, and loading one says so in the log.

---

## Dependency

Unicorn Engine is required for module execution:

```bash
sudo apt install libunicorn-dev   # Ubuntu/Debian
sudo dnf install unicorn-devel    # Fedora
sudo pacman -S unicorn            # Arch
brew install unicorn              # macOS
```

The client builds without Unicorn and skips module emulation; check replies are built by
`WardenHandler` either way.

---

## Key Files

```
include/game/warden_handler.hpp + src/game/warden_handler.cpp   - Packet handler
include/game/warden_check_responder.hpp + src/game/warden_check_responder.cpp - Check replies
include/game/warden_cr_file.hpp                                 - .cr files, built-in module table
include/game/warden_module.hpp  + src/game/warden_module.cpp    - Module loader (8-step pipeline, MD5/RSA)
include/game/warden_emulator.hpp + src/game/warden_emulator.cpp - Unicorn Engine executor
include/game/warden_crypto.hpp  + src/game/warden_crypto.cpp    - RC4 + SHA1Randx key derivation
include/game/warden_memory.hpp  + src/game/warden_memory.cpp    - WoW.exe PE image + memory patching
include/game/warden_constants.hpp                               - Sub-opcodes and result codes
```

---

## Logs

```bash
grep -i warden logs/wowee.log
```

Key messages:
- `Warden: Module loaded successfully` - module downloaded and loaded
- `WardenModule: Calling module entry point` - emulation running
- `Warden: Loaded N CR entries` / `Warden: No usable .cr file` - whether hash requests can be answered from a table
- `Warden: HASH_REQUEST answered by the module's own handler` - answered by emulation instead
- `Warden: (sync) Parsed N checks` / `Warden: (async) Parsed N checks` - a check request was answered (info level; each check and the reply bytes are at debug level, `WOWEE_LOG_LEVEL=debug`)
- `WardenMemory: Loading PE image: <path>` - which executable memory checks are read from
- `WardenMemory: <path> is build N, not M` - the wrong executable; every memory check will fail
- `WardenMemory: WoW.exe not found` - memory checks will answer "unmapped"
- `packetsAfterGate=0` - server not responding after Warden exchange (debug level)

---

## Check Types

Check opcodes are per module (read from the `.cr` header, or the built-in table). Each result goes into one
`CHEAT_CHECKS_RESULT` (`[0x02][uint16 len][uint32 checksum][results]`):

| Name | Response |
|------|----------|
| MEM (read memory) | `[0x00][bytes]`, or `[0xE9]` if unmapped |
| PAGE_A / PAGE_B (code by hash) | `[0x4A]` found, `[0xE9]` not |
| MPQ (file hash) | `[0x00][sha1]`, or `[0x01]` not found |
| LUA | `[0x01]` (not found) |
| MODULE | `[0x4A]` for system DLLs, `[0xE9]` otherwise |
| DRIVER | `[0xE9]` (not found) |
| PROC (API check) | `[0x01]` |
| TIMING | `[0x01][uint32 ticks]` |

"Not found" is `0xE9`: TrinityCore and AzerothCore pass nothing else, and vmangos and
CMaNGOS take anything but `0x4A` as not found. `test_warden_check_grader` grades these
against AzerothCore's check list; see the implementation notes.

---

**Last Updated**: 2026-10-22
