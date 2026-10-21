import asyncio
import binascii
import importlib.util
import io
import json
from pathlib import Path
import struct
import unittest
spec=importlib.util.spec_from_file_location('collector',Path(__file__).parents[1]/'tools/uwb_collect.py')
collector=importlib.util.module_from_spec(spec);spec.loader.exec_module(collector)
def fixture(seq=1,epoch=42,version=2):
    data=bytearray(128 if version==1 else 152)
    data[:12]=bytes([77,84,version,2,16,17,2,1,1,15,2,0])
    struct.pack_into('<IQQIIii',data,12,seq,epoch,123,99,1000,-100,900)
    data[96:101]=(0xffffffffff).to_bytes(5,'little')
    struct.pack_into('<H',data,len(data)-2,binascii.crc_hqx(data[:-2],0xffff))
    return f'UWB{version} '+data.hex().upper()
class Collector(unittest.TestCase):
    def test_versions_units_precision_and_crc(self):
        for v in (1,2):
            r=collector.decode(fixture(version=v))
            self.assertEqual(r['raw_mm'],-100);self.assertEqual(r['calibrated_mm'],900)
            self.assertEqual(r['stamps'][0],'FFFFFFFFFF');self.assertEqual(r['local_clock_domain']['epoch'],'000000000000002A')
            for at in (0,10,100,len(fixture(version=v))-1):
                bad=list(fixture(version=v));bad[at]='?'
                with self.assertRaises(ValueError):collector.decode(''.join(bad))
    def test_ordering_wrap_restart_and_bounded_epochs(self):
        order=collector.Ordering()
        for seq,status,missing in ((0xfffffffe,'first',0),(1,'gap',2),(1,'duplicate',0),(0,'old',0),(2,'forward',0)):
            self.assertEqual(order.accept(collector.decode(fixture(seq))),dict(sequence_status=status,missing=missing))
        self.assertEqual(order.accept(collector.decode(fixture(1,43)))['sequence_status'],'first')
        for epoch in range(1,300):order.accept(collector.decode(fixture(1,epoch)))
        self.assertLessEqual(len(order.last),128)
    def test_truncation(self):
        line=fixture()
        for length in range(len(line)):
            with self.assertRaises(ValueError):collector.decode(line[:length])
class Loopback(unittest.IsolatedAsyncioTestCase):
    async def test_reconnect_fragmented_records_and_read_only_heartbeats(self):
        stop=asyncio.Event();received=[];connections=0
        async def server(reader,writer):
            nonlocal connections
            connections+=1
            writer.write(b'SESSION 0123456789abcdef READY\n');await writer.drain()
            payload=(fixture(connections)+'\n').encode()
            writer.write(payload[:50]);await writer.drain();await asyncio.sleep(.01);writer.write(payload[50:]);await writer.drain()
            received.append(await asyncio.wait_for(reader.readline(),1))
            writer.close();await writer.wait_closed()
            if connections>=2:stop.set()
        listener=await asyncio.start_server(server,'127.0.0.1',0)
        output=io.StringIO();port=listener.sockets[0].getsockname()[1]
        try:await asyncio.wait_for(collector.collect(f'127.0.0.1:{port}',output,stop),5)
        finally:listener.close();await listener.wait_closed()
        rows=[json.loads(x) for x in output.getvalue().splitlines()]
        self.assertGreaterEqual(sum(r['kind']=='telemetry' for r in rows),2,output.getvalue())
        self.assertTrue(all(x==b'CONTROL_HB\n' for x in received))
        self.assertTrue(all('host_received_utc' in r and 'host_monotonic_ns' in r for r in rows))
if __name__=='__main__':unittest.main()
