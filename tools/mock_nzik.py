"""Stand-ins for the phone (N-Zik's PC bridge, contract 1.12) and for Kabuki, for host tests.

MockPhone serves the bridge's REST + WebSocket + audio routes from local files and plays a
silent timeline like the phone does while its output is "pc". MockKabuki accepts one provider
and records what it shows. Standard library only.
"""
import base64
import hashlib
import json
import os
import re
import socket
import struct
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def now_ms():
    return int(time.monotonic() * 1000)


class MockPhone:
    def __init__(self, tracks, port=42420, code='123456'):
        # tracks: list of dicts {id, title, artists, durationMs, path, art}
        self.tracks = tracks
        self.port = port
        self.code = code
        self.token = 'tok-' + os.urandom(8).hex()
        self.commands = []  # (route, body)
        self.lock = threading.Lock()
        self.revision = 0
        self.index = 0
        self.playing = False
        self.position = 0
        self.position_at = now_ms()
        self.shuffle = False
        self.repeat = 'off'
        self.output = 'phone'
        self.sockets = []
        self.ws_connected = threading.Event()
        # The library's one playlist (all tracks) and one album (the first track); names only.
        self.playlist_name = 'Mi playlist'
        self.album_title, self.album_artist = 'Album Uno', 'Artista'
        phone = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a):
                pass

            def body(self):
                n = int(self.headers.get('Content-Length') or 0)
                return json.loads(self.rfile.read(n) or b'{}') if n else {}

            def send_json(self, status, obj):
                data = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def authed(self):
                if self.headers.get('Authorization') != 'Bearer ' + phone.token:
                    self.send_json(401, {'code': 'UNAUTHORIZED', 'message': 'no'})
                    return False
                return True

            def do_GET(self):
                path = self.path
                if path == '/api/v1/meta':
                    return self.send_json(200, {'contractVersion': '1.12.0', 'serverName': 'Mock phone', 'serverTimeMs': now_ms(), 'features': []})
                if path == '/api/v1/ws':
                    if not self.authed():
                        return
                    return phone.websocket(self)
                if path.startswith('/api/v1/library/'):
                    if not self.authed():
                        return
                    route = path[len('/api/v1/library/'):].split('?')[0]
                    tracks = [phone.dto(t) for t in phone.tracks]
                    page = lambda items: {'items': items, 'total': len(items), 'offset': 0, 'limit': 200}
                    if route == 'songs':
                        return self.send_json(200, page(tracks))
                    if route == 'songs/recommendations':
                        return self.send_json(200, {'filter': None, 'items': [{'position': i, 'track': t} for i, t in enumerate(tracks)]})
                    if route == 'playlists':
                        return self.send_json(200, page([{'id': '7', 'name': phone.playlist_name, 'trackCount': len(tracks), 'artworkTrackId': None}]))
                    if route == 'playlists/7/songs':
                        return self.send_json(200, page(tracks))
                    if route == 'albums':  # song1 is on an album; the others are singles
                        return self.send_json(200, page([{'id': 'al1', 'title': phone.album_title, 'artists': phone.album_artist, 'year': None,
                                                          'trackCount': 1, 'hasArtwork': False}]))
                    if route == 'albums/al1/songs':
                        return self.send_json(200, page(tracks[:1]))
                    if route == 'artists':
                        return self.send_json(200, page([]))
                    return self.send_json(404, {'code': 'NOT_FOUND', 'message': route})
                m = re.match(r'/api/v1/artwork/([^?]+)', path)
                if m:
                    if not self.authed():
                        return
                    t = phone.track(m.group(1))
                    data = open(t['art'], 'rb').read()
                    self.send_response(200)
                    self.send_header('Content-Type', 'image/jpeg')
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    return self.wfile.write(data)
                m = re.match(r'/api/v1/audio/([^?]+)\?t=(.+)', path)
                if m:
                    t = phone.track(m.group(1))
                    if not t or m.group(2) != 'audio-' + t['id']:
                        return self.send_json(403, {'code': 'AUDIO_URL_INVALID', 'message': 'no'})
                    data = open(t['path'], 'rb').read()
                    start, end = 0, len(data) - 1
                    r = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range') or '')
                    self.send_response(206 if r else 200)
                    if r:
                        start = int(r.group(1))
                        end = int(r.group(2)) if r.group(2) else end
                        self.send_header('Content-Range', f'bytes {start}-{end}/{len(data)}')
                    self.send_header('Accept-Ranges', 'bytes')
                    self.send_header('Content-Type', 'audio/webm' if t['path'].endswith('webm') else 'audio/mp4')
                    self.send_header('Content-Length', str(end - start + 1))
                    self.end_headers()
                    try:
                        self.wfile.write(data[start:end + 1])
                    except OSError:
                        pass
                    return
                self.send_json(404, {'code': 'NOT_FOUND', 'message': path})

            def do_POST(self):
                path = self.path
                if path == '/api/v1/pairing/validate':
                    b = self.body()
                    if b.get('code') != phone.code:
                        return self.send_json(403, {'code': 'PAIRING_REJECTED', 'message': 'bad code'})
                    phone.paired_name = b.get('deviceName')
                    return self.send_json(200, {'deviceToken': phone.token, 'deviceId': 'dev1', 'serverName': 'Mock phone', 'serverPort': phone.port})
                if not self.authed():
                    return
                m = re.match(r'/api/v1/audio/([^/]+)/url', path)
                if m:
                    self.body()
                    t = phone.track(m.group(1))
                    return self.send_json(200, {'trackId': t['id'], 'quality': 'high', 'url': f'http://127.0.0.1:{phone.port}/api/v1/audio/{t["id"]}?t=audio-{t["id"]}',
                                                'expiresAtMs': 0, 'durationMs': t['durationMs']})
                route = path[len('/api/v1/'):]
                b = self.body()
                with phone.lock:
                    phone.commands.append((route, b))
                ok = phone.apply(route, b)
                self.send_json(200 if ok else 422, {'applied': ok, 'changed': ok, 'revision': phone.revision})

        self.server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        threading.Thread(target=self.timeline, daemon=True).start()

    def track(self, tid):
        return next((t for t in self.tracks if t['id'] == tid), None)

    # ---- the phone's silent player ----
    def pos(self):
        return self.position + (now_ms() - self.position_at if self.playing else 0)

    def set_pos(self, p):
        self.position, self.position_at = p, now_ms()

    def timeline(self):
        while True:
            time.sleep(0.05)
            with self.lock:
                if self.playing and self.pos() >= self.tracks[self.index]['durationMs']:
                    if self.index + 1 < len(self.tracks):
                        self.index += 1
                        self.set_pos(0)
                        self.delta_track()
                    else:
                        self.playing = False
                        self.set_pos(self.tracks[self.index]['durationMs'])
                        self.delta_playback()

    def apply(self, route, b):
        with self.lock:
            if route == 'player/play':
                self.set_pos(self.pos()); self.playing = True; self.delta_playback()
            elif route == 'player/pause':
                self.set_pos(self.pos()); self.playing = False; self.delta_playback()
            elif route == 'player/seek':
                self.set_pos(int(b['positionMs'])); self.delta_playback()
            elif route in ('player/next', 'player/previous'):
                self.index = max(0, min(len(self.tracks) - 1, self.index + (1 if route.endswith('next') else -1)))
                self.set_pos(0); self.delta_track()
            elif route == 'player/shuffle':
                self.shuffle = b['enabled']; self.delta_modes()
            elif route == 'player/repeat':
                self.repeat = b['mode']; self.delta_modes()
            elif route == 'player/output':
                if not self.sockets and b['output'] == 'pc':
                    return False
                self.output = b['output']
                self.send({'type': 'outputChanged', 'audioOutput': self.output})
            elif route.startswith('library/songs/') and route.endswith('/like'):
                return b.get('state') in ('liked', 'neutral', 'disliked')
            elif route == 'queue/list':
                if b['action'] != 'play' or b['list'] not in ({'kind': 'playlist', 'id': '7'}, {'kind': 'songs', 'filter': 'all'}):
                    return False
                self.index = int(b['startIndex']); self.set_pos(0); self.playing = True; self.delta_track()
            elif route == 'queue/jump':
                i = int(b['index'])
                if self.tracks[i]['id'] != b['trackId']:
                    return False
                self.index = i; self.set_pos(0); self.delta_track()
            else:
                return False
            return True

    # ---- WebSocket ----
    def websocket(self, h):
        key = h.headers['Sec-WebSocket-Key']
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        h.send_response(101)
        h.send_header('Upgrade', 'websocket')
        h.send_header('Connection', 'Upgrade')
        h.send_header('Sec-WebSocket-Accept', accept)
        h.end_headers()
        h.wfile.flush()
        sock = h.connection
        with self.lock:
            self.sockets.append(sock)
            self.send_snapshot(sock)
        self.ws_connected.set()
        try:
            while True:
                msg = self.read_frame(sock)
                if msg is None:
                    break
                m = json.loads(msg)
                with self.lock:
                    if m.get('type') == 'ping':
                        self.raw(sock, {'type': 'pong', 'clientTimeMs': m['clientTimeMs'], 'serverReceiveTimeMs': now_ms(), 'serverSendTimeMs': now_ms()})
                    elif m.get('type') == 'requestSnapshot':
                        self.send_snapshot(sock)
        finally:
            with self.lock:
                self.sockets.remove(sock)
                if self.output == 'pc':  # contract §6.2: back to the phone, paused
                    self.output = 'phone'
                    self.playing = False
            h.close_connection = True

    @staticmethod
    def read_frame(sock):
        def exact(n):
            buf = b''
            while len(buf) < n:
                c = sock.recv(n - len(buf))
                if not c:
                    raise ConnectionError
                buf += c
            return buf
        try:
            b0, b1 = exact(2)
            n = b1 & 127
            if n == 126:
                n = struct.unpack('>H', exact(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', exact(8))[0]
            mask = exact(4) if b1 & 128 else b'\0\0\0\0'
            data = bytes(c ^ mask[i % 4] for i, c in enumerate(exact(n)))
            if b0 & 15 == 8:
                return None
            return data.decode()
        except (ConnectionError, OSError):
            return None

    def raw(self, sock, obj):
        data = json.dumps(obj).encode()
        head = bytes([0x81]) + (bytes([len(data)]) if len(data) < 126 else bytes([126]) + struct.pack('>H', len(data)) if len(data) < 65536
                                else bytes([127]) + struct.pack('>Q', len(data)))
        try:
            sock.sendall(head + data)
        except OSError:
            pass

    def send(self, obj):  # a revised delta to every session
        self.revision += 1
        obj = dict(obj, revision=self.revision, serverTimeMs=now_ms())
        for s in self.sockets:
            self.raw(s, obj)

    def dto(self, t):
        return {'id': t['id'], 'title': t['title'], 'artists': t['artists'], 'durationMs': t['durationMs'], 'source': 'online',
                'isDownloaded': False, 'isLiked': False, 'like': 'neutral', 'hasArtwork': True}

    def send_snapshot(self, sock):
        t = self.tracks[self.index]
        self.raw(sock, {'type': 'snapshot', 'revision': self.revision, 'serverTimeMs': now_ms(), 'queue': [self.dto(x) for x in self.tracks],
                        'currentIndex': self.index, 'currentTrackId': t['id'], 'isPlaying': self.playing, 'isBuffering': False,
                        'durationMs': t['durationMs'], 'speed': 1.0, 'positionMs': self.pos(), 'repeatMode': self.repeat,
                        'shuffle': self.shuffle, 'audioOutput': self.output})

    def delta_playback(self):
        self.send({'type': 'playbackChanged', 'isPlaying': self.playing, 'isBuffering': False, 'durationMs': self.tracks[self.index]['durationMs'],
                   'speed': 1.0, 'positionMs': self.pos()})

    def delta_track(self):
        t = self.tracks[self.index]
        self.send({'type': 'trackChanged', 'currentIndex': self.index, 'currentTrackId': t['id'], 'positionMs': 0, 'isPlaying': self.playing,
                   'durationMs': t['durationMs']})

    def delta_modes(self):
        self.send({'type': 'modesChanged', 'repeatMode': self.repeat, 'shuffle': self.shuffle})

    # ---- what the phone does after scanning the PS5's QR (contract §4.4) ----
    def scan(self, qr_json):
        qr = json.loads(qr_json)
        assert qr['type'] == 'nzik-pair' and qr['v'] == 1 and len(qr['requestId']) == 22, qr
        offer = json.dumps({'v': 1, 'requestId': qr['requestId'], 'code': self.code, 'serverIps': ['127.0.0.1'],
                            'serverPort': self.port, 'serverName': 'Mock phone'}).encode()
        req = urllib.request.Request(f'http://127.0.0.1:{qr["port"]}/nzik-pair/v1/offer', data=offer, method='POST',
                                     headers={'Content-Type': 'application/json'})
        return urllib.request.urlopen(req, timeout=10).status

    def user_play(self):  # the user presses play on the phone
        self.apply('player/play', {})


class MockKabuki:
    """Kabuki's provider broker on 127.0.0.1:38590: welcomes one provider, keeps its content."""

    def __init__(self, port=38590):
        self.content = {}  # tile -> merged data
        self.hello = None
        self.conn = None
        self.ready = threading.Event()
        self.lock = threading.Lock()
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(('127.0.0.1', port))
        self.s.listen(1)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            self.conn, _ = self.s.accept()
            buf = b''
            while True:
                try:
                    c = self.conn.recv(65536)
                except OSError:
                    break
                if not c:
                    break
                buf += c
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    m = json.loads(line)
                    if 'hello' in m:
                        self.hello = m['hello']
                        self.conn.sendall(b'{"v":1,"welcome":{"kabuki":"mock","versions":[1],"art_dir":"/tmp","templates":["media","list","plugin"]}}\n')
                        self.ready.set()
                    elif 'content' in m:
                        with self.lock:
                            tile = self.content.setdefault(m['content']['tile'], {})
                            tile.update(m['content']['data'])

    def act(self, tile, action, key=None, value=None):
        a = {'tile': tile, 'action': action}
        if key is not None:
            a['key'] = key
        if value is not None:
            a['value'] = value
        self.conn.sendall((json.dumps({'v': 1, 'action': a}, separators=(',', ':')) + '\n').encode())

    def shown(self, tile):
        self.conn.sendall((json.dumps({'v': 1, 'shown': {'tile': tile}}, separators=(',', ':')) + '\n').encode())

    def tile(self, name):
        with self.lock:
            return dict(self.content.get(name, {}))
