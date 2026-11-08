"""Actual canonical RF encoders and STM radio owner -> additive raw projection."""
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_uwb_collect import collector
from tools.uwb_logging import CompactLog
from tools.uwb_raw import add_raw_projection

ROOT=Path(__file__).resolve().parents[1]
STM=ROOT.parent/'epuck-stm32'

class RawTiming(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not STM.exists():raise unittest.SkipTest('requires existing sibling STM32 checkout')
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        path=Path(cls.temp.name);exe=path/'radio';wire=path/'radio.uart';expected=path/'expected.txt'
        subprocess.run(['python3',str(STM/'tools/build_replay_host.py'),'--raw-pipeline','--motion-guard','--disabled','--sanitize','--output',str(exe)],check=True)
        result=subprocess.run([str(exe)],env=dict(os.environ,MUL_RAW_UART=str(wire),MUL_RAW_EXPECTED=str(expected)),check=True,text=True,capture_output=True)
        cls.summary=json.loads(result.stdout)
        cls.rows=[collector.decode(s.split('\t',1)[1]) for s in wire.read_text().splitlines()]
        cls.expected={tuple(map(int,s.split()[:2])):tuple(map(int,s.split()[2:])) for s in expected.read_text().splitlines()}

    def test_all_accepted_broadcasts_survive_guard_without_replay(self):
        raw=[r for r in self.rows if r['type']=='multi_rx']
        actual={}
        for r in raw:
            o=r['radio_observation'];self.assertTrue(o['hardware_marker_valid'])
            self.assertIsNone(o['geometric_tdoa_dtu']);self.assertFalse(o['geometric_tdoa_valid'])
            if o['kind']!='downlink_broadcast':continue
            actual[(o['rf_cycle'],o['tdma_slot'])]=(o['source_node'],o['tx_timestamp_dtu'],o['local_marker_dtu'])
            self.assertEqual(o['plan_fingerprint'],'EC2B4B2A')
            self.assertIsNone(o['source_clock_domain']['epoch'])
            if o['tdma_slot']:self.assertEqual(o['relation']['epoch'],f"{o['tdma_slot']+10:016X}")
        self.assertEqual(actual,self.expected);self.assertEqual(len(actual),479)
        self.assertEqual(len(raw),694)
        self.assertEqual(self.summary['corrected_components'],0)
        self.assertFalse(any(r['type']=='tdoa_replay_fragment' for r in self.rows))

    def test_twr_direction_power_and_domains_remain_distinct(self):
        twr=[r['radio_observation'] for r in self.rows if r.get('radio_observation',{}).get('kind')=='twr_exchange']
        self.assertEqual(len(twr),215)
        self.assertEqual(sum(o['local_event']=='tx' for o in twr),86)
        for o in twr:
            self.assertEqual(o['exchange_stamp_clock_nodes'],[o['initiator_node'],141,141,o['initiator_node'],o['initiator_node'],141])
            self.assertEqual(len(o['power']),3)
            if o['local_event']=='tx':
                self.assertEqual(o['local_marker_dtu'],o['tx_timestamp_dtu'])
                self.assertEqual(o['source_clock_domain']['radio'],2)
            else:self.assertEqual(o['source_clock_domain']['radio'],1)
        self.assertEqual(sum(r['type']=='ar_range' for r in self.rows),43)

    def test_compact_retains_raw_bytes_and_projection_without_geometric_validity(self):
        output=io.StringIO();log=CompactLog(output,'offline')
        for row in self.rows:
            if row['type']=='multi_rx':log.accept(dict(row,kind='telemetry',host_received_utc='2026-11-08T00:00:00Z'))
        saved=[r for r in map(json.loads,output.getvalue().splitlines()) if r['type']=='multi_rx']
        self.assertEqual(len(saved),694)
        self.assertEqual([r['multi_frame'] for r in saved],[r['multi_frame'] for r in self.rows if r['type']=='multi_rx'])

    def test_unknown_payload_does_not_destroy_original_raw_envelope(self):
        row=dict(next(r for r in self.rows if r['type']=='multi_rx'))
        original='00'+row['multi_frame'][2:];row['multi_frame']=original
        self.assertIs(add_raw_projection(row),row)
        self.assertIsNone(row['radio_observation'])
        self.assertEqual(row['multi_frame'],original)
        self.assertIn('RF prefix',row['radio_observation_error'])
