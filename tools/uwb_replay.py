"""Schema-1 replay snapshots. Lossless integers, bounded reassembly, no TDoA math."""
import binascii
from collections import OrderedDict

SIZE = 384
PARTS = 4


def digest(blob):
    value = 2166136261
    for byte in blob:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def fragment(data, out):
    if data[11] not in (1, 2):
        raise ValueError(f'unsupported tdoa_replay schema {data[11]}')
    if (data[2] != 3 or data[6] != 2 or data[7] not in (4, 5) or data[8] != 2 or
            data[9] or data[10] != 1 or data[40] >= PARTS or data[41] != PARTS or
            int.from_bytes(data[42:44], 'little') != SIZE or
            data[48] != (84 if data[40] == 3 else 100) or not 1 <= data[49] < 8 or
            any(data[50+data[48]:150])):
        raise ValueError('tdoa_replay fragment envelope')
    for key in ('raw_mm', 'calibrated_mm', 'raw_valid', 'calibration', 'calibrated_valid'):
        out.pop(key)
    out.update(replay_schema=data[11], diagnostic_id=int.from_bytes(data[24:28], 'little'),
               diagnostic_dropped=int.from_bytes(data[28:32], 'little'), cycle=out['transaction'],
               part=data[40], parts=PARTS, pair_slot=data[49],
               digest=f'{int.from_bytes(data[44:48], "little"):08X}',
               payload=data[50:50+data[48]].hex().upper())
    return out


class Reader:
    def __init__(self, blob):
        self.blob, self.at = blob, 0

    def take(self, n):
        value = self.blob[self.at:self.at+n]
        self.at += n
        if len(value) != n:
            raise ValueError('truncated replay snapshot')
        return value

    def num(self, n, signed=False):
        return int.from_bytes(self.take(n), 'little', signed=signed)

    def fields(self, spec):
        return {key: self.num(n, signed) for key, n, signed in spec}


