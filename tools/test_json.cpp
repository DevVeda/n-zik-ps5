// JSON reader self-check, run under ASAN by `make test`: hostile input must never read out of bounds.
#include "json.hpp"
#include <cassert>

int main() {
    const char* bad[] = {"{\"requestId\"}", "{\"a\",\"b\"}", "[1,", "{\"a\":}", "", "{\"a\":[{\"b\"}]}", "\"x\"", "{\"k\":\"\\ud800\\u0041\"}"};
    for (auto* b : bad) {
        json::Doc d;
        if (d.parse(b)) {
            d.str(d.get(0, "requestId"));
            d.items(d.get(0, "a"));
            d.str(d.get(0, "k"));
        }
    }
    json::Doc d;
    assert(d.parse("{\"type\":\"snapshot\",\"q\":[{\"id\":\"a\"},{\"id\":\"b\\u00e9\"}],\"n\":-5,\"t\":true,\"z\":null}"));
    assert(d.str(d.get(0, "type")) == "snapshot");
    auto it = d.items(d.get(0, "q"));
    assert(it.size() == 2 && d.str(d.get(it[1], "id")) == "b\xc3\xa9");
    assert(d.num(d.get(0, "n")) == -5 && d.boolean(d.get(0, "t")) && d.is_null(d.get(0, "z")) && d.get(0, "missing") == -1);
    std::puts("json ok");
}
