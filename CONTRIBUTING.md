# Contributing to PCSX-Redux Agentic

This fork targets Linux agentic coding, reverse engineering, and behavioral
analysis. Preserve upstream authorship, license notices, vendor pins, and PSX guest
formats. Follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

Use the local Make build and the existing checks relevant to your change. Native
API changes also need a focused emulator run that demonstrates changed behavior
and a relevant refusal. Record what the run establishes and what remains unverified.
Hosted build, test, release, and cloud automation are not maintained in this fork.

Keep bulk work in bounded C++ APIs, with thin Lua wrappers for composition. Preserve
owner-thread, stopped-state, lease, and restore-validation rules. Performance claims
need measurements of the complete caller operation and equivalent observations.
Do not replace full checked-state admission with a CPU-only shortcut.

Use the existing C++ formatting rules in `src/.clang-format`. Bundle related
functionality in cohesive modules of at most 4000 lines. Avoid backup modules,
artificial slices, and temporary verification frameworks. Add regression coverage
only when requested; adapt and run existing checks when an API moves.

Update the owning skill procedure and user documentation when capabilities change.
Keep experiments and proprietary captures out of maintained procedures. Bug reports
should include the fork revision, Linux environment, inputs that may be shared,
command, actual behavior, expected behavior, and relevant bounded logs.

By submitting a contribution, you represent that you can license it under the
project's existing license.
