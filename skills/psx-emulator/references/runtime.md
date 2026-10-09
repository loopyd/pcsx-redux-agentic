# Runtime missions

Contents: [purpose](#purpose), [preflight](#procedure), [invocation](#invocation),
[evidence contract](#evidence-contract), [discovery/extensions](#discovery).

## Purpose

Run bounded Lua [actions](../SKILL.md) through `bin/harness runtime run --mission NAME`.
The harness owns staging, cleanup and receipts; the requesting project owns
semantic acceptance. Inspect existing states before rerunning or writing a parser.

## Procedure

Verify `bin/harness runtime status`; preserve dirty work and failed evidence.
Follow [Native](native.md) for build customizations and dependency authorization.

<a id="invocation"></a>

Run `bin/harness runtime missions` to query the verified native catalogue.
The binary owns 19 entry missions and 35 helper modules. Both launch routes preload helpers from compiled bytes. Builtin selection
rejects helper entries; external modules cannot shadow canonical helper names.
Select exactly one of `--mission NAME` and `--script TRUSTED_LUA`. Only the external
route accepts explicit noncanonical `--module NAME=PATH` inputs.

```sh
bin/harness runtime run \
  --bios /path/to/rom.bin \
  --mission state \
  --input state=/path/to/state.pbuf --argument path=registers.pc \
  --output out/state-query --timeout 15
```

Use fresh output under an existing parent and a 512 KiB BIOS; its hash does not
infer a model. Acquisition/doctor policies are separate. `--executable` loads a
PS-X EXE through shell handoff; `--disc` mounts [CUE/BINARY media](cdrom.md).
Checked save/load accepts only the fork-owned v6 schema and identity. Historical
wire4 and old card5 inputs refuse; no cross-schema converter is supplied.

For provenance-bound continuation, including [Replay](replay.md), supply
`--origin receipt.json` and `--input state=PATH`, without an EXE. Verification
requires a successful origin, exact state size/hash and matching emulator, SDL,
bootstrap BIOS and declared disc identities. The harness hashes the receipt and
sets `PSX_RUNTIME_ORIGIN_VALIDATED=1`. This is supplied provenance, not authenticated
proof of embedded ROM, hidden host state or original effective settings.

| Input | Contract |
| --- | --- |
| `--argument NAME=VALUE` | Literal strings; ASCII letter then letters/digits/underscores; case-insensitive unique names; 64 maximum, 4096 UTF-8 bytes/value, no NUL |
| `--module NAME=PATH` | External route only; noncanonical lower-case Lua name, staged at `lua/NAME.lua`; maximum 1 MiB/module |
| `--input NAME=PATH` | Staged at `inputs/NAME`; maximum 64 MiB/input |
| Assets | 64 modules/inputs combined, 128 MiB total; original and staged hashes checked before/after execution |

Environment: `PSX_RUNTIME_ARG_NAME`, `PSX_RUNTIME_ARGUMENT_NAMES` (validation),
`PSX_RUNTIME_INPUT_NAME` and decimal EXE entry `PSX_RUNTIME_ENTRY`. Inherited
`PSX_RUNTIME_*` and Lua paths are cleared; only staged/bundled modules are searched.
Arguments enter receipts: never include credentials. Numbers accept decimal/`0x`;
supported `savestate=0|1` defaults to 0.

Maintained actions load `support` before native calls. It disables host Lua JIT
compilation, clears existing traces and rejects re-enabled mission entry; keep it
disabled throughout the mission. This controls native callback reentry and is
separate from guest CPU mode. See [API](api.md) for the calling contract.

For native media association, the harness supplies `PSX_RUNTIME_DIRECTORY` (the
resolved output directory) and `PSX_RUNTIME_MEDIA`: ordered tab-separated
`disc:trackNN.bin`, decimal byte count, lowercase SHA-256 rows, each newline-ended.
No disc means an explicitly empty value. [mount.lua](../../../src/lua/missions/mount.lua) validates
these literal identities and derives exact staged paths; it never executes data
or matches basenames. Final passed-receipt/input checks remain authoritative.

Launch uses interpreter, software GPU, debugger and fresh portable settings;
GDB/web, PCdrv, fastboot and updates are disabled. Timeout: 60 seconds by default,
maximum 3600; default log limit: 2 MiB. `--work-deadline` is an absolute monotonic
cutoff, never renewed. Failure, timeout and overflow trigger cleanup.

## Application

<a id="evidence-contract"></a>

Review Lua's unsandboxed host access. Write captures under the fresh working
directory. [support.lua](../../../src/lua/missions/support.lua) retains callbacks, guards errors/
arguments and writes `completion.json` last before quitting:

```json
{"schema":"psx.runtime-completion/v1","captures":[{"path":"ram.bin","bytes":2097152}]}
```

Missing/malformed markers, duplicate/reserved names, path escapes, symlinks and
size mismatches reject. Limits: 128 captures, 64 MiB/file, 128 MiB total.
`psx.runtime-session/v2` retains input/tool/capture hashes, staging, arguments,
outcome and timing, including failed receipts/logs. Endpoint hashes neither
provide atomic snapshots nor prove intermediate immutability.

`psx.runtime-settings/v1` records launch flags/defaults, not effective settings.
Lua changes, host audio, physical input and scheduling remain uncontrolled.
Distinguish pre-instruction observations, retired steps, host edits and cycles;
qualify executable/overlay and `psx-rizin` RAM alias.
Transport success establishes neither hardware accuracy nor semantic acceptance.
Keep proprietary inputs/captures outside skill assets.

<a id="discovery"></a>

The fork owns `skills/psx-emulator` and `src/lua/missions`. In a host checkout,
`scripts/manage-psx-emulator-skill.sh install HOST_REPO` links its expected
`third_party/pcsx-redux-agentic` skill through `.pi/skills/psx-emulator`; existing
`.agents` and `.codex` aliases keep their current chain. The installer refuses
unowned directories and unrelated symlinks. `check` and `remove` are explicit
idempotent operations on those links, with no global installation.
Follow [API](api.md) for extensions, declaring inputs, observations, stop and exclusions.

Both v2 launch routes bind all 54 physical resource files and their separate compiled
chunk hashes, sorted bundle digest, native schema, source forest, binary and SDL.
The harness queries immutable catalogue data without guest execution and hashes
the binary before/after that query. Historical external v1 receipts retain their
own shape; they cannot become builtin evidence by changing an entry name.

Checked capture/load requires a paused, settled native boundary: Running,
in-Execute, pending load, active recorders/events and Busy ownership refuse before
mutation. Legacy GUI capture has a separate source-compatible running boundary
and shares owning serialization; it carries no checked qualification. Capture
bytes precede opening/truncating a GUI target. Postcommit failure pauses and emits
no Loaded event; no filesystem transactional rollback is claimed.
