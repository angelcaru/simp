#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#include "raylib.h"

#include "app.h"

#define X(name, ...) name##_t name;
APP_FUNCS
#undef X

#ifdef HOTRELOAD
#include <dlfcn.h>
void *libapp = NULL;
#endif // HOTRELOAD

bool load_libapp(void) {
#ifdef HOTRELOAD

    if (libapp != NULL) dlclose(libapp);

    libapp = dlopen("./build/libapp.so", RTLD_NOW);
    if (libapp == NULL) {
        nob_log(ERROR, "Could not load libapp: %s", dlerror());
        return false;
    }

    #define X(name, ...)  \
        name = dlsym(libapp, #name); \
        if (name == NULL) { \
            nob_log(ERROR, "Could not load symbol %s: %s", #name, dlerror()); \
            return false; \
        }
    APP_FUNCS
    #undef X

#endif // HOTRELOAD
    return true;
}

#ifdef HOTRELOAD
void reload_libapp(void) {
    App *g = app_pre_reload();
    load_libapp();
    app_post_reload(g);
}

bool should_reload_libapp = false;

void set_libapp_to_be_reloaded(int unused_arg_for_sigaction) {
    UNUSED(unused_arg_for_sigaction);
    should_reload_libapp = true;
}
#endif // HOTRELOAD

int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(640, 480, "Simple Image Manipulation Program");

    if (!load_libapp()) return 1;

#ifdef HOTRELOAD
    sigaction(SIGHUP, &(struct sigaction) { .sa_handler = set_libapp_to_be_reloaded }, NULL);
#endif // HOTRELOAD

    app_init();

    while (!WindowShouldClose()) {
#ifdef HOTRELOAD
        if (should_reload_libapp || IsKeyPressed(KEY_F5)) {
            reload_libapp();
            should_reload_libapp = false;
        }
#endif // HOTRELOAD
        app_update();
    }

    CloseWindow();
    return 0;
}
