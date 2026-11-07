#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Button presses for the deko3d shader harvest (tools/dk_harvest.sh).

Prints ps2run arguments (one per line, as args.txt takes them): the presses,
--rounds and --frames. Each round starts from the same save state, so a long
loading in one round cannot move the next round's presses.

  characters: from harvest-select.state (Practice's Character Select at frame
    6130, cursor on Naruto, chakra unlimited, a save with all 62 characters
    unlocked). Round k picks character k (two rows of 31 that wrap; the cursor
    walks the short way round), both sides take their link, Battle! goes four
    times (some VS screens take the first too early), then 30 s of combos,
    jutsu, ultimate with mashing, steps, jumps and throws.
  stages: from stage-select.state (Free Battle's Stage Select at frame 5470,
    Naruto against Naruto). Round k picks stage k of the 24 (the list wraps),
    then fights as above.

  tools/dk_harvest_rounds.py characters|stages [--part i --parts n]

--part/--parts take every n-th round from the i-th, for parallel runs.
The launcher's Shader harvest (gs-pcsx2/src/harvest.cpp) is the characters
sweep, written on the console.
"""
import argparse

ROW = 31
CHARACTERS = 2 * ROW
STAGES = 24
FIGHT = 1500  # frames, 50 a second


def fight(p, t, end):
    """Combos, jutsu, ultimate with mashing, a step, a jump and a throw, until end."""
    r = 0
    while t < end:
        for _ in range(4):
            p.append(f"{t}:circle:4")
            t += 12
        t += 12
        p.append(f"{t}:triangle:4")  # jutsu
        t += 12
        p.append(f"{t}:circle:4")
        t += 60
        p.append(f"{t}:triangle:4")  # ultimate
        t += 12
        p.append(f"{t}:triangle:4")
        t += 12
        p.append(f"{t}:circle:4")
        t += 40
        for _ in range(10):
            p.append(f"{t}:circle:4")
            t += 8
        p.append(f"{t}:{'left' if r % 2 else 'right'}:16")
        t += 40
        p.append(f"{t}:cross:4")
        t += 40
        p.append(f"{t}:square:4")
        t += 60
        r += 1


def characters(rounds):
    state = 6130
    length = (ROW // 2 + 1) * 40 + 20 + 700 + 1200 + FIGHT + 200
    p = []
    for slot, k in enumerate(rounds):
        t = state + slot * length + 20
        row, i = divmod(k, ROW)
        if row:
            p.append(f"{t}:down:4")
            t += 40
        steps, move = (i, "right") if i <= ROW // 2 else (ROW - i, "left")
        for _ in range(steps):
            p.append(f"{t}:{move}:4")
            t += 40
        for _ in range(6):  # 1P, its link, link mode; COM, its link, link mode
            p.append(f"{t}:cross:4")
            t += 100
        for _ in range(4):  # Battle!
            p.append(f"{t}:cross:4")
            t += 100
        fight(p, t, state + (slot + 1) * length - 60 - 400)
    return p, length, state + len(rounds) * length


def stages(rounds):
    state = 5470
    length = (STAGES // 2 + 1) * 40 + 400 + 1300 + FIGHT + 200
    p = []
    for slot, k in enumerate(rounds):
        t = state + slot * length + 30
        steps, move = (k, "down") if k <= STAGES // 2 else (STAGES - k, "up")
        for _ in range(steps):
            p.append(f"{t}:{move}:4")
            t += 40
        for _ in range(5):  # the stage, then Battle! (later ones jump)
            p.append(f"{t}:cross:4")
            t += 100
        fight(p, t + 900, state + (slot + 1) * length - 400)
    return p, length, state + len(rounds) * length


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sweep", choices=["characters", "stages"])
    ap.add_argument("--part", type=int, default=0)
    ap.add_argument("--parts", type=int, default=1)
    a = ap.parse_args()
    total = CHARACTERS if a.sweep == "characters" else STAGES
    rounds = [k for k in range(total) if k % a.parts == a.part]
    p, length, end = (characters if a.sweep == "characters" else stages)(rounds)
    for press in p:
        print("--press")
        print(press)
    print("--rounds")
    print(length)
    print("--frames")
    print(end)


if __name__ == "__main__":
    main()
