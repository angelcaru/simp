#include "game.h"

#include <string.h>
#include <unistd.h>

#ifdef HOTRELOAD
    #define NOB_IMPLEMENTATION
#endif // HOTRELOAD
#define NOB_STRIP_PREFIX
#include "nob.h"

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_renderer_raylib.c"

Clay_String clay_string_from_cstr(const char *cstr) {
    return (Clay_String) { .chars = cstr, .length = strlen(cstr), .isStaticallyAllocated = false };
}

#include "raylib.h"
#include "raymath.h"
#define MACRO_VAR(name) _##name##__LINE__
#define BEGIN_END_NAMED(begin, end, i) for (int i = (begin, 0); i < 1; i++, end)
#define BEGIN_END(begin, end) BEGIN_END_NAMED(begin, end, MACRO_VAR(i))
#define Drawing() BEGIN_END(BeginDrawing(), EndDrawing())
#define Mode2D(camera) BEGIN_END(BeginMode2D(camera), EndMode2D())
#define ScissorMode(x, y, w, h) BEGIN_END(BeginScissorMode(x, y, w, h), EndScissorMode())
#define ScissorModeRec(rec) ScissorMode((rec).x, (rec).y, (rec).width, (rec).height)
#define TextureMode(texture) BEGIN_END(BeginTextureMode(texture), EndTextureMode())

#define MOUSE_BUTTON_PAN MOUSE_BUTTON_RIGHT
#define MOUSE_BUTTON_MOVE_OBJECT MOUSE_BUTTON_LEFT
#define MOUSE_BUTTON_DRAW_RECT MOUSE_BUTTON_LEFT

#define OBJECT_RESIZE_HITBOX_SIZE 30

typedef enum {
    OBJ_TEXTURE,
    OBJ_RECT,
} Object_Type;

#define OBJ_NAME_MAX 128
typedef struct {
    Object_Type type;
    Rectangle rec;
    char name[OBJ_NAME_MAX];
    size_t name_len;
    union {
        Texture as_texture;
    };
} Object;

void object_set_name(Object *object, String_View name) {
    size_t count = name.count;
    if (count > OBJ_NAME_MAX) count = OBJ_NAME_MAX;
    memcpy(object->name, name.data, count);
    object->name_len = count;
}

typedef struct {
    Object *items;
    size_t count, capacity;
} Objects;

typedef enum {
    TOOL_MOVE = 0,
    TOOL_RECT,
    COUNT_TOOLS,
} Tool;

struct Game {
    size_t size;

    Clay_Context *clay;
    Font font;

    Objects objects;
    Camera2D camera;

    // Sometimes SetMouseCursor() is extremely slow (once it dropped from >3000 FPS to ~15), so we keep track of the previous
    // cursor ourselves to avoid calling it too much. Raylib already keeps track of this but it doesn't use it for some reason
    // Should probably submit a PR to raylib
    MouseCursor prev_mouse_cursor;

    Tool tool;
    Vector2 rect_start;
};

Game *g;

void handle_clay_error(Clay_ErrorData error) {
    nob_log(ERROR, "Clay Error: %.*s", error.errorText.length, error.errorText.chars);
}

void game_init(void) {
    g = malloc(sizeof(*g));
    memset(g, 0, sizeof(*g));
    g->size = sizeof(*g);

    const char *path = "C_Logo.png";
    Texture texture = LoadTexture(path);
    Object object = {
        .as_texture = texture,
        .rec = { 0, 0, texture.width, texture.height },
    };
    object_set_name(&object, sv_from_cstr(path));
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

typedef struct {
    bool pressed : 1;
    bool hovered : 1;
} Button_State;

Button_State button(Clay_ElementId id, Clay_String text) {
    Button_State state;
    CLAY({
        .id = id,
        .layout.padding = { .left = 5, .right = 5 },
        .backgroundColor = (state.hovered = Clay_Hovered()) ? (Clay_Color) { 150, 150, 150, 255 } : (Clay_Color){ 100, 100, 100, 255 },
        .cornerRadius = CLAY_CORNER_RADIUS(5),
    }) {
        state.pressed = state.hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        uint16_t font_size = 30;
        Clay_TextElementConfig *config = CLAY_TEXT_CONFIG({
            .fontSize = font_size,
            .textColor = {255, 255, 255, 255},
        });
        CLAY_TEXT(text, config);
    }
    return state;
}

Rectangle get_current_rect(void) {
    Vector2 start = g->rect_start;
    Vector2 end = GetScreenToWorld2D(GetMousePosition(), g->camera);

    Vector2 corner1 = Vector2Min(start, end);
    Vector2 corner2 = Vector2Max(start, end);
    Vector2 size = Vector2Subtract(corner2, corner1);
    return (Rectangle) { corner1.x, corner1.y, size.x, size.y };
}

void set_cursor(MouseCursor mouse_cursor) {
    if (mouse_cursor != g->prev_mouse_cursor) {
        SetMouseCursor(mouse_cursor);
        g->prev_mouse_cursor = mouse_cursor;
    }
}

void update_main_area(void) {
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
    if (g->tool == TOOL_MOVE) {
        for (Object *object = g->objects.items + g->objects.count - 1; object >= g->objects.items; object--) {
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
    } else if (g->tool == TOOL_RECT) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_DRAW_RECT)) {
            g->rect_start = mouse_pos;
        }

        if (IsMouseButtonReleased(MOUSE_BUTTON_DRAW_RECT)) {
            Object object = {
                .type = OBJ_RECT,
                .rec = get_current_rect(),
            };
            object_set_name(&object, sv_from_cstr(temp_sprintf("Rectangle (%.0fx%.0f)", object.rec.width, object.rec.height)));
            da_append(&g->objects, object);
        }
    }
    set_cursor(mouse_cursor);
}

