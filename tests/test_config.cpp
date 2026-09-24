#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int main() {
    using tilewm::Config;

    // Compiled-in defaults mirror the original hardcoded behavior.
    {
        Config c = tilewm::default_config();
        CHECK(c.gaps == 0);
        CHECK(c.mfact > 0.54f && c.mfact < 0.56f);
        CHECK(c.nmaster == 1);
        CHECK(c.workspaces == 4);
        CHECK(!c.keys.empty());
        bool has_quit = false, has_ws = false;
        for (const auto &k : c.keys) {
            has_quit = has_quit || k.action == "quit";
            has_ws = has_ws || k.action == "workspace";
        }
        CHECK(has_quit && has_ws);
    }

    // Modifier parsing.
    {
        bool ok = false;
        CHECK(tilewm::parse_mods("Alt+Shift", ok) ==
              (tilewm::MOD_ALT | tilewm::MOD_SHIFT));
        CHECK(ok);
        CHECK(tilewm::parse_mods("ctrl+alt", ok) ==
              (tilewm::MOD_CTRL | tilewm::MOD_ALT));
        CHECK(ok);
        CHECK(tilewm::parse_mods("Super", ok) == tilewm::MOD_SUPER);
        CHECK(ok);
        tilewm::parse_mods("Alt+Frobnicator", ok);
        CHECK(!ok);
    }

    // Actions.
    CHECK(tilewm::known_action("reload-config"));
    CHECK(!tilewm::known_action("make-coffee"));

    // Missing file is an error, config untouched.
    {
        Config c = tilewm::default_config();
        std::string err;
        CHECK(!tilewm::load_config_file("/nonexistent-tilewm-init.lua", c, err));
        CHECK(!err.empty());
        CHECK(c.gaps == 0 && c.keys.size() == tilewm::default_config().keys.size());
    }

    // A real file overrides values and replaces binds.
    {
        const char *path = "/tmp/tilewm-test-init.lua";
        {
            std::ofstream f(path);
            f << "config = { gaps = 12, mfact = 0.7, nmaster = 2, workspaces = 2 }\n"
                 "bind(\"Alt\", \"x\", \"close\")\n";
        }
        Config c = tilewm::default_config();
        std::string err;
        CHECK(tilewm::load_config_file(path, c, err));
        CHECK(c.gaps == 12);
        CHECK(c.mfact > 0.69f && c.mfact < 0.71f);
        CHECK(c.nmaster == 2);
        CHECK(c.workspaces == 2);
        CHECK(c.keys.size() == 1 && c.keys[0].action == "close");
        std::remove(path);
    }

    // A file with no binds keeps the previous keymap.
    {
        const char *path = "/tmp/tilewm-test-init2.lua";
        {
            std::ofstream f(path);
            f << "config = { gaps = 3 }\n";
        }
        Config c = tilewm::default_config();
        const std::size_t nkeys = c.keys.size();
        std::string err;
        CHECK(tilewm::load_config_file(path, c, err));
        CHECK(c.gaps == 3 && c.keys.size() == nkeys);
        std::remove(path);
    }

    // Broken Lua and bad binds are errors.
    {
        const char *path = "/tmp/tilewm-test-init3.lua";
        {
            std::ofstream f(path);
            f << "bind(\"Alt\", \"not-a-real-key-xyz\", \"close\")\n";
        }
        Config c = tilewm::default_config();
        std::string err;
        CHECK(!tilewm::load_config_file(path, c, err));
        CHECK(!err.empty());
        std::remove(path);
    }

    if (failures == 0) {
        std::puts("test_config: all checks passed");
        return 0;
    }
    std::printf("test_config: %d check(s) failed\n", failures);
    return 1;
}