def snapshot(blob, node):
    if len(blob) != SIZE or blob[0] not in (1, 2):
        raise ValueError('replay snapshot size/version')
    r = Reader(blob)
    version, planned, slot, stationary = r.take(4)
    cycle = r.num(4)
    seen, count = r.take(2)
    source_version = r.num(1)
    if source_version > 1 or r.num(1):
        raise ValueError('replay reserved header')
    start = r.num(4)
    configuration = r.take(64)
    schedule = configuration[4:36] if planned else configuration[:32]
    if (planned > 1 or stationary > 1 or count != schedule[2] or not 2 <= count <= 8 or
            not 1 <= slot < count or seen >> count or schedule[3] or schedule[0] not in (1, 2) or
            binascii.crc32(schedule[:28]) != int.from_bytes(schedule[28:], 'little') or
            (not planned and any(configuration[32:])) or
            (planned and binascii.crc32(configuration[:60]) != int.from_bytes(configuration[60:], 'little'))):
        raise ValueError('replay configuration/CRC')
    ids = list(schedule[12:12+count])
    if len(set(ids)) != count or 255 in ids or node in ids:
        raise ValueError('replay roster')
    observations = []
    for index in (0, slot):
        source, receiver = r.take(8), r.take(8)
        o = dict(source_clock=source.hex().upper(), receiver_clock=receiver.hex().upper(),
                 sequence=r.num(4), tx_dtu=r.num(5), raw_rx_dtu=r.num(5), corrected_rx_dtu=r.num(5),
                 generation=r.num(4), accepted_ms=r.num(4), power_centi_dbm=r.num(2, True),
                 power_delta_dtu=r.num(1, True), power_status=r.num(1), power_table=r.num(1))
        flags = r.num(1)
        o.update(valid=bool(flags & 1), remote_epoch_known=bool(flags & 2), has_sequence=bool(flags & 4),
                 received_this_cycle=bool(seen & (1 << index)), node=ids[index],
                 tx_domain=f'ALR{ids[index]} hardware native40', rx_domain=f'RLR{node} hardware native40')
        if flags > 7 or receiver != bytes([node, 0, 0, 0, 0, 0, 0xcf, 0xbc]):
            raise ValueError('replay receiver clock domain')
        if o['received_this_cycle'] and (source != bytes([ids[index], 0, 0, 0, 0, 0, 0xcf, 0xbc])):
            raise ValueError('replay source clock domain/cycle')
        observations.append(o)
    relation = r.fields([('reference_origin_dtu', 5, False), ('follower_origin_dtu', 5, False),
                         ('epoch', 8, False), ('skew_ppb', 4, True), ('reference_sequence', 4, False),
                         ('twr_sequence', 4, False), ('origin_age_ms', 2, False), ('delay_age_ms', 2, False),
                         ('remaining_ms', 2, False), ('delay_dtu', 2, False), ('max_age_ms', 2, False)])
    flags, period = r.take(2)
    relation.update(available=bool(flags & 1), power_corrected_twr=bool(flags & 2), version=(flags >> 2) & 3,
                    delay_state=(flags >> 4) & 7, external_pose=bool(flags & 128), period_10ms=period,
                    received_this_cycle=bool(seen & (1 << slot)),
                    reference_domain=f'ALR{ids[0]} hardware native40', follower_domain=f'ALR{ids[slot]} hardware native40',
                    skew_units='signed ppb; follower relative to reference',
                    provenance='external_pose' if flags & 128 else 'on_air_twr')
    clock = r.fields([('epoch', 8, False), ('sequence', 4, False), ('reference_tx_dtu', 5, False),
                      ('local_rx_dtu', 5, False), ('observed_ms', 4, False), ('applied_ppb', 4, True),
                      ('status', 1, False), ('has_baseline', 1, False), ('count', 1, False), ('next', 1, False)])
    clock.update(period_ms=r.num(2), samples=r.num(4), config_wire=r.take(12).hex().upper())
    fit = r.fields([('epoch', 8, False), ('fitted_ppb', 4, True), ('rms_dtu', 4, False),
                    ('uncertainty_ppb', 4, False), ('rejected', 4, False), ('accepted', 4, False),
                    ('resets', 4, False), ('count', 1, False), ('inliers', 1, False), ('ready', 1, False),
                    ('applied', 1, False), ('fallback', 1, False), ('compensated', 1, False)])
    if version == 2:
        fit['innovation_abs_dtu'] = fit['rms_dtu'] if clock['config_wire'][2:4] == '02' else None
        fit['rms_dtu'] = None
    ages = [r.num(2), r.num(2)]
    status = [r.num(4), r.num(4)]
    now, mismatch = r.num(4), r.num(1)
    intermediate_names = ('tx_separation_ref_dtu', 'raw_arrival_ref_dtu', 'corrected_arrival_ref_dtu', 'raw_tdoa_ref_dtu', 'corrected_tdoa_ref_dtu')
    intermediates = {name: r.num(8, True) for name in intermediate_names}
    intermediate_valid = r.num(1)
    if mismatch > 1 or intermediate_valid > 1 or r.at != 368:
        raise ValueError('replay reserved state')
    expected = r.fields([('tdoa_dtu', 4, True), ('range_diff_mm', 4, True), ('status', 1, False),
                         ('clock_valid', 1, False), ('rate_state', 1, False), ('reason', 1, False), ('source', 1, False)])
    if any(r.take(SIZE-r.at)):
        raise ValueError('replay reserved result')
    return dict(replay_schema=version, calculation_revision='506913dc676d0a3c0d43c48977ef11be2085aa34', source_version=source_version, cycle=cycle, pair_slot=slot, reference_node=ids[0], follower_node=ids[slot],
                alr_ids=ids, seen_mask=seen, complete=bool(seen & 1 and seen & (1 << slot)),
                cycle_start_ms=start, evaluation_ms=now, stationary=bool(stationary), planned=bool(planned),
                configuration_wire=configuration.hex().upper(),
                schedule_revision=int.from_bytes(schedule[8:12], 'little'),
                schedule_fingerprint=f'{int.from_bytes(configuration[60:64] if planned else schedule[28:32], "little"):08X}',
                slot_ms=int.from_bytes(schedule[4:6], 'little'), cycle_ms=int.from_bytes(schedule[6:8], 'little'),
                observations=observations, relation=relation, clock=clock, fit=fit,
                capture_age_ms=ages, rx_status=status, intermediates=intermediates, intermediates_captured=bool(intermediate_valid), admission_mismatch=bool(mismatch), expected=expected,
                blob=blob.hex().upper(), timestamp_units='native DTU 1/63897600000s, modulo 2^40',
                replay_scope='receiver-only; received on-air follower relation held fixed')


