"""Strict scorer for a bounded combat gate, not campaign or WAN certification."""
import argparse
import copy
import json
import math
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
ROW = re.compile(r'^NET state tick=(\d+) rng=([0-9a-f]+) pos0=([0-9a-f,]+) pos1=([0-9a-f,]+) slots=([\d,-]+) input=([0-9a-f,]+) world=(\d+),([0-9a-f]+)(?: progress=([0-9a-f]+) area=([0-9a-f]+))?$', re.M)
HP = re.compile(r'NET actor tick=(\d+) serial=(\d+).*? hp=([-\d.e+]+)/([-\d.e+]+).*? team_flag=(\d+)')

def xyz(text):
    return tuple(struct.unpack('<f', bytes.fromhex(word)[::-1])[0] for word in text.split(','))

def assess(logs, frames, exits, native_axes=False, disconnected=False, transition=False, transition_count=1, progress=False):
    errors = []
    parsed = []
    for seat, text in enumerate(logs):
        name = 'host' if seat == 0 else 'peer'
        rows = [match.groups() for match in ROW.finditer(text)]
        ticks = [int(row[0]) for row in rows]
        if ticks != list(range(frames)):
            errors.append(f'{name}: missing, duplicated, or out-of-order frames ({len(rows)}/{frames})')
        if f'NET READY role={seat+1}' not in text:
            errors.append(f'{name}: expected network role absent')
        if f'NET SCRIPT_COMPLETE frames={frames}; peer completion confirmed;' not in text:
            errors.append(f'{name}: peer-confirmed completion absent')
        if 'NET clean WM_CLOSE posted to owned game window' not in text:
            errors.append(f'{name}: normal close request absent')
        if exits[seat] != 0:
            errors.append(f'{name}: game exit is {exits[seat]}, expected zero')
        if re.search(r'NET FAIL|EXCEPTION|FAIL closed|TIMEOUT:', text, re.I):
            errors.append(f'{name}: failure, exception, or timeout recorded')
        if any(row[4] != '0,1' for row in rows):
            errors.append(f'{name}: two expected logical slots not retained')
        if progress and (not rows or any(not row[8] or not row[9] for row in rows)):
            errors.append(f'{name}: serialized progress/current-area sample absent')
        if not rows or max(int(row[6]) for row in rows) < (2 if transition else 3):
            errors.append(f'{name}: no nonparty character observed')
        for player, column in ((0, 2), (1, 3)):
            try:
                positions = [xyz(row[column]) for row in rows]
                if not positions or not all(math.isfinite(v) for pos in positions for v in pos):
                    raise ValueError('invalid pose')
                if max(math.dist(positions[0], pos) for pos in positions) < 0.25:
                    errors.append(f'{name}: player {player} did not move enough')
            except (ValueError, struct.error):
                errors.append(f'{name}: invalid player {player} pose')
        health = [(int(m[2]), float(m[3]), float(m[4]), int(m[5])) for m in HP.finditer(text)]
        if not transition and not any(serial < 2 and hp < maximum and maximum > 0 for serial, hp, maximum, _ in health):
            errors.append(f'{name}: no party damage observed')
        if not transition and not any(serial >= 2 and team == 0 and hp <= 0 and maximum > 0 for serial, hp, maximum, team in health):
            errors.append(f'{name}: no hostile defeat observed')
        if native_axes and ('HUMAN AXIS_ORACLE' not in text or 'HUMAN reserved remote XInput' not in text):
            errors.append(f'{name}: native input oracle absent')
        if transition and ('TRANSITION PARTY_INSIDE_REAL_EXIT' not in text or 'TRANSITION AREA_CHANGED' not in text):
            errors.append(f'{name}: real exit containment and area change not both observed')
        if transition:
            changes = re.findall(r'^TRANSITION AREA_CHANGED tick=(\d+) old_slot=(\d+) new_slot=(\d+) count=(\d+) area=([A-Za-z0-9_]+)$', text, re.M)
            entrances = re.findall(r'^TRANSITION PARTY_INSIDE_REAL_EXIT tick=(\d+) ', text, re.M)
            if (len(changes) != transition_count or len(entrances) != transition_count
                    or [int(change[3]) for change in changes] != list(range(1, transition_count+1))
                    or any(change[1] == change[2] for change in changes)
                    or len({change[4] for change in changes}) != transition_count):
                errors.append(f'{name}: expected {transition_count} distinct ordered native area swaps')
        parsed.append(rows)
    if parsed[0] != parsed[1]:
        first = next((i for i, pair in enumerate(zip(*parsed)) if pair[0] != pair[1]), None)
        errors.append(f'paired sampled state differs (first common-row difference {first})')
    if disconnected:
        actual = {int(t) for t in re.findall(r'HUMAN DISCONNECT_NEUTRAL tick=(\d+)', logs[0])}
        if not set(range(901, 932)).issubset(actual):
            errors.append('host: all controller-disconnect neutral samples were not observed')
    return {'gate': 'bounded-native-area-transition' if transition else 'bounded-native-input-combat', 'result': 'FAIL' if errors else 'PASS',
            'expected_frames': frames, 'sample_counts': [len(rows) for rows in parsed], 'errors': errors,
            'scope': 'Same-machine paired processes; sampled character/health/RNG/input parity and normal exit. Campaign, full-world state, actual remote PCs and WAN remain unverified.'}

