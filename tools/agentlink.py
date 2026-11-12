"""AgentLink v1 from Python (mirror of src/agent/agentlink.h). Windows only."""
import ctypes as C
import mmap
import time

AGL_MAGIC = 0x314C4741
AGL_SIZE = 65536
V_POS, V_VEL, V_YAW, V_HP, V_STAMINA, V_AREA, V_FLAGS, V_PROXY = (1 << i for i in range(8))
BUTTONS = ['cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'l3', 'r3', 'options',
           'touchpad', 'up', 'down', 'left', 'right']


class Header(C.Structure):
    _fields_ = [('magic', C.c_uint32), ('version', C.c_uint32), ('size', C.c_uint32), ('host_kind', C.c_uint32),
                ('pid', C.c_uint32), ('up_axis', C.c_uint32), ('caps', C.c_uint32), ('reserved0', C.c_uint32),
                ('game', C.c_char * 32), ('reserved', C.c_uint32 * 8)]


class Frame(C.Structure):
    _fields_ = [('seq', C.c_uint32), ('pad0', C.c_uint32), ('frame_id', C.c_uint64), ('host_ms', C.c_double),
                ('dt_ms', C.c_float), ('pad1', C.c_float)]


class Player(C.Structure):
    _fields_ = [('seq', C.c_uint32), ('valid', C.c_uint32), ('frame_id', C.c_uint64), ('pos', C.c_float * 3),
                ('vel', C.c_float * 3), ('yaw', C.c_float), ('pitch', C.c_float), ('hp', C.c_float),
                ('hp_max', C.c_float), ('stamina', C.c_float), ('flags', C.c_uint32), ('area_id', C.c_uint32),
                ('area', C.c_char * 32)]


class Camera(C.Structure):
    _fields_ = [('seq', C.c_uint32), ('valid', C.c_uint32), ('frame_id', C.c_uint64), ('pos', C.c_float * 3),
                ('fwd', C.c_float * 3), ('view', C.c_float * 12), ('inv_view', C.c_float * 12),
                ('proj', C.c_float * 4), ('render_w', C.c_uint32), ('render_h', C.c_uint32)]


class Command(C.Structure):
    _fields_ = [('seq', C.c_uint32), ('cmd_seq', C.c_uint32), ('issued_ms', C.c_double), ('ttl_ms', C.c_uint32),
                ('buttons', C.c_uint32), ('stick', C.c_float * 4), ('move', C.c_float * 3), ('look', C.c_float * 2),
                ('triggers', C.c_float * 2), ('teleport', C.c_float * 3), ('flags', C.c_uint32), ('pad0', C.c_uint32)]


class Entity(C.Structure):
    _fields_ = [('id', C.c_uint32), ('kind', C.c_uint32), ('pos', C.c_float * 3), ('hp_pct', C.c_float),
                ('name', C.c_char * 24)]


class Entities(C.Structure):
    _fields_ = [('seq', C.c_uint32), ('count', C.c_uint32), ('frame_id', C.c_uint64), ('e', Entity * 64)]


class Link(C.Structure):
    _fields_ = [('h', Header), ('frame', Frame), ('player', Player), ('camera', Camera), ('cmd', Command),
                ('ents', Entities)]


_qpf = C.c_int64()
C.windll.kernel32.QueryPerformanceFrequency(C.byref(_qpf))


def now_ms():
    c = C.c_int64()
    C.windll.kernel32.QueryPerformanceCounter(C.byref(c))
    return c.value * 1000.0 / _qpf.value


class AgentLink:
    def __init__(self, pid):
        self.mm = mmap.mmap(-1, AGL_SIZE, tagname=f'Local\\AgentLink_{pid}')
        self.link = Link.from_buffer(self.mm)
        if self.link.h.magic != AGL_MAGIC:
            raise RuntimeError(f'no AgentLink for pid {pid}')
        self.cmd_seq = 0

    def read(self, name):
        blk = getattr(self.link, name)
        size = C.sizeof(blk)
        addr = C.addressof(blk)
        for _ in range(1000):
            a = blk.seq
            if a & 1:
                continue
            out = type(blk).from_buffer_copy(C.string_at(addr, size))
            if blk.seq == a:
                return out
        raise RuntimeError('torn read')

    def send(self, stick=(0, 0, 0, 0), buttons=(), ttl_ms=200):
        c = self.link.cmd
        bits = 0
        for b in buttons:
            bits |= 1 << BUTTONS.index(b)
        c.seq += 1
        self.cmd_seq += 1
        c.cmd_seq = self.cmd_seq
        c.ttl_ms = ttl_ms
        c.buttons = bits
        for i in range(4):
            c.stick[i] = stick[i]
        c.issued_ms = now_ms()
        c.seq += 1
        return self.cmd_seq

    def stop(self):
        self.send(ttl_ms=0)
