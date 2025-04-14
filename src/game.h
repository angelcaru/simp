#ifndef GAME_H_
#define GAME_H_

typedef struct Game Game;

#define GAME_FUNCS \
    X(game_init, void, void) \
    X(game_pre_reload, Game*, void) \
    X(game_post_reload, void, Game*) \
    X(game_update, void, void)

#ifdef HOTRELOAD
    #define X(name, ret, ...) typedef ret (*name##_t)(__VA_ARGS__);
#else
    #define X(name, ret, ...) typedef ret name##_t(__VA_ARGS__);
#endif // HOTRELOAD
GAME_FUNCS
#undef X

#endif // GAME_H_