void game_update(void) {
    size_t temp_checkpoint = temp_save();

    Clay_SetLayoutDimensions((Clay_Dimensions) { GetScreenWidth(), GetScreenHeight() });
    Clay_SetPointerState((Clay_Vector2) { GetMouseX(), GetMouseY() }, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    Vector2 wheel_v = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(true, (Clay_Vector2) { wheel_v.x, wheel_v.y }, GetFrameTime());

    Rectangle main_area;
    CustomLayoutElement get_bounding_box = {
        .type = CUSTOM_LAYOUT_ELEMENT_TYPE_GET_BOUNDING_BOX,
        .customData.boundingBoxPtr = &main_area,
    };

    Clay_BeginLayout();
    CLAY({
        .id = CLAY_ID("Root"),
        .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() },
        .backgroundColor = {0, 0, 0, 255},
    }) {
        CLAY({
            .id = CLAY_ID("Sidebar"),
            .layout.sizing = { CLAY_SIZING_PERCENT(0.33), CLAY_SIZING_PERCENT(1) },
            .layout.layoutDirection = CLAY_TOP_TO_BOTTOM,
            .layout.childGap = 5,
            .layout.padding = CLAY_PADDING_ALL(10),
            .backgroundColor = {50, 50, 50, 255},
        }) {
            if (button(CLAY_ID("MoveButton"), CLAY_STRING("Move")).pressed) g->tool = TOOL_MOVE;
            if (button(CLAY_ID("RectangleButton"), CLAY_STRING("Rectangle")).pressed) g->tool = TOOL_RECT;

            CLAY({ .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() }});

            CLAY_TEXT(CLAY_STRING("Objects in Scene:"), CLAY_TEXT_CONFIG({
                .fontSize = 30,
                .textColor = {255, 255, 255, 255},
            }));

            Clay_TextElementConfig *text_config = CLAY_TEXT_CONFIG({
                .fontSize = 25,
                .textColor = {255, 255, 255, 255},
            });

            da_foreach(Object, object, &g->objects) {
                CLAY({
                    .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_FIT() },
                    .layout.layoutDirection = CLAY_LEFT_TO_RIGHT,
                }) {
                    Clay_String name = {
                        .chars = object->name,
                        .length = object->name_len,
                        .isStaticallyAllocated = false,
                    };
                    CLAY_TEXT(name, text_config);

                    CLAY({ .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() } });

                    if (button((Clay_ElementId) {0}, CLAY_STRING("Remove")).pressed) {
                        size_t i = object - g->objects.items;
                        memmove(object, object + 1, g->objects.count - i - 1);
                        g->objects.count -= 1;
                    }
                }
            }
        }
        CLAY({
            .id = CLAY_ID("MainArea"),
            .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() },
            .custom = { &get_bounding_box },
        });
    }
    Clay_RenderCommandArray commands = Clay_EndLayout();

    Drawing() {
        ClearBackground(GetColor(0xFF00FFFF));
        Clay_Raylib_Render(commands, &g->font);

        ScissorModeRec(main_area) Mode2D(g->camera) {
            if (CheckCollisionPointRec(GetMousePosition(), main_area)) {
                update_main_area();
            } else {
                set_cursor(MOUSE_CURSOR_DEFAULT);
            }

            da_foreach(Object, object, &g->objects) {
                switch (object->type) {
                    case OBJ_TEXTURE: {
                        Texture texture = object->as_texture;
                        Rectangle source = { 0, 0, texture.width, texture.height };
                        DrawTexturePro(texture, source, object->rec, Vector2Zero(), 0.0f, WHITE);
                    } break;
                    case OBJ_RECT: {
                        DrawRectangleRec(object->rec, RED);
                    } break;
                    default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
                }
            }

            if (g->tool == TOOL_RECT && CheckCollisionPointRec(GetMousePosition(), main_area) && IsMouseButtonDown(MOUSE_BUTTON_DRAW_RECT)) {
                DrawRectangleRec(get_current_rect(), RED);
            }
        }
    }

    temp_rewind(temp_checkpoint);
}
