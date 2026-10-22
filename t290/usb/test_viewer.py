import io
import unittest
import viewer

def packet(data, *, width=3, height=2, stride=9, shift=1):
    return viewer.HEADER.pack(viewer.MAGIC,1,1,7,width,height,stride,viewer.RBG3,shift,len(data),123,0)+data

class ViewerTests(unittest.TestCase):
    def test_fragmented_native_panel_and_channels(self):
        data=bytes([10,30,20, 40,60,50, 70,90,80, 100,120,110, 130,150,140, 160,180,170])
        stream=io.BytesIO(packet(data))
        result=viewer.read_packet(lambda n: stream.read(min(n,3)))
        image=viewer.decode_frame(result)
        self.assertEqual(image.size,(3,2))
        self.assertEqual(list(image.getdata()),[
            (0,0,0),(10,20,30),(40,50,60),
            (70,80,90),(100,110,120),(130,140,150)])
        self.assertEqual(result.sequence,7)
        self.assertEqual(result.wire,packet(data))

    def test_rejects_oversized_payload_before_reading_it(self):
        header=viewer.HEADER.pack(viewer.MAGIC,1,2,0,0,0,0,0,0,viewer.MAX_BYTES+1,0,0)
        with self.assertRaisesRegex(ValueError,'limit'):
            viewer.read_packet(io.BytesIO(header).read)

    def test_rejects_inconsistent_geometry(self):
        with self.assertRaisesRegex(ValueError,'layout'):
            viewer.read_packet(io.BytesIO(packet(bytes(18),stride=8)).read)

    def test_preserves_log_bytes(self):
        text=b'backboardd: t290eagl: GL error 0x502\n'
        wire=viewer.HEADER.pack(viewer.MAGIC,1,2,9,0,0,0,0,0,len(text),10,0)+text
        result=viewer.read_packet(io.BytesIO(wire).read)
        self.assertEqual(result.kind,2)
        self.assertEqual(result.payload,text)

if __name__=='__main__': unittest.main()
