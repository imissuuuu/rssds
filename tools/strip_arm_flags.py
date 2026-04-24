#!/usr/bin/env python3
"""Remove ARM/devkitPro-specific compiler flags that clang-tidy doesn't understand."""
import json
import shlex

REMOVE_FLAGS = {
    '-mword-relocations',
    '-mfloat-abi=hard',
    '-mfloat-abi=softfp',
    '-mtp=soft',
    '-fno-rtti',
    '-fno-exceptions',
    '-fomit-frame-pointer',
    '-ffunction-sections',
}
REMOVE_PREFIXES = ('-specs=', '-march=', '-mtune=')


def should_remove(flag: str) -> bool:
    return flag in REMOVE_FLAGS or any(flag.startswith(p) for p in REMOVE_PREFIXES)


with open('compile_commands.json') as f:
    db = json.load(f)

for entry in db:
    if 'arguments' in entry:
        entry['arguments'] = [a for a in entry['arguments'] if not should_remove(a)]
    elif 'command' in entry:
        args = shlex.split(entry['command'])
        args = [a for a in args if not should_remove(a)]
        entry['command'] = ' '.join(shlex.quote(a) for a in args)

with open('compile_commands.json', 'w') as f:
    json.dump(db, f, indent=2)

print(f"Processed {len(db)} entries in compile_commands.json")
