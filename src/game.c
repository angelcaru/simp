#include "game.h"

#include <unistd.h>

#ifdef HOTRELOAD
    #define NOB_IMPLEMENTATION
#endif // HOTRELOAD
#define NOB_STRIP_PREFIX
#include "nob.h"

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "clay_renderer_raylib.c"

#include "raylib.h"
#include "raymath.h"
#define MACRO_VAR(name) _##name##__LINE__
#define BEGIN_END_NAMED(begin, end, i) for (int i = (begin, 0); i < 1; i++, end)
#define BEGIN_END(begin, end) BEGIN_END_NAMED(begin, end, MACRO_VAR(i))
#define Drawing() BEGIN_END(BeginDrawing(), EndDrawing())
#define Mode2D(camera) BEGIN_END(BeginMode2D(camera), EndMode2D())
#define Mode3D(camera) BEGIN_END(BeginMode3D(camera), EndMode3D())

#define MOUSE_BUTTON_PAN MOUSE_BUTTON_RIGHT
#define MOUSE_BUTTON_MOVE_OBJECT MOUSE_BUTTON_LEFT

#define OBJECT_RESIZE_HITBOX_SIZE 30

typedef struct {
    Texture texture;
    Rectangle rec;
} Object;

typedef struct {
    Object *items;
    size_t count, capacity;
} Objects;

struct Game {
    size_t size;

    Clay_Context *clay;
    Font font;

    Objects objects;
    Camera2D camera;

    // Sometimes SetMouseCursor() is extremely slow (once it dropped from >3000 FPS to ~15), so we keep track of the previous
    // cursor ourselves to avoid calling it too much. Raylib already keeps track of this but it doesn't use it for some reason
    // Should probably submit a PR to raylib
    int prev_mouse_cursor;
};

Game *g;

void handle_clay_error(Clay_ErrorData error) {
    nob_log(ERROR, "Clay Error: %.*s", error.errorText.length, error.errorText.chars);
}

void game_init(void) {
    g = malloc(sizeof(*g));
    memset(g, 0, sizeof(*g));
    g->size = sizeof(*g);

    Texture texture = LoadTexture("C_Logo.png");
    Object object = {
        .texture = texture,
        .rec = { 0, 0, texture.width, texture.height },
    };
    da_append(&g->objects, object);

    g->camera.zoom = 1;
    g->camera.target = (Vector2) { (float)GetScreenWidth() / 2, (float)GetScreenHeight() / 2 };

    uint64_t total_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(total_memory_size, malloc(total_memory_size));

    g->font = LoadFont("./fonts/Alegreya-Regular.ttf");
    g->clay = Clay_Initialize(clay_arena, (Clay_Dimensions) { GetScreenWidth(), GetScreenHeight() }, (Clay_ErrorHandler) { handle_clay_error, 0 });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, &g->font);
}

Game* game_pre_reload(void) {
    return g;
}
void game_post_reload(Game *new_g) {
    g = new_g;

    if (g->size < sizeof(*g)) {
        nob_log(INFO, "Migrating struct Game (%zu bytes -> %zu bytes)", g->size, sizeof(*g));
        g = realloc(g, sizeof(*g));
        memset((char*)g + g->size, 0, sizeof(*g) - g->size);
        g->size = sizeof(*g);
    }

    // The addresses of the functions may shift after hot-reloading so
    // we must set them again
    // Clay also uses internally a Clay__currentContext global variable, whose address
    // (and value) also shifts after the reload. Hence keeping track of it ourselves
    Clay_SetCurrentContext(g->clay);
    Clay_SetMeasureTextFunction(Raylib_MeasureText, &g->font);
    g->clay->errorHandler = (Clay_ErrorHandler) { handle_clay_error, 0 };
}

#define DEBUG(fmt, value, x, y) DrawText(TextFormat("%s = "fmt, #value, value), x, y, 20, LIME)

