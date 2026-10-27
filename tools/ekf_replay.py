#!/usr/bin/env python3
"""Host-only seeded trajectory/input/replay campaign using the production ESP service.

No serial devices, ROS/MCS, network or robot access. Raw commands, truth and
estimates are separate exclusive-create artifacts. Statistical inputs below are
experimental simulation parameters, never physical calibration defaults.
"""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import zlib
from uwb_localize import solve_2d

ROOT = Path(__file__).resolve().parents[1]
T0 = 1 << 40
CONFIG = 'EKF CONFIG EXPERIMENTAL .2 .0001 1e-9 1e-8 0 16 10000 100000 400000 0 1000 3 1'
INIT = f'EKF INIT 1 2 3 {T0} 1.25 1.2 .15 .09 .09 .04'


def compile_service(destination, sanitize=False):
    core = ROOT/'ThirdParty/mul_core'
    sources = [ROOT/'tests/fixtures/ekf_cli.c', ROOT/'main/ekf_service.c']
    sources += [core/(name+'.c') for name in ('mul_ekf', 'mul_ekf_stream', 'mul_pose', 'mul_schedule', 'mul_common')]
    flags = ['-fsanitize=address,undefined', '-fno-pie', '-no-pie'] if sanitize else []
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', *flags,
                    '-I'+str(core), '-I'+str(ROOT/'main'), *map(str, sources), '-lm', '-o', str(destination)], check=True)


def pose_hex(node, xyz, time, sequence, sigma):
    b = bytearray(80)
    b[0:4] = bytes((1, node, 7, 0))
    struct.pack_into('<IIIIQ', b, 4, 1, 2, 3, sequence, time)
    struct.pack_into('<iii', b, 28, *(round(v*1000) for v in xyz))
    variance = math.ceil((sigma*1000)**2)
    struct.pack_into('<iiiiii', b, 40, variance, 0, 0, variance, 0, variance)
    struct.pack_into('<HHH', b, 64, 0, 1000, 0)
    struct.pack_into('<I', b, 76, zlib.crc32(b[:76]))
    return b.hex()


def arc(x, left, right, width=.2):
    # Independent simulation oracle: closed-form circle, straight-line limit.
    angle = (right-left)/width
    distance = (left+right)/2
    if abs(angle) < 1e-10:
        dx, dy = distance*math.cos(x[2]), distance*math.sin(x[2])
    else:
        radius = distance/angle
        dx = radius*(math.sin(x[2]+angle)-math.sin(x[2]))
        dy = radius*(math.cos(x[2])-math.cos(x[2]+angle))
    return [x[0]+dx, x[1]+dy, math.atan2(math.sin(x[2]+angle), math.cos(x[2]+angle))]


def scenario(seed=7, kind='turns', steps=120, sigma=.03, moving=True):
    rng = random.Random(seed)
    x = [1., 1., 0.]
    counters = [0xfffffff0, 0xfffffff0]
    initial = counters.copy()
    samples = []
    for step in range(steps+1):
        if step:
            if kind == 'spin':
                left, right = -.004, .004
            elif kind != 'straight' and 35 <= step < 75:
                left, right = .005, .011
            else:
                left = right = .008
            x = arc(x, left, right)
            for i, distance in enumerate((left, right)):
                # Synthetic MEASURED counter increments; never commanded telemetry.
                counts = round(distance/.0001 + rng.gauss(0, 1) + (distance/.0001*.01 if i else 0))
                counters[i] = (counters[i]+counts) & 0xffffffff
        t = T0+step*50000
        truth_anchors = []
        for i, (ax, ay) in enumerate(((0, 0), (4, 0), (0, 4), (4, 4))):
            motion = .1*math.sin(step*.03+i) if moving else 0
            truth_anchors.append([ax+motion, ay+motion*.5, 0.])
        noisy = [[v+rng.gauss(0, sigma) for v in a] for a in truth_anchors]
        sample = dict(step=step, time_us=t, truth=x.copy(), truth_alrs=truth_anchors,
                      counters=counters.copy(), noisy_alrs=noisy, batch=None)
        if step and step % 5 == 0 and not (kind == 'loss' and 20 <= step <= 55):
            distances = [math.dist([*x[:2], 0], a) for a in truth_anchors]
            radio = [rng.gauss(0, .015) for _ in distances]
            values = [distances[i]-distances[0]+radio[i]-radio[0] for i in range(1, 4)]
            outlier = kind == 'nlos' and step % 15 == 0
            if outlier:
                values[0] += 3
            twr = distances[0]+rng.gauss(0, .03)+(.2 if kind == 'bias' else 0)
            sample['batch'] = dict(tdoa=values, twr=twr, nlos=outlier)
        samples.append(sample)
    return dict(seed=seed, scenario=kind, sigma_m=sigma, sigma_before_mm_quantization=True,
                pose_rounding_bound_mm=.5, independent_pose_times=True, moving_alrs=moving,
                initial_counters=initial, samples=samples)