def selftest():
    def bits(value):
        return struct.pack('>f', value).hex()
    rows = [f'NET state tick={t} rng=12345678 pos0={bits(t)},00000000,00000000 pos1=00000000,{bits(t)},00000000 slots=0,1 input=11111111,22222222 world=3,12345678' for t in range(4)]
    shared = '\n'.join(rows) + '\nNET SCRIPT_COMPLETE frames=4; peer completion confirmed; no co-op QA awarded\nNET clean WM_CLOSE posted to owned game window\nNET actor tick=2 serial=0 kind=0 hp=199/200 slot=0 team_flag=1\nNET actor tick=3 serial=2 kind=0 hp=-1/45 slot=-1 team_flag=0\n'
    good = [shared + f'NET READY role={i+1}\n' for i in range(2)]
    assert assess(good, 4, [0, 0])['result'] == 'PASS'
    cases = []
    bad = copy.copy(good); bad[0] = bad[0].replace(rows[1], ''); cases.append((bad, [0, 0]))
    bad = copy.copy(good); bad[0] += rows[0] + '\n'; cases.append((bad, [0, 0]))
    bad = copy.copy(good); bad[1] = bad[1].replace('rng=12345678', 'rng=99999999', 1); cases.append((bad, [0, 0]))
    for failure in ('NET FAIL injected', 'EXCEPTION test', 'TIMEOUT: owned process'):
        bad = copy.copy(good); bad[1] += failure; cases.append((bad, [0, 0]))
    bad = copy.copy(good); bad[0] = bad[0].replace('NET SCRIPT_COMPLETE', 'NO_COMPLETION'); cases.append((bad, [0, 0]))
    cases.append((good, [0, 124])); cases.append((good, [0, 126])); cases.append((good, [0, 3221225477]))
    for bad, exits in cases:
        assert assess(bad, 4, exits)['result'] == 'FAIL'
    assert assess(good, 4, [0, 0], native_axes=True)['result'] == 'FAIL'
    assert assess(good, 4, [0, 0], disconnected=True)['result'] == 'FAIL'
    transition_lines = '\n'.join(f'TRANSITION PARTY_INSIDE_REAL_EXIT tick={i} trigger_index=1\nTRANSITION AREA_CHANGED tick={i+1} old_slot={i%2} new_slot={(i+1)%2} count={i+1} area={i+2}_Area' for i in range(3))
    transitions = [text + transition_lines for text in good]
    assert assess(transitions, 4, [0, 0], transition=True, transition_count=3)['result'] == 'PASS'
    for source, target in (('count=2', 'count=1'), ('area=3_Area', 'area=2_Area'), ('new_slot=1', 'new_slot=0')):
        bad = [text.replace(source, target) for text in transitions]
        assert assess(bad, 4, [0, 0], transition=True, transition_count=3)['result'] == 'FAIL'
    assert assess(good, 4, [0, 0], transition=True, transition_count=3)['result'] == 'FAIL'
    assert assess(good, 4, [0, 0], progress=True)['result'] == 'FAIL'
    progress_good = [re.sub(r'(world=3,12345678)', r'\1 progress=11112222 area=33334444', text) for text in good]
    assert assess(progress_good, 4, [0, 0], progress=True)['result'] == 'PASS'
    bad = progress_good.copy(); bad[1] = bad[1].replace('progress=11112222', 'progress=55556666', 1)
    assert assess(bad, 4, [0, 0], progress=True)['result'] == 'FAIL'
    print(f'SCORER_SELFTEST_PASS positive=3 rejected={len(cases)+8}')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', nargs='?'); parser.add_argument('peer', nargs='?')
    parser.add_argument('--frames', type=int, default=3600)
    parser.add_argument('--exits', type=int, nargs=2)
    parser.add_argument('--native-axes', action='store_true'); parser.add_argument('--disconnect', action='store_true')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        selftest(); return 0
    if not args.host or not args.peer or args.exits is None:
        parser.error('two run names and actual --exits are required')
    for name in (args.host, args.peer):
        if not re.fullmatch(r'run-[a-z0-9-]+', name):
            parser.error('use private run directory names')
    logs = [(ROOT / name / 'probe.log').read_text(errors='replace') for name in (args.host, args.peer)]
    result = assess(logs, args.frames, args.exits, args.native_axes, args.disconnect)
    (ROOT / 'analysis' / 'native' / f'{args.host}-gate.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    return int(result['result'] != 'PASS')

if __name__ == '__main__':
    raise SystemExit(main())
