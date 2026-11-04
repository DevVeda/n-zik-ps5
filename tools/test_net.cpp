// net::sweep finds a listener on its /24 (here 127.0.0.x, from a pretend address .5).
#include "net.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
    net::Listener l;
    int port = 47000;
    while (!l.listen_on(port, true)) ++port;
    const auto found = net::sweep("127.0.0.5", port, 500);
    assert(std::find(found.begin(), found.end(), "127.0.0.1") != found.end());
    net::sweep("127.0.0.5", port + 1000, 300); // nothing listening there: returns on time
    std::puts("sweep ok");
}
