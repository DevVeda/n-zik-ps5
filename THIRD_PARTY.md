# Third-party software

N-Zik PS5 is licensed under **GPL-3.0-or-later** (`LICENSE`). The ELF links or embeds:

| Component | License | Use |
|---|---|---|
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | GPL-3.0-or-later (BSD for `include/freebsd`) | Runtime and system stubs |
| [FFmpeg](https://ffmpeg.org) 7.1.1 (libavformat, libavcodec, libswresample, libavutil) | [LGPL-2.1-or-later](https://ffmpeg.org/legal.html) | Audio demuxing, decoding and resampling. Built without GPL parts by `tools/build-ffmpeg.sh`; linked statically. |
| [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) (`third_party/qrcodegen.*`) | MIT (`third_party/qrcodegen.LICENSE`) | Pairing QR |
| [jsmn](https://github.com/zserge/jsmn) (`third_party/jsmn.h`) | MIT (`third_party/jsmn.LICENSE`) | JSON parsing |
| [Kabuki](https://github.com/stupiditytries/kabuki-ps5) provider header (`third_party/kabuki.h`) | GPL-3.0-or-later | Overlay tiles |
| ShellUI loader (`third_party/shellui-loader/`), from [shsrv](https://github.com/ps5-payload-dev/shsrv) and [ps5-dualsense-overlay](https://github.com/erickdavestech/ps5-dualsense-overlay), as used by [librespot-ps5](https://github.com/stupiditytries/librespot-ps5) | GPL-3.0-or-later (`third_party/shellui-loader/COPYING`) | Runs the audio worker inside SceShellUI. Sources and pinned commits: `third_party/shellui-loader/SOURCES.md` |

The full license texts ship with those sources at the links above.

N-Zik PS5 talks to N-Zik's PC bridge over the network; it contains no N-Zik code. It is not affiliated with N-Zik, Sony Interactive Entertainment or YouTube. PlayStation and PS5 are trademarks of their owners.
