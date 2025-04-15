#ifndef GAME_H_
#define GAME_H_

typedef struct App App;

#define APP_FUNCS \
    X(app_init, void, void) \
    X(app_pre_reload, App*, void) \
    X(app_post_reload, void, App*) \
    X(app_update, void, void)

#ifdef HOTRELOAD
    #define X(name, ret, ...) typedef ret (*name##_t)(__VA_ARGS__);
#else
    #define X(name, ret, ...) typedef ret name##_t(__VA_ARGS__);
#endif // HOTRELOAD
APP_FUNCS
#undef X

#endif // GAME_H_
