// Seek precision check for audio::Decoder. The file at URL must be silent before 10 s and a
// tone after. Run by tools/test_host.py against the mock phone.
#include "audio/decoder.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static double rms(audio::Decoder& d, int ms) {
    static std::int16_t buf[48000 * 2];
    const int frames = audio::rate * ms / 1000;
    int got = 0;
    while (got < frames) {
        const int n = d.read(buf + 2 * got, frames - got);
        if (n <= 0) break;
        got += n;
    }
    double sum = 0;
    for (int i = 0; i < got * 2; ++i) sum += double(buf[i]) * buf[i];
    return got ? std::sqrt(sum / (got * 2)) : 0;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    audio::Decoder d;
    assert(d.open(argv[1], 0));
    assert(d.seek(9500));
    const double before = rms(d, 400); // 9.5 .. 9.9 s: silence
    assert(d.position_ms() >= 9890 && d.position_ms() <= 9910);
    rms(d, 200);                        // 9.9 .. 10.1: the edge
    const double after = rms(d, 400);   // 10.1 .. 10.5: tone
    assert(d.seek(10200));
    const double landed = rms(d, 100);  // must be tone right away
    assert(d.open(argv[1], 12000));     // joining mid-song
    const double joined = rms(d, 100);
    std::printf("seek: before %.0f after %.0f landed %.0f joined %.0f\n", before, after, landed, joined);
    assert(before < 50 && after > 1000 && landed > 1000 && joined > 1000);
    std::puts("decoder ok");
}
