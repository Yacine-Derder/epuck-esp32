"""Strict UWB3 type8/9 and generated-plan11/12 decoding. No host-side localization arithmetic."""
import struct

REASONS = ('ok', 'missing', 'no_reference', 'capture_age', 'schedule_mismatch',
           'clock_unavailable', 'no_rate', 'stale_rate', 'timing', 'sequence')
STATUSES = ('no_relation', 'no_rate', 'stale_rate', 'inconsistent', 'available')
SENTINEL = -2147483648


def decode_multi(data, out):
    policy = data[148]
    source_version = data[149]
    planned = data[3] in (11, 12)
    experimental = data[11] == 3 or planned
    if (planned and data[11] != 4) or (not planned and data[11] not in (0, 3)):
        raise ValueError("multi profile marker")
    if (data[2] != 3 or data[6] != 2 or data[8] != 2 or data[7] not in (4, 5)
            or policy != (data[7] == 5) or source_version>1 or (data[3] in (9,12) and source_version)):
        raise ValueError('multi envelope')
    for key in ('raw_mm', 'calibrated_mm', 'raw_valid', 'calibration', 'calibrated_valid'):
        out.pop(key)
    out.update(schedule_profile=4 if planned else 3 if experimental else 2, experimental_timing=experimental,
               fingerprint_scope='ranging_plan' if planned else 'broadcast_schedule', rate_policy='stationary' if policy else 'unknown', source='stm32_onboard',
               dtu_nominal_hz=63897600000,
               local_clock_domain=dict(node=out['node'], radio=out['radio'], epoch=out['epoch']))
    if data[3] in (9, 12):
        n = data[137]
        slot = int.from_bytes(data[138:140], 'little')
        revision = int.from_bytes(data[144:148], 'little')
        if n not in (40, 80, 90) or (not slot if experimental else slot not in (10, 50, 500)) or not revision or any(data[40+n:130]):
            raise ValueError('multi raw envelope')
        frame = data[40:40+n]
        fingerprint = int.from_bytes(data[140:144], 'little')
        if planned and (frame[84 if n == 90 else 34] != 4
                        or int.from_bytes(frame[86:90] if n == 90 else frame[36:40], 'little') != fingerprint
                        or (n == 90 and frame[85] >= 32)):
            raise ValueError('planned raw binding')
        out.update(multi_frame=data[40:40+n].hex().upper(), multi_length=n,
                   rx=f'{int.from_bytes(data[130:135], "little"):010X}',
                   capture_age_ms=int.from_bytes(data[135:137], 'little'), slot_ms=slot,
                   schedule_fingerprint=f'{int.from_bytes(data[140:144], "little"):08X}',
                   schedule_revision=revision)
        if planned:
            marker=out['rx'];tx=bool(data[9]&16)
            out.update(direction='tx' if tx else 'rx',marker_dtu=marker,tx=marker if tx else None,rx=None if tx else marker)
        return out
    cycle, fingerprint, revision = struct.unpack_from('<III', data, 40)
    slot, period = struct.unpack_from('<HH', data, 57)
    count, seen, valid = data[61:64]
    ids = list(data[64:64+count])
    if (not 2 <= count <= 8 or not revision
            or (not 100 <= period <= 2000 or period % 10 or not slot or ((count-1)*slot+20 >= period if planned else (count-1)*slot+40 > period) if experimental
                else period != 1000 or slot not in (10, 50, 500) or (count-1)*slot+20 > 630) or seen >> count or valid >> count or valid & 1
            or valid & ~seen or len(set(ids)) != count or 255 in ids or data[4] in ids
            or any(data[64+count:72]) or any(data[72+10*(count-1):142])):
        raise ValueError('multi schedule')
    values = []
    for index in range(1, count):
        offset = 72+10*(index-1)
        dd, dt, packed, reason = struct.unpack_from('<iiBB', data, offset)
        status, clock, rate = packed & 7, (packed >> 3) & 1, (packed >> 4) & 3
        available = status == 4
        source = packed >> 6
        if ((not source_version and source) or (source_version and bool(source)!=bool(seen & (1<<index)))
                or (source_version and source==3 and (available or clock)) or status > 4 or rate > 2 or reason > 9 or (rate and not policy)
                or available != bool(valid & (1 << index))
                or (available and (not clock or rate != 1 or reason or not seen & 1))
                or (available and SENTINEL in (dd, dt))
                or (not available and (dd != SENTINEL or dt != SENTINEL or not reason))):
            raise ValueError('multi follower')
        values.append(dict(follower_node=ids[index], slot=index, seen=bool(seen & (1 << index)),
                           valid=available, range_diff_mm=dd if available else None,
                           tdoa_dtu=dt if available else None, status=status, status_name=STATUSES[status],
                           reason=reason, reason_name=REASONS[reason], clock_valid=bool(clock), rate_state=rate))
        if source_version:
            values[-1].update(propagation_source=("unknown","twr","external","none")[source],propagation_source_code=source)
    if source_version:
        out.update(propagation_metadata_version=source_version,physical_uncertainty="UNKNOWN")
    out.update(cycle=cycle, reference_node=ids[0], alr_ids=ids, alr_count=count,
               seen_mask=seen, valid_mask=valid, followers=values, slot_ms=slot, cycle_ms=period,
               schedule_revision=revision, schedule_fingerprint=f'{fingerprint:08X}',
               rx=f'{int.from_bytes(data[52:57], "little"):010X}',
               dropped=int.from_bytes(data[142:146], 'little'), queue_depth=data[146], queue_high=data[147])
    return out