class Reassembly:
    """At most eight incomplete snapshots; flush losses at each connection boundary."""
    def __init__(self):
        self.pending = OrderedDict()

    def finish(self, reason='connection_end'):
        rows = [dict(type='tdoa_replay_incomplete', s=value['s'], diagnostic_id=key[-1],
                     cycle=value['cycle'], pair_slot=value['slot'], received_parts=sorted(value['parts']),
                     reason=reason) for key, value in self.pending.items()]
        self.pending.clear()
        return rows

    def accept(self, row, sid):
        key = (row['node'], row['radio'], row['epoch'], row['diagnostic_id'])
        rows = []
        if key not in self.pending:
            # The STM producer serializes one blob at a time. Once a newer ID
            # arrives on this ordered stream, an older partial cannot complete.
            for old_key, old in list(self.pending.items()):
                delta = (key[-1]-old_key[-1]) & 0xffffffff
                if old_key[:3] == key[:3] and 0 < delta < 0x80000000:
                    del self.pending[old_key]
                    rows.append(dict(type='tdoa_replay_incomplete', s=old['s'], diagnostic_id=old_key[-1],
                                     cycle=old['cycle'], pair_slot=old['slot'], received_parts=sorted(old['parts']),
                                     reason='new_snapshot', fragment_sequences=list(old['sequences'].values())))
            if len(self.pending) >= 8:
                old_key, old = self.pending.popitem(last=False)
                rows.append(dict(type='tdoa_replay_incomplete', s=old['s'], diagnostic_id=old_key[-1],
                                 cycle=old['cycle'], pair_slot=old['slot'], received_parts=sorted(old['parts']), reason='assembly_capacity'))
            self.pending[key] = dict(parts={}, sequences={}, s=sid, cycle=row['cycle'], slot=row['pair_slot'], digest=row['digest'])
        item = self.pending[key]
        if (item['cycle'], item['slot'], item['digest']) != (row['cycle'], row['pair_slot'], row['digest']):
            del self.pending[key]
            raise ValueError('replay fragment association conflict')
        part = row['part']
        if part in item['parts'] and item['parts'][part] != row['payload']:
            del self.pending[key]
            raise ValueError('replay duplicate fragment conflict')
        item['parts'][part], item['sequences'][part] = row['payload'], row['sequence']
        if len(item['parts']) == PARTS:
            del self.pending[key]
            blob = bytes.fromhex(''.join(item['parts'][i] for i in range(PARTS)))
            if f'{digest(blob):08X}' != item['digest']:
                raise ValueError('replay snapshot integrity')
            result = snapshot(blob, row['node'])
            if (result['cycle'], result['pair_slot']) != (row['cycle'], row['pair_slot']):
                raise ValueError('replay fragment/blob identity')
            rows.append(dict(result, type='tdoa_replay', s=sid, node=row['node'], radio=row['radio'], epoch=row['epoch'],
                             diagnostic_id=row['diagnostic_id'], diagnostic_dropped=row['diagnostic_dropped'],
                             fragment_sequences=[item['sequences'][i] for i in range(PARTS)], snapshot_digest=item['digest']))
        return rows