void game_update(void) {
    Clay_SetLayoutDimensions((Clay_Dimensions) { GetScreenWidth(), GetScreenHeight() });
    Clay_SetPointerState((Clay_Vector2) { GetMouseX(), GetMouseY() }, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    Vector2 wheel_v = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(true, (Clay_Vector2) { wheel_v.x, wheel_v.y }, GetFrameTime());

    g->camera.offset = (Vector2) { (float)GetScreenWidth() / 2, (float)GetScreenHeight() / 2 };

    float wheel = GetMouseWheelMove();
    g->camera.zoom *= wheel / 20.0f + 1;

    Vector2 mouse_delta = Vector2Scale(GetMouseDelta(), 1/g->camera.zoom);
    if (IsMouseButtonDown(MOUSE_BUTTON_PAN)) {
        g->camera.target = Vector2Subtract(g->camera.target, mouse_delta);
    }

    float object_resize_hitbox_size = OBJECT_RESIZE_HITBOX_SIZE / g->camera.zoom;

    bool is_move_down = IsMouseButtonDown(MOUSE_BUTTON_MOVE_OBJECT);

    Vector2 mouse_pos = GetScreenToWorld2D(GetMousePosition(), g->camera);
    int mouse_cursor = MOUSE_CURSOR_DEFAULT;
    da_foreach(Object, object, &g->objects) {
        Rectangle top_resize_hitbox = {
            object->rec.x, object->rec.y - object_resize_hitbox_size / 2.0f,
            object->rec.width, object_resize_hitbox_size,
        };
        Rectangle bottom_resize_hitbox = {
            object->rec.x, object->rec.y + object->rec.height - object_resize_hitbox_size / 2.0f,
            object->rec.width, object_resize_hitbox_size,
        };
        Rectangle left_resize_hitbox = {
            object->rec.x - object_resize_hitbox_size / 2.0f, object->rec.y,
            object_resize_hitbox_size, object->rec.height,
        };
        Rectangle right_resize_hitbox = {
            object->rec.x + object->rec.width - object_resize_hitbox_size / 2.0f, object->rec.y,
            object_resize_hitbox_size, object->rec.height,
        };

        if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox)) {
            mouse_cursor = MOUSE_CURSOR_RESIZE_NS;

            if (is_move_down) {
                object->rec.y += mouse_delta.y;
                object->rec.height -= mouse_delta.y;
            }
        } else if (CheckCollisionPointRec(mouse_pos, bottom_resize_hitbox)) {
            mouse_cursor = MOUSE_CURSOR_RESIZE_NS;

            if (is_move_down) {
                object->rec.height += mouse_delta.y;
            }
        } else if (CheckCollisionPointRec(mouse_pos, left_resize_hitbox)) {
            mouse_cursor = MOUSE_CURSOR_RESIZE_EW;

            if (is_move_down) {
                object->rec.x += mouse_delta.x;
                object->rec.width -= mouse_delta.x;
            }
        } else if (CheckCollisionPointRec(mouse_pos, right_resize_hitbox)) {
            mouse_cursor = MOUSE_CURSOR_RESIZE_EW;

            if (is_move_down) {
                object->rec.width += mouse_delta.x;
            }
        } else if (CheckCollisionPointRec(mouse_pos, object->rec)) {
            mouse_cursor = MOUSE_CURSOR_RESIZE_ALL;

            if (is_move_down) {
                object->rec.x += mouse_delta.x;
                object->rec.y += mouse_delta.y;
            }
        } else continue;

        break;
    }
    if (mouse_cursor != g->prev_mouse_cursor) {
        SetMouseCursor(mouse_cursor);
        g->prev_mouse_cursor = mouse_cursor;
    }

    Clay_BeginLayout();
    CLAY({
        .id = CLAY_ID("HelloWorld"),
        //.layout.layoutDirection = CLAY_TOP_TO_BOTTOM,
        .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() },
        .backgroundColor = {24, 24, 24, 255},
    }) {
        uint16_t font_size = 50;
        Clay_TextElementConfig *config = CLAY_TEXT_CONFIG({.fontSize = font_size, .textColor = {255, 255, 255, 255}, });
        CLAY_TEXT(CLAY_STRING("Hello, World!"), config);
        CLAY_TEXT(CLAY_STRING("Foo, Bar!"), config);
    }
    Clay_RenderCommandArray commands = Clay_EndLayout();

    Drawing() {
        Clay_Raylib_Render(commands, &g->font);

        Mode2D(g->camera) {
            da_foreach(Object, object, &g->objects) {
                Rectangle source = { 0, 0, object->texture.width, object->texture.height };
                DrawTexturePro(object->texture, source, object->rec, Vector2Zero(), 0.0f, WHITE);
            }
        }

        DrawFPS(10, 10);
    }
}
