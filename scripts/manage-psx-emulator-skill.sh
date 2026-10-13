#!/usr/bin/env bash
set -euo pipefail

fail() { printf '%s\n' "$*" >&2; exit 1; }
[[ $# == 2 ]] || fail 'usage: manage-psx-emulator-skill.sh install|check|remove HOST_REPO'
operation=$1
[[ $operation == install || $operation == check || $operation == remove ]] || fail 'unknown operation'
[[ -d $2 ]] || fail 'host repository does not exist'
host_repo=$(realpath -- "$2")
fork_root=$(realpath -- "$(dirname -- "${BASH_SOURCE[0]}")/..")
[[ $(realpath -- "$host_repo/third_party/pcsx-redux-agentic") == "$fork_root" ]] || fail 'fork must be the expected host submodule'
skill=$fork_root/.agents/skills/psx-emulator
[[ -f $skill/SKILL.md && ! -L $skill/SKILL.md ]] || fail 'canonical skill source is missing or not regular'
[[ $(head -n 1 -- "$skill/SKILL.md") == --- ]] || fail 'skill frontmatter is missing'
awk 'NR == 1 { next } /^---$/ { closed = 1; exit } /^name: psx-emulator$/ { found = 1 } END { exit !(found && closed) }' "$skill/SKILL.md" || fail 'canonical skill name differs'
links=(.agents/skills/psx-emulator .pi/skills/psx-emulator)
targets=(../../third_party/pcsx-redux-agentic/.agents/skills/psx-emulator ../../.agents/skills/psx-emulator)
legacy_targets=(../../.pi/skills/psx-emulator ../../third_party/pcsx-redux-agentic/.agents/skills/psx-emulator)
for index in "${!links[@]}"; do
    path=$host_repo/${links[index]}
    parent=$(dirname -- "$path")
    [[ $(realpath -m -- "$parent") == "$parent" ]] || fail 'link parent traverses an unrelated symlink'
    if [[ -L $path ]]; then
        target=$(readlink -- "$path")
        [[ $target == "${targets[index]}" || ($operation == install && $target == "${legacy_targets[index]}") ]] || fail "unrelated or stale symlink: ${links[index]}"
    elif [[ -e $path ]]; then
        fail "unowned path must be migrated by its owner: ${links[index]}"
    elif [[ $operation == check ]]; then
        fail "missing skill link: ${links[index]}"
    fi
done
for index in "${!links[@]}"; do
    path=$host_repo/${links[index]}
    if [[ $operation == install && -L $path && $(readlink -- "$path") != "${targets[index]}" ]]; then
        [[ $(readlink -- "$path") == "${legacy_targets[index]}" ]] || fail 'link changed before migration'
        rm -- "$path"
    fi
    if [[ $operation == install && ! -L $path ]]; then
        mkdir -p -- "$(dirname -- "$path")"
        temporary=$(mktemp -u -- "${path}.link.XXXXXXXX")
        [[ ! -e $temporary && ! -L $temporary ]] || fail 'temporary link collision'
        ln -s -- "${targets[index]}" "$temporary"
        if [[ -e $path || -L $path ]]; then rm -- "$temporary"; fail 'link target appeared during installation'; fi
        mv -n -T -- "$temporary" "$path"
        if [[ -L $temporary ]]; then rm -- "$temporary"; fail 'link target appeared during installation'; fi
    elif [[ $operation == remove && -L $path ]]; then
        [[ $(readlink -- "$path") == "${targets[index]}" ]] || fail 'link changed before removal'
        rm -- "$path"
    fi
done
printf '%s\n' "$operation: $host_repo"
