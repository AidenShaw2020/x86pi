"""Type into the guest over the same serial link that carries the log.

The kernel reads frames of FEh, press/release, keycode - see DrainSerialKeys()
in pc/kernel.cpp.  Codes are Linux/AT keycodes, the same ones the USB path
produces, so both arrive at ps2_put_keycode() identically.

Text is sent as press/release pairs with Shift held where the character needs
it.  Only the characters DOS actually needs are mapped; anything else is
reported rather than silently dropped, because a setup menu that does not
respond is hard enough to debug without guessing which keystroke went missing.
"""
import argparse
import time
import serial

KEY = {
    'esc':1,'1':2,'2':3,'3':4,'4':5,'5':6,'6':7,'7':8,'8':9,'9':10,'0':11,
    '-':12,'=':13,'bs':14,'tab':15,
    'q':16,'w':17,'e':18,'r':19,'t':20,'y':21,'u':22,'i':23,'o':24,'p':25,
    '[':26,']':27,'enter':28,'ctrl':29,
    'a':30,'s':31,'d':32,'f':33,'g':34,'h':35,'j':36,'k':37,'l':38,
    ';':39,"'":40,'`':41,'shift':42,'\\':43,
    'z':44,'x':45,'c':46,'v':47,'b':48,'n':49,'m':50,
    ',':51,'.':52,'/':53,'rshift':54,'*':55,'alt':56,'space':57,'caps':58,
    'f1':59,'f2':60,'f3':61,'f4':62,'f5':63,'f6':64,'f7':65,'f8':66,'f9':67,'f10':68,
    'up':103,'left':105,'right':106,'down':108,
    'home':102,'end':107,'pgup':104,'pgdn':109,'ins':110,'del':111,
    'f11':87,'f12':88,
    # The Windows key, which the on-screen menus use as their modifier.
    'meta':125,'rmeta':126,
}
SHIFTED = {'!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8',
           '(':'9',')':'0','_':'-','+':'=','{':'[','}':']',':':';','"':"'",
           '~':'`','|':'\\','<':',','>':'.','?':'/'}

def frames(name, down):
    code = KEY.get(name)
    if code is None:
        raise KeyError(name)
    return bytes((0xfe, 1 if down else 0, code))

def main():
    p = argparse.ArgumentParser()
    p.add_argument("port")
    p.add_argument("keys", nargs="+",
                   help="Literal text, or key names in braces such as {enter} {down} {esc}. "
                        "{+name} holds a key down and {-name} releases it, for "
                        "chords such as {+meta} {f11} {-meta}.")
    p.add_argument("--log", metavar="FILE",
                   help="After the keys, keep the port open and record what the board "
                        "says into FILE. Only one process can hold the port, so a "
                        "separate logger cannot start until this one exits - and a "
                        "fault provoked by the last keystroke lands in that gap.")
    p.add_argument("--log-seconds", type=float, default=45,
                   help="How long to keep recording with --log")
    p.add_argument("--delay", type=float, default=0.03,
                   help="Seconds between key events")
    a = p.parse_args()

    seq = []
    for item in a.keys:
        if item.startswith("{") and item.endswith("}"):
            seq.append((item[1:-1].lower(), False))
            continue
        for chexp in item:
            ch = chexp
            shift = False
            if ch in SHIFTED:
                ch, shift = SHIFTED[ch], True
            elif ch.isupper():
                ch, shift = ch.lower(), True
            if ch == ' ':
                ch = 'space'
            seq.append((ch, shift))

    uart = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    uart.rts = False        # never pulse the board's RUN line from here
    uart.dtr = False
    uart.port = a.port
    uart.open()
    try:
        for name, shift in seq:
            hold = release = False
            if name[:1] == '+': hold, name = True, name[1:]
            elif name[:1] == '-': release, name = True, name[1:]
            try:
                down = frames(name, True)
                up = frames(name, False)
            except KeyError:
                print(f"no keycode for {name!r}; skipped")
                continue
            if hold:
                uart.write(down); time.sleep(a.delay); continue
            if release:
                uart.write(up); time.sleep(a.delay); continue
            if shift:
                uart.write(frames('shift', True)); time.sleep(a.delay)
            uart.write(down); time.sleep(a.delay)
            uart.write(up); time.sleep(a.delay)
            if shift:
                uart.write(frames('shift', False)); time.sleep(a.delay)
        uart.flush()
        print(f"sent {len(seq)} key(s)")
        if a.log:
            import pathlib
            end = time.time() + a.log_seconds
            with pathlib.Path(a.log).open("wb") as log:
                while time.time() < end:
                    data = uart.read(max(1, uart.in_waiting))
                    if data:
                        log.write(data)
                        log.flush()
            print(f"recorded {a.log_seconds:.0f}s to {a.log}")
    finally:
        uart.close()

if __name__ == "__main__":
    main()
