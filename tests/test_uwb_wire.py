"""Actual portable C producer -> workstation parser -> offline acquisition tool."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).parents[1]
def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/f'{name}.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
class Wire(unittest.TestCase):
    def test_c_producer_to_collector_and_analysis(self):
        with tempfile.TemporaryDirectory() as work:
            p=Path(work);(p/'produce.c').write_text('''
#include "mul_telemetry.h"
#include <stdio.h>
int main(void){
 mulTelemetry_t r={.type=mulTlmObservation,.node=10,.peer=7,.radio=2,.remoteRadio=1,.remoteEpoch=9,.epoch=8,.mode=3,.role=2,.flags=8,.transaction=42,.rawMm=-1234,.calMm=5678,.radioErrors=99,.timeouts=7,.txDropped=3,.modelVersion=19,.stamps={0xFFFFFFFFFFULL,0x0123456789ULL}};
 char line[MUL_TLM_LINE];if(!mulTelemetryEncode(line,sizeof(line),&r))return 1;puts(line);
 r.node=11;r.stamps[0]=1;if(!mulTelemetryEncode(line,sizeof(line),&r))return 1;puts(line);return 0;
}
''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'ThirdParty/mul_core'),str(p/'produce.c'),str(ROOT/'ThirdParty/mul_core/mul_telemetry.c'),'-o',str(p/'produce')],check=True)
            lines=subprocess.check_output([str(p/'produce')],text=True).splitlines()
        collect,tdoa=module('uwb_collect'),module('uwb_tdoa');records=[]
        for line in lines:
            r=collect.decode(line);r['kind']='telemetry';records.append(r)
        self.assertEqual(records[0]['stamps'][:2],['FFFFFFFFFF','0123456789'])
        self.assertEqual((records[0]['raw_mm'],records[0]['calibrated_mm']),(-1234,5678))
        self.assertEqual((records[0]['radio_errors'],records[0]['timeouts'],records[0]['model_version']),(99,7,19))
        self.assertEqual(records[0]['remote_epoch'],'0000000000000009')
        rows=list(tdoa.analyze(records));self.assertEqual(rows[-1]['status'],'missing_sync')
if __name__=='__main__':unittest.main()
