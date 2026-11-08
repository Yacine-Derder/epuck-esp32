"""Additive RF acquisition projection. No clock mapping or geometric correction.

Original frame hex is always retained. Unsupported/malformed payloads remain
available as raw envelopes; this projection never upgrades them to measurements.
"""

def project_raw(out):
    frame = bytes.fromhex(out.get('multi_frame', out.get('legacy_frame', '')))
    u = lambda at, n: int.from_bytes(frame[at:at+n], 'little')
    if len(frame) not in (34, 40, 74, 80, 84, 90):
        raise ValueError('unsupported RF length')
    if frame[:2] != b'\x41\xdc' or frame[3:5] != b'\xcf\xbc' or frame[21:24] != b'MUL' or frame[2] != frame[25]:
        raise ValueError('RF prefix/sequence')
    source, destination = frame[13:21], frame[5:13]
    marker = out.get('marker_dtu', out.get('rx'))
    event = out.get('direction', 'rx')
    result = dict(projection_version=1, source_node=source[0], source_address=source.hex().upper(),
        destination_address=destination.hex().upper(), source_clock_domain=dict(node=source[0], address=source.hex().upper(), radio=out['radio'] if event=='tx' else out.get('remote_radio'), counter_bits=40, epoch=None),
        local_clock_domain=out['local_clock_domain'], local_event=event,
        local_marker_dtu=int(marker,16), rf_sequence=u(25,4), rf_cycle=None, tdma_slot=None,
        plan_fingerprint=out.get('schedule_fingerprint'), plan_revision=out.get('schedule_revision'),
        hardware_marker_valid=bool(out['flags']&8), capture_age_ms=out['capture_age_ms'], capture_valid=out['capture_age_ms']<=15,
        processing_uptime_ms=out['uptime_ms'], dtu_nominal_hz=63897600000,
        timestamp_provenance='radio hardware marker; processing uptime is not radio time',
        geometric_tdoa_valid=False, geometric_tdoa_dtu=None)
    if len(frame) in (40,80):
        slot=frame[35]
        if frame[24] != (0x31 if slot else 0x30) or len(frame)!=(80 if slot else 40):
            raise ValueError('RF broadcast slot/length')
        result.update(radio_protocol_version=frame[34],kind='downlink_broadcast', rf_cycle=u(25,4), tdma_slot=slot,
            tx_timestamp_dtu=u(29,5), tx_timestamp_provenance='advertised antenna-referenced TX',
            reference_node=source[0] if slot==0 else None,
            reference_identity_provenance='own reference TX' if slot==0 else 'lookup by RF plan and slot',
            relation=None)
        if slot:
            version, flags=frame[40:42]
            if version not in (1,2,3):raise ValueError('RF relation version')
            result['relation']=dict(version=version, available=bool(flags&1),
                epoch=f'{u(42,8):016X}', epoch_scope='follower/reference mapping; not a global transmitter boot ID',
                reference_sequence=u(50,4), propagation_sequence=u(54,4), reference_origin_dtu=u(58,5),
                follower_origin_dtu=u(63,5), apparent_rate_ppb=int.from_bytes(frame[68:72],'little',signed=True),
                independently_validated_oscillator_rate=False,
                origin_age_ms=u(72,2), propagation_age_ms=u(74,2),
                propagation_delay_dtu=u(76,2) if version>=2 else None,
                propagation_max_age_ms=u(78,2) if version>=2 else 1000,
                propagation_state=(flags>>3)&7 if version>=2 else None,
                propagation_source=('twr' if version<3 else {1:'external',2:'none'}.get(flags>>6,'unknown')),
                power_corrected_twr=bool(flags&4), physical_uncertainty='UNKNOWN')
    elif len(frame)==90:
        stage=frame[24]-0x18
        if not 0<=stage<=4 or frame[68]!=1:raise ValueError('RF TWR stage/version')
        initiator,responder=(source[0],destination[0]) if stage in (0,2,4) else (destination[0],source[0])
        result.update(radio_protocol_version=frame[84],kind='twr_exchange', rf_cycle=u(25,4), operation=frame[85], stage=stage,
            initiator_node=initiator,responder_node=responder,tx_timestamp_dtu=u(59,5),
            tx_timestamp_provenance='advertised antenna-referenced TX',
            exchange_stamps_dtu=[u(29+5*i,5) for i in range(6)],
            exchange_stamp_clock_nodes=[initiator,responder,responder,initiator,initiator,responder],
            exchange_stamps_present=(1,3,5,6,6)[stage],
            exchange_stamp_provenance='packet stamps; RX power correction metadata must be retained',
            power=[dict(power_centi_dbm=int.from_bytes(frame[69+5*i:71+5*i],'little',signed=True),
                delta_dtu=int.from_bytes(frame[71+5*i:72+5*i],'little',signed=True),
                status=frame[72+5*i],table=frame[73+5*i]) for i in range(3)])
    else:
        # Legacy hex stays available through the established native hybrid tool.
        raise ValueError('legacy RF payload; use legacy_frame and hybrid decoder')
    return result


def add_raw_projection(out):
    try:out['radio_observation']=project_raw(out)
    except (ValueError,KeyError,TypeError) as error:
        out['radio_observation']=None
        out['radio_observation_error']=str(error)
    return out
