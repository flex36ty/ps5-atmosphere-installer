"""Exercise ordered rendering across the CPU batch capacity boundary."""
import ctypes as c
class Api(c.Structure):
    _fields_=[('version',c.c_uint32),('size',c.c_uint32)]+[(n,c.c_void_p) for n in ('open','close','begin','present','rect','text','measure','texture','image','delete_texture','error')]
lib=c.CDLL('./obj/gl/host-renderer.so');a=Api();a.version=1;a.size=c.sizeof(a)
lib.atmosphere_gl_start.argtypes=[c.c_size_t,c.c_void_p]
assert lib.atmosphere_gl_start(c.sizeof(a),c.byref(a))==0
assert c.CFUNCTYPE(c.c_int)(a.open)()==0
begin=c.CFUNCTYPE(None,c.c_uint32)(a.begin)
rect=c.CFUNCTYPE(None,*([c.c_float]*5),c.c_uint32,c.c_uint32)(a.rect)
pixels=(c.c_uint32*(1920*1080))();lib.atmosphere_gl_readback.argtypes=[c.c_void_p]
begin(0xff000000)
for i in range(40000):
    color=0xffff0000 if i%2 else 0xff00ff00
    rect(10,10,20,20,0,color,color)
assert lib.atmosphere_gl_readback(pixels)==0
assert pixels[(1079-20)*1920+20]&0xffffff==0xff0000
rect(10,10,20,20,0,0xff00ff00,0xff00ff00)
begin(0xff000000) # Clearing must happen AFTER the previously queued draw.
assert lib.atmosphere_gl_readback(pixels)==0
assert pixels[(1079-20)*1920+20]&0xffffff==0
c.CFUNCTYPE(None)(a.close)()
print('PASS: 40000 ordered draws across batch overflow; queued draws respect a subsequent clear')
