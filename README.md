# N-Zik PS5

**A companion app for [N-Zik](https://github.com/N-Zik-Group/N-Zik): listen to your N-Zik music on a jailbroken PS5 while you play.**

Your phone stays in charge (queue, library, search); the PS5 becomes its speaker. You control it from the phone or from the [Kabuki](https://github.com/stupiditytries/kabuki-ps5) overlay (L3 + R3), on top of any game. It also plays music saved on the console.

<p align="center"><a href="https://ko-fi.com/DevVeda"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a></p>

*[Español más abajo](#español).*

## Highlights

- **Your phone's music, on the console.** N-Zik plays silently on the phone and the PS5 plays the same song, in sync. No changes to the phone app.
- **Over any game.** Four Kabuki tiles:
  - **Now Playing**: cover, seek, play/pause, skip, shuffle, repeat and its own volume.
  - **N-Zik**: like, save offline, settings and the phone's queue.
  - **Explore**: the phone's library (recommended, liked, most played, playlists, albums, artists, songs).
  - **Music**: songs saved on the console, and the console's queue.
- **Play, play next or add to the end.** A row above every song list picks what ✕ does.
- **Save offline.** Keeps the song that is playing, as served (opus/aac), in `/data/music/<artist>/<album>/`.
- **Pair once.** Scan a QR with the phone. If the phone gets a new IP, N-Zik PS5 finds it on the network.
- **Spanish or English**, following the console language.

## Install

Needs a jailbroken PS5, [Payload Manager](https://github.com/itsPLK/ps5-payload-manager), [Kabuki](https://github.com/stupiditytries/kabuki-ps5) for the overlay, and [N-Zik](https://github.com/N-Zik-Group/N-Zik) on your phone (Android).

**From Payload Manager (recommended, with updates):**
1. Go to **Settings → Manage Sources → Add Source**.
2. Add `https://github.com/DevVeda/n-zik-ps5/releases/latest/download/repo.json`.
3. Install **N-Zik PS5** from that source.

**By hand:**
1. Download `n-zik-ps5.elf` from the [latest release](../../releases/latest).
2. Copy it over FTP to `/data/pldmgr/payloads/local/n-zik-ps5/n-zik-ps5.elf`.

**Then:**
1. Launch **Kabuki**, then **N-Zik PS5**, from Payload Manager.
2. Press L3 + R3: **Now Playing** shows a QR.
3. On the phone: N-Zik → PC bridge → turn on the server → Scan QR.
4. Play something on the phone: it plays on the PS5.

To stop: N-Zik → Settings → **Quit N-Zik**. The sound goes back to the phone. Launching N-Zik PS5 again replaces the running copy. Its files are in `/data/n-zik-ps5/`.

## Build

Needs:
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (in `../ps5-payload-sdk`, or `SDK=...`)
- LLVM 18
- FFmpeg 7.1 sources, for the console build; FFmpeg libraries and the `ffmpeg` command, for the host tests
- Python 3

Works on macOS and Linux.

```sh
make test                                     # host build + simulated N-Zik and Kabuki
FFMPEG_SRC=<ffmpeg 7.1> bash tools/build-ffmpeg.sh   # small static FFmpeg for the PS5 (once)
make ps5                                      # build/n-zik-ps5.elf
make push PS5_HOST=<ps5-ip>                   # upload to Payload Manager over FTP (or put PS5_HOST in local.mk)
make log PS5_HOST=<ps5-ip>
```

## Acknowledgements

- [N-Zik](https://github.com/N-Zik-Group/N-Zik), for the app and its PC bridge, which this talks to. N-Zik PS5 is not affiliated with N-Zik.
- [stupiditytries](https://github.com/stupiditytries), for [Kabuki](https://github.com/stupiditytries/kabuki-ps5) and its provider protocol, and for [librespot-ps5](https://github.com/stupiditytries/librespot-ps5), which showed how to play audio over games from SceShellUI.
- [ps5-payload-dev](https://github.com/ps5-payload-dev) and John Törnblom, for the PS5 payload SDK and [shsrv](https://github.com/ps5-payload-dev/shsrv).
- [erickdavestech](https://github.com/erickdavestech), for [ps5-dualsense-overlay](https://github.com/erickdavestech/ps5-dualsense-overlay), whose injector we reuse.
- [itsPLK](https://github.com/itsPLK), for [Payload Manager](https://github.com/itsPLK/ps5-payload-manager).
- [FFmpeg](https://ffmpeg.org), [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki, and [jsmn](https://github.com/zserge/jsmn) by Serge Zaitsev.

## AI disclosure

AI was used to create this software. Use it at your own discretion.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Bundled software is listed in [THIRD_PARTY.md](THIRD_PARTY.md).

---

## Español

**Una app complementaria para [N-Zik](https://github.com/N-Zik-Group/N-Zik): escucha tu música de N-Zik en un PS5 con jailbreak mientras juegas.**

El teléfono sigue mandando (cola, biblioteca, búsqueda); el PS5 hace de altavoz. Lo controlas desde el teléfono o desde el overlay de [Kabuki](https://github.com/stupiditytries/kabuki-ps5) (L3 + R3), encima de cualquier juego. También reproduce música guardada en la consola.

### Lo principal

- **La música del teléfono, en la consola.** N-Zik suena en silencio en el teléfono y el PS5 reproduce la misma canción, sincronizada. Sin cambios en la app del teléfono.
- **Encima de cualquier juego.** Cuatro tiles de Kabuki:
  - **Sonando**: carátula, barra, play/pausa, saltar, aleatorio, repetir y su propio volumen.
  - **N-Zik**: me gusta, guardar offline, ajustes y la cola del teléfono.
  - **Explorar**: la biblioteca del teléfono (recomendadas, me gusta, lo más escuchado, playlists, álbumes, artistas, canciones).
  - **Música**: las canciones guardadas en la consola y la cola de la consola.
- **Reproducir, reproducir siguiente o agregar al final.** Una fila encima de cada lista de canciones elige qué hace ✕.
- **Guardar offline.** Guarda la canción que suena tal cual (opus/aac) en `/data/music/<artista>/<álbum>/`.
- **Se empareja una vez.** Escaneas un QR con el teléfono. Si el teléfono cambia de IP, N-Zik PS5 lo busca en la red.
- **En español o inglés**, según el idioma de la consola.

### Instalar

Necesitas un PS5 con jailbreak, [Payload Manager](https://github.com/itsPLK/ps5-payload-manager), [Kabuki](https://github.com/stupiditytries/kabuki-ps5) para el overlay y [N-Zik](https://github.com/N-Zik-Group/N-Zik) en el teléfono (Android).

**Desde Payload Manager (recomendado, con actualizaciones):**
1. Ve a **Settings → Manage Sources → Add Source**.
2. Añade `https://github.com/DevVeda/n-zik-ps5/releases/latest/download/repo.json`.
3. Instala **N-Zik PS5** desde esa fuente.

**A mano:**
1. Descarga `n-zik-ps5.elf` de la [última versión](../../releases/latest).
2. Cópialo por FTP a `/data/pldmgr/payloads/local/n-zik-ps5/n-zik-ps5.elf`.

**Después:**
1. Lanza **Kabuki** y luego **N-Zik PS5** desde Payload Manager.
2. Pulsa L3 + R3: **Sonando** muestra un QR.
3. En el teléfono: N-Zik → Puente PC → activar el servidor → Escanear QR.
4. Pon música en el teléfono: suena en el PS5.

Para parar: N-Zik → Ajustes → **Cerrar N-Zik**. El sonido vuelve al teléfono. Lanzar N-Zik PS5 otra vez reemplaza la copia que corre. Sus archivos están en `/data/n-zik-ps5/`.

### Compilar

Ver [Build](#build).

### Agradecimientos

Ver [Acknowledgements](#acknowledgements). N-Zik PS5 no está afiliado a N-Zik.

### Uso de IA

Se usó IA para crear este software. Úsalo bajo tu propio criterio.

### Licencia

GPL-3.0-or-later. Ver [LICENSE](LICENSE). El software incluido está en [THIRD_PARTY.md](THIRD_PARTY.md).
