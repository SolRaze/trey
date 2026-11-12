#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "arm.hpp"
#include "plgldr.h"
#include "plugin_bin.h"

static const char* plugin_path = "/3ds/relay/relay.3gx";

static bool mkdirs(const char* file) {
    char path[256];
    snprintf(path, sizeof(path), "%s", file);
    for (char* p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return true;
}

// Written on every arm: each build changes the plugin and its version field does not.
static const char* writePlugin() {
    if (!mkdirs(plugin_path)) return "Cannot create /3ds/relay";
    FILE* f = fopen(plugin_path, "w");
    if (!f) return "Cannot open the plugin file";
    size_t written = fwrite(plugin_bin, 1, plugin_bin_size, f);
    if (fclose(f) != 0 || written != plugin_bin_size) return "Cannot write the plugin file";
    return nullptr;
}

static const char* loadPlugin() {
    // Luma keeps persistent plugin parameters under /luma/plugins.
    mkdirs("/luma/plugins/.");

    PluginLoadParameters param = {};
    param.noFlash = false;
    param.pluginMemoryStrategy = PLG_STRATEGY_NONE;
    param.persistent = 1;
    param.lowTitleId = 0;
    strcpy(param.path, plugin_path);

    if (R_FAILED(plgLdrInit())) return "Plugin loader not running";
    const char* err = nullptr;
    u32 version = 0;
    bool enabled = false;
    if (R_FAILED(PLGLDR__GetVersion(&version))) err = "Plugin loader error";
    else if (version < SYSTEM_VERSION(1, 0, 2)) err = "Luma3DS too old for plugins";
    else if (R_FAILED(PLGLDR__IsPluginLoaderEnabled(&enabled))) err = "Plugin loader error";
    else {
        // The plugin restores this state when it starts.
        param.config[0] = enabled;
        if (R_FAILED(PLGLDR__SetPluginLoaderState(true))) err = "Cannot enable the plugin loader";
        else if (R_FAILED(PLGLDR__SetPluginLoadParameters(&param))) err = "Plugin loader error";
    }
    plgLdrExit();
    return err;
}

const char* armBroadcast() {
    const char* err = writePlugin();
    return err ? err : loadPlugin();
}