def batch_commands(sample, fused=True):
    step, t, b = sample['step'], sample['time_us'], sample['batch']
    count = 4 if fused else 3
    commands = [f'EKF BEGIN 1 2 3 {step} {t} {count} 0 1']
    commands += [f'EKF POSE {pose_hex(132+i, a, t, step, sample["sigma_m"])}'
                 for i, a in enumerate(sample['noisy_alrs'])]
    commands += [f'EKF OBS {i} {step*8+i+1} TDOA {133+i} 132 {value:.17g} 1 1'
                 for i, value in enumerate(b['tdoa'])]
    if fused:
        commands.append(f'EKF OBS 3 {step*8+4} TWR 132 255 {b["twr"]:.17g} 1 0')
    for i in range(count):
        values = [(.03**2 if i == 3 else 2*.015**2) if i == j
                  else (.015**2 if i < 3 and j < 3 else 0) for j in range(count)]
        commands.append(f'EKF COVROW {i} '+ ' '.join(f'{v:.17g}' for v in values))
    commands += ['EKF COMMIT', 'EKF INNOV']
    return commands


def commands(data, mode='fused', delayed=False):
    out = [CONFIG, INIT, 'EKF ON']
    pending = []
    for sample in data['samples']:
        sample = dict(sample, sigma_m=data['sigma_m'])
        step, time, counters = sample['step'], sample['time_us'], sample['counters']
        out.append(f'EKF ODOM 1 2 {time} {step} 32 SIMULATED {counters[0]} {counters[1]} 0')
        if delayed and pending and step % 5 == 2:
            out += pending
            pending = []
        if sample['batch'] and mode != 'odometry':
            batch = batch_commands(sample, mode == 'fused')
            if delayed and step < data['samples'][-1]['step']:
                pending += batch
            else:
                out += batch
        out += ['EKF STATE', 'EKF COV']
    return out+pending+['EKF STATE', 'EKF COV', 'EKF STATUS']


def replay(executable, raw_commands):
    result = subprocess.run([str(executable)], input='\n'.join(raw_commands)+'\n',
                            text=True, capture_output=True, check=True)
    responses = result.stdout.splitlines()
    if len(responses) != len(raw_commands):
        raise AssertionError('incomplete command/result trace')
    return list(zip(raw_commands, responses))


def fields(line):
    return dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)


def validate(trace):
    covariances = []
    for command, response in trace:
        if response.startswith('EKF ERR'):
            raise AssertionError((command, response))
        f = fields(response)
        if f.get('status') not in (None, 'ok', 'disabled', 'baseline', 'queued', 'gated'):
            raise AssertionError((command, response))
        if command == 'EKF COV':
            a, b, c, d, e, g = map(float, f['P'].split(','))
            assert all(math.isfinite(v) for v in (a, b, c, d, e, g))
            assert min(a, d, g) >= 0 and a*d-b*b >= -1e-10
            assert a*g-c*c >= -1e-10 and d*g-e*e >= -1e-10
            assert a*d*g+2*b*c*e-a*e*e-d*c*c-g*b*b >= -1e-10
            covariances.append((a, d, g))
    states = [fields(response) for command, response in trace if command == 'EKF STATE']
    assert all(abs(float(s['theta'])) <= math.pi+1e-8 and s['healthy'] == '1' for s in states)
    return states, covariances


def metrics(data, states):
    errors = [math.hypot(float(s['x'])-truth['truth'][0], float(s['y'])-truth['truth'][1])
              for s, truth in zip(states, data['samples'])]
    observed = [e for e, sample in zip(errors, data['samples']) if sample['batch']]
    return dict(position_rmse_m=math.sqrt(sum(e*e for e in errors)/len(errors)),
                observation_position_rmse_m=math.sqrt(sum(e*e for e in observed)/len(observed)) if observed else None,
                final_error_m=errors[-1], updates=int(states[-1]['updates']), gated=int(states[-1]['gated']))


