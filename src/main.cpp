// SPDX-License-Identifier: GPL-3.0-or-later
// n-zik-ps5: plays your phone's N-Zik queue on the PS5, over games, controlled from the
// phone or from Kabuki (L3+R3). Load Kabuki first, then this payload.
// Host build (macOS/Linux, for testing): n-zik-host <data dir> <out.wav>
#include "i18n.hpp"
#include "app.hpp"
#include "log.hpp"
#include "net.hpp"
#include "notify.hpp"
#include "pairing.hpp"
#include "ui/kabuki_ui.hpp"
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include "audio/shell_sink.hpp"
#else
#include "audio/sink.hpp"
#endif

using Body = void (*)(App*);
static void spawn(Body body, App* app) {
    struct Start { Body body; App* app; };
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);
    pthread_create(&t, &a, [](void* p) -> void* {
        auto* s = static_cast<Start*>(p);
        s->body(s->app);
        delete s;
        return nullptr;
    }, new Start{body, app});
    pthread_detach(t);
}

struct BridgeThread {
    static void body(App* app) {
        while (!app->quit) {
            if (app->bridge.paired()) app->bridge.run(); // returns when unpaired
            else sleep(1);
        }
    }
};
struct PlayerThread {
    static void body(App* app) { app->player.run(); }
};
struct PairingThread {
    // Shows a QR while unpaired, when asked from Settings, or when the paired phone can't be found
    // (that one goes away by itself if the phone turns up again).
    static void body(App* app) {
        while (!app->quit) {
            if (app->bridge.paired() && !app->pair_requested && !app->bridge.lost) { sleep(1); continue; }
            const bool asked = app->pair_requested.exchange(false);
            const std::string ip = net::local_ip();
            if (ip.empty()) { app->set_pairing_screen(TR("No network: connect the PS5 to Wi-Fi or cable", "Sin red: conecta el PS5 al Wi-Fi o cable"), ""); sleep(3); continue; }
            const std::string id = pairing::new_request_id();
            std::string text, status;
            net::Listener listener;
            const int port = pairing::open_listener(listener);
            if (!port) { app->set_pairing_screen(TR("Could not open the pairing port", "No pude abrir el puerto de emparejamiento"), ""); sleep(5); continue; }
            if (id.empty() || !pairing::make_qr(id, app->config.device_name, ip, port, paths::qr.c_str(), text)) {
                app->set_pairing_screen(TR("Could not make the QR", "No pude crear el QR"), "");
                sleep(5);
                continue;
            }
            logf("PAIR qr %s", text.c_str());
            app->set_pairing_screen(TR("Scan with N-Zik (", "Escanea con N-Zik (") + ip + ")", paths::qr);
            Pairing p;
            const auto cancel = [&] { return app->quit || app->pair_requested || (!asked && app->bridge.paired() && !app->bridge.lost); };
            if (pairing::wait_for_phone(listener, id, app->config.device_name, 180000, p, status, cancel)) {
                app->bridge.set_pairing(p);
                notify("N-Zik: %s", status.c_str());
                app->set_pairing_screen(status, "");
            } else if (!asked && app->bridge.paired() && !app->bridge.lost) {
                app->set_pairing_screen("", ""); // the phone turned up again
            } else {
                logf("PAIR no pairing: %s", status.empty() ? "timeout" : status.c_str());
                app->set_pairing_screen(status.empty() ? TR("QR expired: make another", "QR caducado: genera otro") : status, "");
                if (!app->bridge.paired()) sleep(2); // unpaired: a fresh QR right away
            }
        }
    }
};

constexpr int single_port = 38580; // loopback only; held while N-Zik runs

int main(int argc, char** argv) {
#ifdef __PROSPERO__
    (void)argc;
    (void)argv;
    // Same folder as /data/n-zik-ps5, but ShellUI (which draws Kabuki's pictures) has no /data.
    paths::set_root("/user/data/n-zik-ps5");
#else
    if (argc < 3) { std::fprintf(stderr, "usage: %s <data dir> <out.wav>\n", argv[0]); return 2; }
    paths::set_root(argv[1]);
    local::root = paths::root + "/music";
#endif
    mkdir(paths::root.c_str(), 0777);
    mkdir(paths::art.c_str(), 0777);
#ifdef __PROSPERO__
    log_file = paths::log.c_str();
#endif
    i18n::init();
    // One N-Zik at a time: a second copy fights the first over the phone and the ShellUI worker.
    // The copy holding this loopback port is the running one; a new copy asks it to stop (the stop
    // file) and takes over once it has gone.
    net::Listener single;
    for (int i = 0; !single.listen_on(single_port, true); ++i) {
        if (i == 0) {
            if (FILE* f = std::fopen(paths::stop.c_str(), "w")) std::fclose(f);
            logf("START another N-Zik is running: asking it to stop");
        }
        if (i == 40) {
            notify("%s", TR("N-Zik is already running", "N-Zik ya esta abierto"));
            return 1;
        }
        usleep(250000);
    }
    std::remove(paths::stop.c_str());
    logf("START n-zik-ps5");
    logf("LANG %d -> %s", i18n::lang, i18n::spanish ? "es" : "en");

    Config config;
    config.load(paths::config.c_str());
    Bridge bridge(paths::pairing.c_str());
#ifdef __PROSPERO__
    shell::Host host;
    std::string info;
    if (!host.start(info)) {
        notify(TR("N-Zik: cannot use the system audio (%s)", "N-Zik: no puedo usar el audio del sistema (%s)"), info.c_str());
        return 1;
    }
    // Default B (librespot-ps5's); A and C also play in games. Selectable in N-Zik > Settings.
    audio::ShellSink sink(host, config.ownership ? config.ownership : shell::Source);
#else
    audio::FileSink sink(argv[2], true);
    std::string info = "host build";
#endif
    local::Queue local_queue;
    Player player(bridge, local_queue, sink);
    player.volume = config.volume;
    player.low_quality = config.low_quality;
    player.song_toasts = config.song_toasts;
    App app(bridge, player, local_queue);
    app.config = config;
    app.audio_info = info;
#ifdef __PROSPERO__
    app.on_ownership = [&sink](int o) { sink.set_ownership(o ? o : shell::Source); };
#endif

    spawn(BridgeThread::body, &app);
    spawn(PlayerThread::body, &app);
    spawn(PairingThread::body, &app);
    notify(bridge.paired() ? TR("N-Zik ready: open N-Zik on your phone", "N-Zik listo: abre N-Zik en el telefono") : TR("N-Zik: pair your phone from Sonando/Now Playing (L3+R3)", "N-Zik: empareja tu telefono en el tile N-Zik (L3+R3)"));
    spawn([](App* a) { KabukiUi(*a).run(); }, &app);
    while (!app.quit && access(paths::stop.c_str(), F_OK)) sleep(1);
    app.quit = true;
    logf("STOP");
    bridge.stop(); // no more "pc" requests from the bridge thread
    if (bridge.paired()) bridge.command("player/output", "{\"output\":\"phone\"}"); // give the sound back to the phone
#ifdef __PROSPERO__
    host.command(shell::Stop, shell::Direct);
    sleep(2); // the worker stops and restores what it changed
#endif
    // Other threads may be mid-call: leave without running destructors under them.
    _exit(0);
}
