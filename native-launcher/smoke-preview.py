"""Check the diagnostic's success, cleanup and reopen behavior on host Mesa."""
import ctypes as c
class Api(c.Structure):
    _fields_ = [('version',c.c_uint32),('size',c.c_uint32)] + [(n,c.c_void_p) for n in ('open','close','begin','present','rect','text','measure','texture','image','delete_texture','error')]
lib=c.CDLL('./obj/gl/host-smoke.so')
a=Api();a.version=1;a.size=c.sizeof(a)
lib.atmosphere_gl_start.argtypes=[c.c_size_t,c.c_void_p]
assert lib.atmosphere_gl_start(c.sizeof(a),c.byref(a))==0
for attempt in range(2):
    assert c.CFUNCTYPE(c.c_int)(a.open)()==-1 # Deliberate return to software after test.
    message=c.CFUNCTYPE(c.c_char_p)(a.error)().decode()
    assert message=='GPU smoke test passed; software mode restored',message
print('PASS: clear, triangle, presentation, teardown and reopen; deliberate software fallback')
