# PSX emulator action catalog

Select through [skill routing](../SKILL.md#select-an-action); read only
[Runtime](runtime.md) and the selected reference. Discover embedded entry names,
helper roles and compiled resources with `bin/harness runtime missions`.

Use `--mission NAME`. Execution context: `--argument target=ADDRESS`, EXE entry
or restored state. Mission names are not target-qualified selectors.
Both routes preload canonical helpers. Custom Lua must require `support` before
native calls; external `--module` inputs must be noncanonical.

Prefer offline [state queries](states.md), [GPU decoding](gpu.md) or device
inspection when execution is unnecessary. Use [Step](step.md) for bounded
pause-transition observations, [Watch](watch.md) for selected hits/accesses, and
[Call](call.md) for host-established calls. [Trace](trace.md) qualifies coverage;
[Bus](bus.md) covers guest MMIO effects. Neither is a standalone mission.

Extend missing capability through [API](api.md) or [Native](native.md).
