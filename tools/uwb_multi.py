"""Strict UWB3 type8/9 decoding. No host-side localization arithmetic."""
import struct

REASONS = ('ok', 'missing', 'no_reference', 'capture_age', 'schedule_mismatch',
           'clock_unavailable', 'no_rate', 'stale_rate', 'timing', 'sequence')
STATUSES = ('no_relation', 'no_rate', 'stale_rate', 'inconsistent', 'available')
SENTINEL = -2147483648


def decode_multi(data, out):
    policy = data[148]
    experimental = data[11] == 3
    if (data[2] != 3 or data[6] != 2 or data[8] != 2 or data[7] not in (4, 5)
            or policy != (data[7] == 5) or data[149]):
        raise ValueError('multi envelope')
    for key in ('raw_mm', 'calibrated_mm', 'raw_valid', 'calibration', 'calibrated_valid'):
        out.pop(key)
    out.update(schedule_profile=3 if experimental else 2, experimental_timing=experimental, rate_policy='stationary' if policy else 'unknown', source='stm32_onboard',
               dtu_nominal_hz=63897600000,
               local_clock_domain=dict(node=out['node'], radio=out['radio'], epoch=out['epoch']))
    if data[3] == 9:
        n = data[137]
        slot = int.from_bytes(data[138:140], 'little')
        revision = int.from_bytes(data[144:148], 'little')
        if n not in (40, 80, 90) or (not slot if experimental else slot not in (10, 50, 500)) or not revision or any(data[40+n:130]):
            raise ValueError('multi raw envelope')
        out.update(multi_frame=data[40:40+n].hex().upper(), multi_length=n,
                   rx=f'{int.from_bytes(data[130:135], "little"):010X}',
                   capture_age_ms=int.from_bytes(data[135:137], 'little'), slot_ms=slot,
                   schedule_fingerprint=f'{int.from_bytes(data[140:144], "little"):08X}',
                   schedule_revision=revision)
        return out
    cycle, fingerprint, revision = struct.unpack_from('<III', data, 40)
    slot, period = struct.unpack_from('<HH', data, 57)
    count, seen, valid = data[61:64]
    ids = list(data[64:64+count])
    if (not 2 <= count <= 8 or not revision
            or (not 100 <= period <= 2000 or period % 10 or not slot or (count-1)*slot+40 > period if experimental
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
        if (packed & 0xc0 or status > 4 or rate > 2 or reason > 9 or (rate and not policy)
                or available != bool(valid & (1 << index))
                or (available and (not clock or rate != 1 or reason or not seen & 1))
                or (available and SENTINEL in (dd, dt))
                or (not available and (dd != SENTINEL or dt != SENTINEL or not reason))):
            raise ValueError('multi follower')
        values.append(dict(follower_node=ids[index], slot=index, seen=bool(seen & (1 << index)),
                           valid=available, range_diff_mm=dd if available else None,
                           tdoa_dtu=dt if available else None, status=status, status_name=STATUSES[status],
                           reason=reason, reason_name=REASONS[reason], clock_valid=bool(clock), rate_state=rate))
    out.update(cycle=cycle, reference_node=ids[0], alr_ids=ids, alr_count=count,
               seen_mask=seen, valid_mask=valid, followers=values, slot_ms=slot, cycle_ms=period,
               schedule_revision=revision, schedule_fingerprint=f'{fingerprint:08X}',
               rx=f'{int.from_bytes(data[52:57], "little"):010X}',
               dropped=int.from_bytes(data[142:146], 'little'), queue_depth=data[146], queue_high=data[147])
    return out
