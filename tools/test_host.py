"""End-to-end test of the host build: pairing by QR, following the phone, Kabuki controls.

    make test     (needs the ffmpeg CLI to make the test songs)
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from mock_nzik import MockKabuki, MockPhone  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def wait(what, check, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = check()
        except OSError:  # a file not written yet
            v = None
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError('timed out waiting for ' + what)


def main():
    tmp = tempfile.mkdtemp(prefix='nzik-test-')
    # Two songs in the formats YouTube Music serves: webm/opus and mp4/aac.
    songs = [('song1', 'webm', 'libopus', 440), ('song2', 'm4a', 'aac', 660)]
    tracks = []
    for tid, ext, codec, hz in songs:
        path = f'{tmp}/{tid}.{ext}'
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 'lavfi', '-i', f'sine=frequency={hz}:duration=8', '-c:a', codec, path], check=True)
        art = f'{tmp}/{tid}.jpg'
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 'lavfi', '-i', 'color=c=red:s=64x64', '-frames:v', '1', art], check=True)
        tracks.append({'id': tid, 'title': f'Cancion {tid[-1]}', 'artists': 'Artista', 'durationMs': 8000, 'path': path, 'art': art})

    # Silent until 10 s, then a tone: checks that seeks land where asked.
    edge = f'{tmp}/edge.webm'
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:duration=20', '-af',
                    "volume=enable='lt(t,10)':volume=0", '-c:a', 'libopus', edge], check=True)
    tracks.append({'id': 'edge', 'title': 'Borde', 'artists': 'Test', 'durationMs': 20000, 'path': edge, 'art': tracks[0]['art']})

    phone = MockPhone(tracks)
    r = subprocess.run([f'{ROOT}/build/test-decoder', f'http://127.0.0.1:{phone.port}/api/v1/audio/edge?t=audio-edge'],
                       capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr[-2000:])
    assert r.returncode == 0, 'decoder seek test failed'
    kabuki = MockKabuki()
    data = f'{tmp}/data'
    out = f'{tmp}/out.wav'
    log = []
    proc = subprocess.Popen([f'{ROOT}/build/n-zik-host', data, out], stderr=subprocess.PIPE, text=True)
    threading.Thread(target=lambda: [log.append(l) for l in proc.stderr], daemon=True).start()

    def logged(pattern):
        return next((m for l in list(log) for m in [re.search(pattern, l)] if m), None)

    try:
        kabuki.ready.wait(10)
        assert kabuki.hello and [t['id'] for t in kabuki.hello['tiles']] == ['NZKNOW', 'NZKQUE', 'NZKLIB', 'NZKLOC'], kabuki.hello

        # 1. Pairing: the phone "scans" the QR the PS5 shows.
        qr = wait('QR', lambda: logged(r'PAIR qr (\{.*\})'))
        assert os.path.getsize(f'{data}/art/pair-qr.png') > 100
        # A LAN peer that connects and stalls must not block the real phone for long.
        import socket
        slow = socket.create_connection(('127.0.0.1', json.loads(qr.group(1))['port']))
        slow.sendall(b'POST /nzik-pair/v1/offer HTTP/1.1\r\n')
        # And garbage JSON must be refused, not crash the payload.
        import urllib.request, urllib.error
        try:
            urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{json.loads(qr.group(1))["port"]}/nzik-pair/v1/offer',
                                                          data=b'{"requestId"}', method='POST'), timeout=10)
        except urllib.error.HTTPError as e:
            assert e.code in (404, 410), e.code
        assert proc.poll() is None, 'payload died on garbage'
        started = time.time()
        assert phone.scan(qr.group(1)) == 200
        assert time.time() - started < 6, 'stalled by a slow client'
        slow.close()
        wait('pairing file', lambda: os.path.exists(f'{data}/pairing.txt'))
        assert phone.token in open(f'{data}/pairing.txt').read()

        # 2. It connects and takes the audio output.
        phone.ws_connected.wait(10)
        wait('output pc', lambda: phone.output == 'pc')
        assert ('player/output', {'output': 'pc'}) in phone.commands

        # 3. The user plays on the phone: the PS5 decodes song1 (opus) and shows it.
        phone.user_play()
        wait('now playing', lambda: kabuki.tile('NZKNOW').get('title') == 'Cancion 1' and kabuki.tile('NZKNOW').get('playing'))
        wait('cover', lambda: (kabuki.tile('NZKNOW').get('art') or '').endswith('.jpg'))
        rows = kabuki.tile('NZKQUE')['top']['rows']
        assert [r['key'] for r in rows] == ['like', 'offline', 'settings', '0|song1', '1|song2', '2|edge'], rows
        wait('decoder opus', lambda: logged(r'DECODER opus'))
        time.sleep(2)

        # 4. Kabuki controls go to the phone: next (aac), pause, play, seek, queue jump.
        kabuki.act('NZKNOW', 'next')
        wait('song2', lambda: kabuki.tile('NZKNOW').get('title') == 'Cancion 2')
        wait('decoder aac', lambda: logged(r'DECODER aac'))
        time.sleep(1)
        kabuki.act('NZKNOW', 'play_pause')
        wait('paused', lambda: not phone.playing)
        wait('player paused', lambda: logged(r'PLAYER En pausa'))
        kabuki.act('NZKNOW', 'play_pause')
        wait('playing', lambda: phone.playing)
        kabuki.act('NZKNOW', 'seek', value=6000)
        wait('seek', lambda: ('player/seek', {'positionMs': 6000}) in phone.commands)
        wait('decoder follows the seek', lambda: logged(r'PLAYER drift .* seek to 6\d\d\d'))
        kabuki.act('NZKQUE', 'select', key='0|song1')
        wait('jump', lambda: phone.index == 0)
        wait('back to song1', lambda: kabuki.tile('NZKNOW').get('title') == 'Cancion 1')
        kabuki.act('NZKNOW', 'volume', value=40)
        wait('volume saved', lambda: 'volume 40' in open(f'{data}/config.txt').read())
        kabuki.act('NZKQUE', 'select', key='like')
        wait('like', lambda: ('library/songs/song1/like', {'state': 'liked'}) in phone.commands)
        wait('like shown', lambda: kabuki.tile('NZKQUE')['top']['rows'][0]['title'].startswith('Te gusta'))
        kabuki.act('NZKNOW', 'shuffle', value=1)
        wait('shuffle', lambda: phone.shuffle)
        time.sleep(1.5)

        # 5. Save offline: the whole stream, as served, under music/<artist>/N-Zik, with the cover.
        kabuki.act('NZKQUE', 'select', key='offline')
        saved = f'{data}/music/Artista/Album Uno/Cancion 1.webm'
        wait('offline file', lambda: os.path.exists(saved))
        assert open(saved, 'rb').read() == open(tracks[0]['path'], 'rb').read()
        wait('offline cover', lambda: os.path.exists(f'{data}/music/Artista/Album Uno/cover.jpg'))
        wait('offline shown', lambda: kabuki.tile('NZKQUE')['top']['rows'][1]['title'] == 'Guardada offline')

        # 6. Explorar: the phone's library; a playlist plays from the chosen song.
        kabuki.shown('NZKLIB')
        wait('library', lambda: any(r['key'] == 'p:7' for r in kabuki.tile('NZKLIB').get('top', {}).get('rows', [])))
        kabuki.act('NZKLIB', 'open', key='p:7')
        wait('playlist songs', lambda: len((kabuki.tile('NZKLIB').get('sub') or {}).get('rows', [])) == 5)  # mode + Aleatorio + 3
        kabuki.act('NZKLIB', 'select', key='1')
        wait('queue/list', lambda: phone.index == 1 and any(c[0] == 'queue/list' for c in phone.commands))
        kabuki.act('NZKLIB', 'close')
        # The first row cycles the view; the Songs view plays a song right from the top level.
        assert kabuki.tile('NZKLIB')['top']['rows'][0]['key'] == 'view'
        for view in ('Playlists', 'Albumes', 'Artistas'):
            kabuki.act('NZKLIB', 'select', key='view')
            wait(view, lambda: kabuki.tile('NZKLIB')['top']['rows'][0]['title'] == 'Ver: ' + view)
        assert [r['key'] for r in kabuki.tile('NZKLIB')['top']['rows']] == ['view']  # the mock has no artists
        kabuki.act('NZKLIB', 'select', key='view')
        wait('songs view', lambda: [r['key'] for r in kabuki.tile('NZKLIB')['top']['rows']] == ['view', 'addmode', '0', '1', '2'])
        kabuki.act('NZKLIB', 'select', key='2')
        wait('song from the Songs view', lambda: phone.index == 2 and ('queue/list', {'list': {'kind': 'songs', 'filter': 'all'}, 'action': 'play',
                                                                                     'startIndex': 2, 'startTrackId': 'edge'}) in phone.commands)
        # The mode row: Cross on a song can also queue just that song on the phone.
        kabuki.act('NZKLIB', 'select', key='addmode')
        wait('mode next', lambda: kabuki.tile('NZKLIB')['top']['rows'][1]['title'] == '✕: Reproducir siguiente')
        kabuki.act('NZKLIB', 'select', key='0')
        wait('queue/add next', lambda: ('queue/add', {'trackIds': ['song1'], 'position': 'next'}) in phone.commands)
        kabuki.act('NZKLIB', 'select', key='addmode')
        kabuki.act('NZKLIB', 'select', key='1')
        wait('queue/add end', lambda: ('queue/add', {'trackIds': ['song2'], 'position': 'end'}) in phone.commands)
        kabuki.act('NZKLIB', 'select', key='addmode')  # back to Play
        wait('mode play', lambda: kabuki.tile('NZKLIB')['top']['rows'][1]['title'] == '✕: Reproducir')

        # 7. Musica: the saved song plays from the console; the phone pauses, then takes over again.
        kabuki.shown('NZKLOC')
        wait('local albums', lambda: [r['title'] for r in kabuki.tile('NZKLOC')['top']['rows']] == ['Album Uno'])
        assert kabuki.tile('NZKLOC')['top']['rows'][0]['art'].endswith('/music/Artista/Album Uno/cover.jpg')
        kabuki.act('NZKLOC', 'open', key='0')
        wait('local songs', lambda: [r['title'] for r in (kabuki.tile('NZKLOC').get('sub') or {}).get('rows', [])] == ['✕: Reproducir', 'Cancion 1'])
        kabuki.act('NZKLOC', 'select', key='0')
        wait('phone paused', lambda: not phone.playing)
        wait('local playing', lambda: kabuki.tile('NZKNOW').get('note', '').startswith('En la consola') and kabuki.tile('NZKNOW').get('playing'))
        wait('local decoder', lambda: logged(r'DECODER opus .*bytes') and logged(r'PLAYER Sonando Cancion 1'))
        kabuki.act('NZKNOW', 'play_pause')
        wait('local paused', lambda: not kabuki.tile('NZKNOW').get('playing'))
        kabuki.act('NZKNOW', 'play_pause')
        # The console's queue: "play next" adds the song after the current one; Music shows the
        # queue on top and jumps within it.
        kabuki.act('NZKLOC', 'select', key='addmode')
        kabuki.act('NZKLOC', 'select', key='0')
        wait('console queue', lambda: kabuki.tile('NZKLOC')['top']['rows'][0]['key'] == 'queue' and
             kabuki.tile('NZKLOC')['top']['rows'][0]['subtitle'].startswith('2 canciones'))
        kabuki.act('NZKLOC', 'select', key='addmode')
        kabuki.act('NZKLOC', 'select', key='addmode')  # back to Play
        kabuki.act('NZKLOC', 'close')
        kabuki.act('NZKLOC', 'open', key='queue')
        wait('queue rows', lambda: [r['title'] for r in (kabuki.tile('NZKLOC').get('sub') or {}).get('rows', [])] == ['Cancion 1', 'Cancion 1'])
        kabuki.act('NZKLOC', 'select', key='1')
        wait('jumped', lambda: (kabuki.tile('NZKLOC').get('sub') or {}).get('rows', [{}, {}])[1].get('subtitle', '').startswith('Sonando'))
        kabuki.act('NZKLOC', 'close')
        time.sleep(1.5)
        # A song deleted by FTP leaves the console's queue when Music is shown again.
        extra = f'{data}/music/Artista/Album Uno/Extra.webm'
        import shutil
        shutil.copy(saved, extra)
        kabuki.act('NZKLOC', 'open', key='0')
        wait('album with extra', lambda: len((kabuki.tile('NZKLOC').get('sub') or {}).get('rows', [])) == 3)
        kabuki.act('NZKLOC', 'select', key='addmode')
        kabuki.act('NZKLOC', 'select', key='addmode')  # Add to the end
        kabuki.act('NZKLOC', 'select', key='1')       # Extra (song keys are song indexes)
        wait('three queued', lambda: kabuki.tile('NZKLOC')['top']['rows'][0]['subtitle'].startswith('3 canciones'))
        kabuki.act('NZKLOC', 'select', key='addmode')  # back to Play
        kabuki.act('NZKLOC', 'close')
        os.remove(extra)
        kabuki.shown('NZKLOC')
        wait('pruned', lambda: kabuki.tile('NZKLOC')['top']['rows'][0]['subtitle'].startswith('2 canciones'))
        phone.user_play()
        wait('phone again', lambda: not kabuki.tile('NZKNOW').get('note', '').startswith('En la consola'))

        # 8. A second copy (the Payload Manager launched twice) makes the first one step aside.
        second = subprocess.Popen([f'{ROOT}/build/n-zik-host', data, f'{tmp}/out2.wav'], stderr=subprocess.DEVNULL)
        proc.wait(10)
        assert logged(r'STOP'), 'the first copy did not step aside'
        time.sleep(1)
        assert second.poll() is None, 'the second copy did not take over'

        # 9. Stop: the payload hands the audio back to the phone.
        open(f'{data}/stop', 'w').close()
        second.wait(10)
        wait('output phone', lambda: phone.output == 'phone')
    finally:
        if proc.poll() is None:
            proc.kill()
        sys.stderr.write(''.join(log[-40:]))

    # The WAV holds real audio: about the time we played, and not silence.
    raw = open(out, 'rb').read()[44:]
    seconds = len(raw) / 4 / 48000
    samples = struct.unpack(f'<{len(raw) // 2}h', raw[:len(raw) // 2 * 2])
    peak = max(abs(s) for s in samples)
    print(f'audio: {seconds:.1f} s, peak {peak}')
    assert seconds > 4, seconds
    assert peak > 1000, peak
    print('OK')


if __name__ == '__main__':
    main()
