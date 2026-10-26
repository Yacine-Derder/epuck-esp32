"""Strict planned AR result/quality decoding; raw hardware clock domains stay distinct."""
import struct
REASONS=('none','broadcast','clock','deadline','radio','antenna','target','late','start','tx_stamp','packet_or_window','math','rearm','encode_or_write','stale_tx','cancelled')

def decode_ar(data,out):
    if data[2]!=3 or data[6]!=2 or data[8]!=2 or data[7] not in (4,5) or data[4]==255 or data[10]!=1 or data[11]!=4:
        raise ValueError('AR envelope')
    for key in ('raw_mm','calibrated_mm','raw_valid','calibration','calibrated_valid'):
        out.pop(key)
    out.update(schedule_profile=4,fingerprint_scope='ranging_plan',source='stm32_onboard_twr',dtu_nominal_hz=63897600000,
               physical_uncertainty='UNKNOWN',local_clock_domain=dict(node=out['node'],radio=2,epoch=out['epoch']))
    if data[3]==13:
        cycle,fingerprint,revision=struct.unpack_from('<III',data,40)
        initiator,responder,operation,reply=data[52:56]
        raw,tof,reported=struct.unpack_from('<HHI',data,56)
        if (data[9]!=9 or data[111]!=1 or data[110]!=1 or data[109]!=255 or not revision or initiator==responder or initiator==255
            or responder!=out['node'] or initiator!=out['peer'] or operation>=32 or reply not in (5,10,20) or raw>63898 or tof>63898
            or (reported-out['uptime_ms'])&0xffffffff>200 or out['transaction']!=cycle or any(data[140:150])):
            raise ValueError('AR result')
        powers=[]
        for i in range(3):
            dbm,delta,status,table=struct.unpack_from('<hbBB',data,94+5*i)
            if (status>4 or table>2 or not -64<=delta<=64 or (status!=1 and delta)
                or (status==1 and (not table or not -9500<=dbm<=-6100)) or (status==0 and (table or dbm or delta))):
                raise ValueError('AR power')
            powers.append(dict(power_centi_dbm=None if dbm==-32768 else dbm,delta_dtu=delta,status=status,table=table))
        corrected=all(p['status']==1 for p in powers)
        if not corrected and raw!=tof: raise ValueError('AR correction unavailable')
        stamps=[f'{int.from_bytes(data[64+5*i:69+5*i],"little"):010X}' for i in range(6)]
        out.update(cycle=cycle,schedule_fingerprint=f'{fingerprint:08X}',schedule_revision=revision,
                   initiator_node=initiator,responder_node=responder,operation=operation,reply_ms=reply,
                   raw_tof_dtu=raw,tof_dtu=tof,reported_uptime_ms=reported,acquisition_uptime_ms=out['uptime_ms'],
                   measurement_age_at_report_ms=(reported-out['uptime_ms'])&0xffffffff,stamps=stamps,power=powers,
                   power_corrected=corrected,calibrated=False,valid=True,tof_clock_node=initiator,
                   stamp_clock_nodes=[initiator,responder,responder,initiator,initiator,responder],
                   raw_range_mm=round(raw*299792458000/63897600000),range_mm=round(tof*299792458000/63897600000),
                   dropped=int.from_bytes(data[112:116],'little'),uart_errors=int.from_bytes(data[116:120],'little'),tx_dropped=int.from_bytes(data[120:124],'little'),
                   queue_depth=int.from_bytes(data[124:126],'little'),queue_high=int.from_bytes(data[126:128],'little'),
                   completed=int.from_bytes(data[128:132],'little'),aborted=int.from_bytes(data[132:136],'little'),rejected=int.from_bytes(data[136:140],'little'),
                   remote_epoch_known=False)
    else:
        fingerprint,revision,cycle=struct.unpack_from('<III',data,40)
        active,pending,reference=data[96:99]
        reason,operation,stage,enabled=data[100:104]
        if (data[9] or out['peer']!=255 or not revision or max(active,pending,reference,enabled)>1 or (pending and not active)
            or (active and (not enabled or not reference)) or reason>=len(REASONS) or (operation!=255 and operation>=32)
            or (stage and not 0x10<=stage<=0x14) or data[99] or data[104]!=1 or any(data[105:150])):
            raise ValueError('AR quality')
        names=('attempts','completed','aborted','rejected','math_invalid','result_dropped','raw_dropped','late','radio_errors','timeouts','reset_generation')
        out.update(zip(names,struct.unpack_from('<11I',data,52)))
        out.update(schedule_fingerprint=f'{fingerprint:08X}',schedule_revision=revision,cycle=cycle,
                   active=bool(active),pending=bool(pending),reference_seen=bool(reference),enabled=bool(enabled),
                   last_reason=REASONS[reason],operation=None if operation==255 else operation,stage=stage)
    return out
