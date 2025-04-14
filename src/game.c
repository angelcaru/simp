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

#define OBJECT_RESIZE_HITBOX_SIZE 80

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

    Objects objects;
    Camera2D camera;

    // Sometimes SetMouseCursor() is extremely slow (once it dropped from >3000 FPS to ~15), so we keep track of the previous
    // cursor ourselves to avoid calling it too much. Raylib already keeps track of this but it doesn't use it for some reason
    // Should probably submit a PR to raylib
    int prev_mouse_cursor;
};

Game *g;

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
}

#define DEBUG(fmt, value, x, y) DrawText(TextFormat("%s = "fmt, #value, value), x, y, 20, LIME)

void game_update(void) {
    g->camera.offset = (Vector2) { (float)GetScreenWidth() / 2, (float)GetScreenHeight() / 2 };
    //g->camera.offset = (Vector2) { 0 };

    float wheel = GetMouseWheelMove();
    g->camera.zoom *= wheel / 20.0f + 1;

    Vector2 mouse_delta = Vector2Scale(GetMouseDelta(), 1/g->camera.zoom);
    if (IsMouseButtonDown(MOUSE_BUTTON_PAN)) {
        g->camera.target = Vector2Subtract(g->camera.target, mouse_delta);
    }

    float object_resize_hitbox_size = OBJECT_RESIZE_HITBOX_SIZE;

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

    Drawing() {
        ClearBackground(BLACK);

        Mode2D(g->camera) {
            da_foreach(Object, object, &g->objects) {
                Rectangle source = { 0, 0, object->texture.width, object->texture.height };
                DrawTexturePro(object->texture, source, object->rec, Vector2Zero(), 0.0f, WHITE);
            }
        }

        DrawFPS(10, 10);
    }
}
