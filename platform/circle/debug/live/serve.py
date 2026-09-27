"""Share the HDMI capture with any number of viewers.

A DirectShow device can only be opened once, and writing frames to a file does
not work around that on Windows: a browser polling the file holds a lock often
enough that ffmpeg's rewrite fails and the capture dies.  So ffmpeg is opened
once here and its MJPEG output is read from a pipe, the newest frame is kept in
memory, and HTTP hands it to as many clients as ask.

  /         a page showing the stream
  /stream   multipart/x-mixed-replace, which browsers play as live video
  /frame    the newest single JPEG, for scripted grabs
"""
import http.server
import socketserver
import subprocess
import threading
import sys

PORT = 8090
DEVICE = "video=USB Video"
WIDTH = 1280
FPS = 20

_latest = None
_lock = threading.Condition()
_seq = 0

def reader():
    global _latest, _seq
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
           "-f", "dshow", "-rtbufsize", "300M", "-framerate", "30",
           "-i", DEVICE,
           "-vf", f"scale={WIDTH}:-2", "-r", str(FPS), "-q:v", "5",
           "-f", "mpjpeg", "pipe:1"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=sys.stderr, bufsize=0)
    buf = b""
    while True:
        chunk = proc.stdout.read(65536)
        if not chunk:
            break
        buf += chunk
        while True:
            start = buf.find(b"\xff\xd8")
            if start < 0:
                if len(buf) > 1 << 20: buf = buf[-2:]
                break
            end = buf.find(b"\xff\xd9", start + 2)
            if end < 0:
                if start: buf = buf[start:]
                break
            frame = buf[start:end + 2]
            buf = buf[end + 2:]
            with _lock:
                _latest = frame
                _seq += 1
                _lock.notify_all()
    print("capture ended", file=sys.stderr)

PAGE = b"""<!DOCTYPE html><html lang="cs"><head><meta charset="utf-8">
<title>tiny386 live</title><style>
:root{color-scheme:dark}
body{margin:0;background:#0b0b0d;color:#c9c9cf;font:14px system-ui,sans-serif;
     display:flex;flex-direction:column;align-items:center;gap:.75rem;padding:1rem}
img{width:min(100%,1280px);image-rendering:pixelated;border:1px solid #2a2a31;
    border-radius:6px;background:#000;display:block}
</style></head><body>
<img id="video" src="/frame" alt="obraz z Pi">
<div>zivy stream z HDMI grabberu</div>
<script>
const video = document.getElementById('video');
function refresh() { video.src = '/frame?t=' + Date.now(); }
video.addEventListener('load', () => setTimeout(refresh, 100));
video.addEventListener('error', () => setTimeout(refresh, 500));
</script>
</body></html>"""

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def do_GET(self):
        if self.path.startswith("/stream"):
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            seen = 0
            try:
                while True:
                    with _lock:
                        while _seq == seen or _latest is None:
                            _lock.wait(5)
                        seen = _seq
                        frame = _latest
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                     b"Content-Length: " + str(len(frame)).encode()
                                     + b"\r\n\r\n" + frame + b"\r\n")
            except (BrokenPipeError, ConnectionResetError, OSError):
                return
        elif self.path.startswith("/frame"):
            with _lock:
                frame = _latest
            if frame is None:
                self.send_error(503, "no frame yet"); return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(frame)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(frame)
        else:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

if __name__ == "__main__":
    threading.Thread(target=reader, daemon=True).start()
    print(f"http://127.0.0.1:{PORT}/  (stream), /frame for a single image", flush=True)
    Server(("127.0.0.1", PORT), Handler).serve_forever()