def unfiltered(data, fused):
    errors, unavailable = [], 0
    for sample in data['samples']:
        b = sample['batch']
        if not b:
            continue
        observations = [dict(kind='range_difference', anchor=133+i, reference=132, value_m=v, valid=True)
                        for i, v in enumerate(b['tdoa'])]
        if fused:
            observations.append(dict(kind='range', anchor=132, value_m=b['twr'], valid=True))
        # Prior-independent existing unfiltered 2D solver. Project noisy3D inputs
        # onto the known simulated plane for this baseline; not EKF covariance.
        anchors = {str(132+i): a[:2] for i, a in enumerate(sample['noisy_alrs'])}
        result = solve_2d(anchors, observations, (0, 4, 0, 4))
        if result['status'] == 'available':
            errors.append(math.hypot(result['position']['x']-sample['truth'][0], result['position']['y']-sample['truth'][1]))
        else:
            unavailable += 1
    return dict(available=len(errors), unavailable=unavailable,
                position_rmse_m=math.sqrt(sum(e*e for e in errors)/len(errors)) if errors else None,
                physical_uncertainty='UNKNOWN', covariance_model='baseline solver omits ALR/radio correlation')


def campaign(executable, output):
    output.mkdir(exist_ok=False)
    report = dict(schema=1, simulation_only=True, physical_uncertainty='UNKNOWN', cases=[])
    for kind in ('straight', 'turns', 'spin', 'loss', 'bias', 'nlos'):
        data = scenario(kind=kind)
        case = dict(scenario=kind)
        with (output/f'{kind}.truth.jsonl').open('x') as stream:
            for s in data['samples']:
                stream.write(json.dumps({k: s[k] for k in ('step', 'time_us', 'truth', 'truth_alrs')})+'\n')
        for mode in ('odometry', 'tdoa', 'fused'):
            raw = commands(data, mode)
            trace = replay(executable, raw)
            states, _ = validate(trace)
            case[mode] = metrics(data, states)
            # Empirical planar consistency diagnostic, not a calibrated coverage claim.
            nees = []
            cov_lines = [fields(r) for c, r in trace if c == 'EKF COV']
            for state, covariance, sample in zip(states, cov_lines, data['samples']):
                a, b, _, d, _, _ = map(float, covariance['P'].split(','))
                dx = float(state['x'])-sample['truth'][0]
                dy = float(state['y'])-sample['truth'][1]
                det = a*d-b*b
                if det > 1e-16:
                    nees.append((d*dx*dx-2*b*dx*dy+a*dy*dy)/det)
            case[mode]['empirical_mean_planar_nees'] = sum(nees)/len(nees) if nees else None
            with (output/f'{kind}.{mode}.commands.txt').open('x') as stream:
                stream.write('\n'.join(raw)+'\n')
            with (output/f'{kind}.{mode}.results.jsonl').open('x') as stream:
                for command, result in trace:
                    stream.write(json.dumps(dict(command=command, result=result))+'\n')
            # Same immutable observations delivered late; final posterior must match.
            late = replay(executable, commands(data, mode, delayed=True))
            late_states, _ = validate(late)
            assert all(
                math.isclose(float(states[-1][k]), float(late_states[-1][k]), abs_tol=1e-9)
                for k in ('x', 'y', 'theta'))
            assert [r for c, r in trace if c == 'EKF COV'][-1] == [r for c, r in late if c == 'EKF COV'][-1]
        case['unfiltered_tdoa'] = unfiltered(data, False)
        case['unfiltered_tdoa_twr'] = unfiltered(data, True)
        report['cases'].append(case)
    with (output/'report.json').open('x') as stream:
        json.dump(report, stream, indent=2)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--service', type=Path)
    p.add_argument('--commands', type=Path, help='Replay archived commands instead of generating a campaign')
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    with tempfile.TemporaryDirectory() as work:
        executable = args.service or Path(work)/'ekf'
        if not args.service:
            compile_service(executable, args.sanitize)
        if args.commands:
            raw = args.commands.read_text(encoding='ascii').splitlines()
            trace = replay(executable, raw)
            with args.output.open('x') as stream:
                for command, response in trace:
                    stream.write(json.dumps(dict(command=command, result=response))+'\n')
            print(json.dumps(dict(commands=len(trace), output=str(args.output), hardware_access=False)))
        else:
            report = campaign(executable, args.output)
            print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
