#include "app.h"

#include <math.h>
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

#include "tinyfiledialogs.h"

#include "bundle.c"

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
#define ShaderMode(shader) BEGIN_END(BeginShaderMode(shader), EndShaderMode())

#define MOUSE_BUTTON_PAN MOUSE_BUTTON_RIGHT
#define MOUSE_BUTTON_MOVE_OBJECT MOUSE_BUTTON_LEFT
#define MOUSE_BUTTON_DRAW_RECT MOUSE_BUTTON_LEFT
#define MOUSE_BUTTON_DRAW MOUSE_BUTTON_LEFT

#define OBJECT_RESIZE_HITBOX_SIZE 30
#define HOVERED_OBJECT_OUTLINE_THICKNESS 5

typedef struct {
    Vector2 *items;
    size_t count, capacity;
    Color color;
    float weight;
} Stroke;

typedef enum {
    OBJ_TEXTURE,
    OBJ_RECT,
    OBJ_STROKE,
    COUNT_OBJS,
} Object_Type;

#define OBJ_NAME_MAX 128
typedef struct {
    Object_Type type;
    char name[OBJ_NAME_MAX];
    size_t name_len;
    union {
        struct {
            Rectangle rec;
            Texture texture;
        } as_texture;
        struct {
            Rectangle rec;
            Color color;
        } as_rect;
        Stroke as_stroke;
    };
} Object;

void object_unload(Object *object) {
    static_assert(COUNT_OBJS == 3, "Exhaustive handling of object types in object_unload");
    switch (object->type) {
        case OBJ_TEXTURE:
            UnloadTexture(object->as_texture.texture);
            break;
        case OBJ_RECT: break;
        case OBJ_STROKE:
            da_free(object->as_stroke);
            break;
        case COUNT_OBJS:
        default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
    }
}

void object_set_name(Object *object, String_View name) {
    size_t count = name.count;
    if (count > OBJ_NAME_MAX) count = OBJ_NAME_MAX;
    memcpy(object->name, name.data, count);
    object->name_len = count;
}

Rectangle object_get_bounding_box(const Object *object) {
    static_assert(COUNT_OBJS == 3, "Exhaustive handling of object types in object_get_bounding_box");
    switch (object->type) {
        case OBJ_RECT: return object->as_rect.rec;
        case OBJ_TEXTURE: return object->as_texture.rec;
        case OBJ_STROKE: {
            Vector2 min = {INFINITY, INFINITY};
            Vector2 max = {-INFINITY, -INFINITY};

            da_foreach(Vector2, point, &object->as_stroke) {
                min = Vector2Min(min, *point);
                max = Vector2Max(max, *point);
            }

            return (Rectangle) { min.x, min.y, max.x - min.x, max.y - min.y };
        } break;
        case COUNT_OBJS:
        default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
    }
}

void object_set_bounding_box(Object *object, Rectangle new) {
    static_assert(COUNT_OBJS == 3, "Exhaustive handling of object types in object_set_bounding_box");
    switch (object->type) {
        case OBJ_RECT:    object->as_rect.rec = new; break;
        case OBJ_TEXTURE: object->as_texture.rec = new; break;
        case OBJ_STROKE: {
            Rectangle old = object_get_bounding_box(object);
            Vector2 old_min = {old.x, old.y};
            Vector2 new_min = {new.x, new.y};
            Vector2 old_size = {old.width, old.height};
            Vector2 new_size = {new.width, new.height};

            //Vector2 translate = Vector2Subtract(new_min, old_min);
            Vector2 scale = Vector2Divide(new_size, old_size);

            da_foreach(Vector2, point, &object->as_stroke) {
                Vector2 old_point_rel = Vector2Subtract(*point, old_min);
                Vector2 new_point_rel = Vector2Multiply(old_point_rel, scale);
                *point = Vector2Add(new_point_rel, new_min);
            }
        } break;
        case COUNT_OBJS:
        default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
    }
}

typedef struct {
    Object *items;
    size_t count, capacity;
} Objects;

typedef enum {
    TOOL_MOVE = 0,
    TOOL_RECT,
    TOOL_DRAW,
    TOOL_CHANGE_CANVAS,
    COUNT_TOOLS,
} Tool;

struct App {
    size_t size;

    Clay_Context *clay;
    Font font;
    Tool tool;
    Vector2 rect_start;
    Color current_color;
    bool color_picker_open;
    Texture one_by_one_texture;
    Shader color_picker_shader;
    Shader hue_picker_shader;
    float curr_hue;
    Vector2 color_picker_pos;
    Objects objects;
    Camera2D camera;

    // Sometimes SetMouseCursor() is extremely slow (once it dropped from >3000 FPS to ~15), so we keep track of the previous
    // cursor ourselves to avoid calling it too much. Raylib already keeps track of this but it doesn't use it for some reason
    // Should probably submit a PR to raylib
    MouseCursor prev_mouse_cursor;

    Rectangle canvas_bounds;
    int hovered_object;
    Stroke current_stroke;
    float stroke_weight;
};

App *g;

void handle_clay_error(Clay_ErrorData error) {
    nob_log(ERROR, "Clay Error: %.*s", error.errorText.length, error.errorText.chars);
}

#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
EM_JS(void, save_file, (const unsigned char *data, size_t count), {
    async function saveFile(data) {
        if ("showSaveFilePicker" in window) {
            const handle = await showSaveFilePicker();
            const writable = await handle.createWritable();
            await writable.write(data);
            await writable.close();
        } else {
            const a = document.createElement("a");
            const file = new Blob([data], { type: "image/png" });
            a.href = URL.createObjectURL(file);
            a.download = "image.png";
            document.body.append(a);
            a.click();
            document.body.removeChild(a);
        }
    }

    return saveFile(HEAPU8.subarray(data, data + count))
        .catch(error => alert("Couldn't save file: " + String(error)));
});

//EM_JS(unsigned char *, load_file, (char *file_extension_buf, size_t *size), {
//    async function loadFile() {
//        if ("showOpenFilePicker" in window) {
//            const [handle] = await showOpenFilePicker();
//            debugger;
//            const file = await handle.getFile();
//            const data = new Uint8Array(await file.arrayBuffer());
//            HEAPU32[size>>2] = data.byteLength;
//            const extension = "." + file.name.split(".").at(-1);
//            HEAPU8
//                .subarray(file_extension_buf, file_extension_buf + extension.length)
//                .set(new TextEncoder().encode(extension));
//            const ptr = _malloc(data.byteLength);
//            if (ptr === 0) {
//                alert("Buy MORE RAM loll!!!!!!");
//                return 0;
//            }
//            HEAPU8
//                .subarray(ptr, ptr+data.byteLength)
//                .set(data);
//            return ptr;
//        }
//    }
//
//    return loadFile().catch(error => {
//        alert("Couldn't load file: " + String(error));
//        return 0;
//    });
//});
#endif // PLATFORM_WEB

#ifdef PLATFORM_WEB
#define GLSL_BOILERPLATE \
    "precision mediump float;\n" \
    "#define in varying\n" \
    "#define finalColor gl_FragColor\n"
#else
#define GLSL_BOILERPLATE \
    "#version 330\n" \
    "out vec4 finalColor;\n"
#endif // PLATFORM_WEB

// https://gist.github.com/983/e170a24ae8eba2cd174f
#define GLSL_RGB_TO_HSV  \
    "vec3 rgb2hsv(vec3 c) {\n" \
    "    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);\n" \
    "    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));\n" \
    "    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));\n" \
    "\n" \
    "    float d = q.x - min(q.w, q.y);\n" \
    "    float e = 1.0e-10;\n" \
    "    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);\n" \
    "}\n" \
    "\n" \
    "vec3 hsv2rgb(vec3 c) {\n" \
    "    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);\n" \
    "    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);\n" \
    "    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);\n" \
    "}\n"

void app_init(void) {
    g = malloc(sizeof(*g));
    memset(g, 0, sizeof(*g));
    g->size = sizeof(*g);

    g->camera.zoom = 1;
    g->camera.target = (Vector2) { (float)GetScreenWidth() / 2, (float)GetScreenHeight() / 2 };

    uint64_t total_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(total_memory_size, malloc(total_memory_size));

    // NOTE: I have no idea what the last 3 parameters of LoadFontFromMemory() mean,
    // I just copied them from the source code of the regular LoadFont()
    g->font = LoadFontFromMemory(".ttf", font_data, font_len, 32, NULL, 95);
    g->clay = Clay_Initialize(clay_arena, (Clay_Dimensions) { GetScreenWidth(), GetScreenHeight() }, (Clay_ErrorHandler) { handle_clay_error, 0 });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, &g->font);

    g->current_color = WHITE;

    Image one_by_one_image = GenImageColor(1, 1, WHITE);
    g->one_by_one_texture = LoadTextureFromImage(one_by_one_image);
    UnloadImage(one_by_one_image);

    g->hue_picker_shader = LoadShaderFromMemory(NULL,
GLSL_BOILERPLATE
GLSL_RGB_TO_HSV
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"

"void main() {\n"
"    float hue = fragTexCoord.x;\n"
"    vec3 rgb = hsv2rgb(vec3(hue, 1.0, 1.0));\n"
"    finalColor = vec4(rgb, 1.0);\n"
"}\n"
"\n");
    g->color_picker_shader = LoadShaderFromMemory(NULL,
GLSL_BOILERPLATE
GLSL_RGB_TO_HSV
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"

"uniform float hue;\n"

"void main() {\n"
"    vec3 rgb = hsv2rgb(vec3(hue, fragTexCoord.x, 1.0-fragTexCoord.y));\n"
"    finalColor = vec4(rgb, 1.0);\n"
"}\n"
"\n");

    g->canvas_bounds = (Rectangle) {0, 0, 1920, 1080};
    g->hovered_object = -1;
}

App *app_pre_reload(void) {
    return g;
}

void app_post_reload(App *new_g) {
    g = new_g;

    if (g->size < sizeof(*g)) {
        nob_log(INFO, "Migrating struct App (%zu bytes -> %zu bytes)", g->size, sizeof(*g));
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

Button_State tool_button(Clay_ElementId id, Clay_String text, Tool tool) {
    Button_State state;
    CLAY({
        .id = id,
        .layout.padding = { .left = 5, .right = 5 },
        .backgroundColor = (state.hovered = Clay_Hovered()) || g->tool == tool ? (Clay_Color) { 150, 150, 150, 255 } : (Clay_Color){ 100, 100, 100, 255 },
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
    if (state.pressed) g->tool = tool;
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
#ifdef PLATFORM_WEB
    wheel /= -240;
#endif // PLATFORM_WEB
    g->camera.zoom *= wheel / 20.0f + 1;

    Vector2 mouse_delta = Vector2Scale(GetMouseDelta(), 1/g->camera.zoom);
    if (IsMouseButtonDown(MOUSE_BUTTON_PAN)) {
        g->camera.target = Vector2Subtract(g->camera.target, mouse_delta);
    }

    float object_resize_hitbox_size = OBJECT_RESIZE_HITBOX_SIZE / g->camera.zoom;
    bool is_move_down = IsMouseButtonDown(MOUSE_BUTTON_MOVE_OBJECT);
    Vector2 mouse_pos = GetScreenToWorld2D(GetMousePosition(), g->camera);
    int mouse_cursor = MOUSE_CURSOR_DEFAULT;
    static_assert(COUNT_TOOLS == 4, "Exhaustive handling of tools in update_main_area");
    switch (g->tool) {
        case TOOL_MOVE: if (g->objects.count > 0) {
            for (Object *object = g->objects.items + g->objects.count - 1; object >= g->objects.items; object--) {
                Rectangle bounding_box = object_get_bounding_box(object);
                Rectangle top_resize_hitbox = {
                    bounding_box.x, bounding_box.y - object_resize_hitbox_size / 2.0f,
                    bounding_box.width, object_resize_hitbox_size,
                };
                Rectangle bottom_resize_hitbox = {
                    bounding_box.x, bounding_box.y + bounding_box.height - object_resize_hitbox_size / 2.0f,
                    bounding_box.width, object_resize_hitbox_size,
                };
                Rectangle left_resize_hitbox = {
                    bounding_box.x - object_resize_hitbox_size / 2.0f, bounding_box.y,
                    object_resize_hitbox_size, bounding_box.height,
                };
                Rectangle right_resize_hitbox = {
                    bounding_box.x + bounding_box.width - object_resize_hitbox_size / 2.0f, bounding_box.y,
                    object_resize_hitbox_size, bounding_box.height,
                };

                if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox) && CheckCollisionPointRec(mouse_pos, left_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                    if (is_move_down) {
                        bounding_box.y += mouse_delta.y;
                        bounding_box.height -= mouse_delta.y;
                        bounding_box.x += mouse_delta.x;
                        bounding_box.width -= mouse_delta.x;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox) && CheckCollisionPointRec(mouse_pos, right_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                    if (is_move_down) {
                        bounding_box.y += mouse_delta.y;
                        bounding_box.height -= mouse_delta.y;
                        bounding_box.width += mouse_delta.x;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, bottom_resize_hitbox) && CheckCollisionPointRec(mouse_pos, left_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                    if (is_move_down) {
                        bounding_box.height += mouse_delta.y;
                        bounding_box.x += mouse_delta.x;
                        bounding_box.width -= mouse_delta.x;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, bottom_resize_hitbox) && CheckCollisionPointRec(mouse_pos, right_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                    if (is_move_down) {
                        bounding_box.height += mouse_delta.y;
                        bounding_box.width += mouse_delta.x;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_RESIZE_NS;

                    if (is_move_down) {
                        bounding_box.y += mouse_delta.y;
                        bounding_box.height -= mouse_delta.y;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, bottom_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_RESIZE_NS;

                    if (is_move_down) {
                        bounding_box.height += mouse_delta.y;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, left_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_RESIZE_EW;

                    if (is_move_down) {
                        bounding_box.x += mouse_delta.x;
                        bounding_box.width -= mouse_delta.x;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, right_resize_hitbox)) {
                    mouse_cursor = MOUSE_CURSOR_RESIZE_EW;

                    if (is_move_down) {
                        bounding_box.width += mouse_delta.x;
                    }
                } else if (CheckCollisionPointRec(mouse_pos, bounding_box)) {
                    mouse_cursor = MOUSE_CURSOR_RESIZE_ALL;

                    if (is_move_down) {
                        bounding_box.x += mouse_delta.x;
                        bounding_box.y += mouse_delta.y;
                    }
                } else continue;

                object_set_bounding_box(object, bounding_box);
                g->hovered_object = object - g->objects.items;

                break;
            }
        } break;
        case TOOL_RECT:
            if (IsMouseButtonPressed(MOUSE_BUTTON_DRAW_RECT)) {
                g->rect_start = mouse_pos;
            }

            if (IsMouseButtonReleased(MOUSE_BUTTON_DRAW_RECT)) {
                Object object = {
                    .type = OBJ_RECT,
                    .as_rect = {
                        .rec = get_current_rect(),
                        .color = g->current_color,
                    },
                };
                object_set_name(&object, sv_from_cstr(temp_sprintf("Rectangle (#%02hhx%02hhx%02hhx)", g->current_color.r, g->current_color.g, g->current_color.b)));
                da_append(&g->objects, object);
            }
            break;
        case TOOL_CHANGE_CANVAS:
            if (IsMouseButtonPressed(MOUSE_BUTTON_DRAW_RECT)) {
                g->rect_start = mouse_pos;
            }

            if (IsMouseButtonReleased(MOUSE_BUTTON_DRAW_RECT)) {
                g->canvas_bounds = get_current_rect();
            }
            break;
        case TOOL_DRAW:
            if (IsMouseButtonPressed(MOUSE_BUTTON_DRAW)) {
                assert(g->current_stroke.items == NULL
                    && g->current_stroke.count == 0
                    && g->current_stroke.capacity == 0);
                g->current_stroke.color = g->current_color;
                g->current_stroke.weight = g->stroke_weight;
            }

            if (IsMouseButtonDown(MOUSE_BUTTON_DRAW)) {
                da_append(&g->current_stroke, mouse_pos);
            }

            if (IsMouseButtonReleased(MOUSE_BUTTON_DRAW)) {
                Object object = {
                    .type = OBJ_STROKE,
                    .as_stroke = g->current_stroke,
                };
                object_set_name(&object, sv_from_cstr("Stroke"));
                da_append(&g->objects, object);
                memset(&g->current_stroke, 0, sizeof(g->current_stroke));
            }
            break;
        default: UNREACHABLE("invalid tool: you have a memory corruption somewhere. good luck");
    }
    set_cursor(mouse_cursor);
}

void draw_stroke(Stroke stroke) {
    if (stroke.count < 2) return;
    for (size_t i = 0; i < stroke.count - 1; i++) {
        Vector2 a = stroke.items[i];
        Vector2 b = stroke.items[i+1];
        DrawLineEx(a, b, stroke.weight, stroke.color);
    }
}

void draw_scene(void) {
    da_foreach(Object, object, &g->objects) {
        static_assert(COUNT_OBJS == 3, "Exhaustive handling of object types in draw_scene");
        switch (object->type) {
            case OBJ_TEXTURE: {
                Texture texture = object->as_texture.texture;
                Rectangle source = { 0, 0, texture.width, texture.height };
                DrawTexturePro(texture, source, object->as_texture.rec, Vector2Zero(), 0.0f, WHITE);
            } break;
            case OBJ_RECT: {
                DrawRectangleRec(object->as_rect.rec, object->as_rect.color);
            } break;
            case OBJ_STROKE: {
                draw_stroke(object->as_stroke);
            } break;
            case COUNT_OBJS:
            default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
        }
    }
}

void add_image_object(const char *path) {
    Texture texture = LoadTexture(path);
    Object object = {
        .as_texture = {
            .rec = { 0, 0, texture.width, texture.height },
            .texture = texture,
        },
    };
    String_View path_sv = sv_from_cstr(path);
    assert(path_sv.count > 0);
    int i;
    for (i = path_sv.count - 1; i >= 0; i--) {
        if (
            path_sv.data[i] == '/'
            #ifdef _WIN32
            || path_sv.data[i] == '\\'
            #endif
        ) {
            i++;
            break;
        }
    }
    path_sv = sv_from_parts(path_sv.data + i, path_sv.count - i);
    object_set_name(&object, path_sv);
    da_append(&g->objects, object);
}

RenderTexture export_image_to_render_texture(void) {
    Camera2D camera = {
        .zoom = 1.0f,
        .offset = {-g->canvas_bounds.x, -g->canvas_bounds.y},
    };

    int width = g->canvas_bounds.width;
    int height = g->canvas_bounds.height;

    RenderTexture rtex_flipped = LoadRenderTexture(width, height);
    TextureMode(rtex_flipped) Mode2D(camera)  {
        draw_scene();
    }
    RenderTexture rtex_nflipped = LoadRenderTexture(width, height);
    // flip texture
    TextureMode(rtex_nflipped) {
        DrawTexture(rtex_flipped.texture, 0, 0, WHITE);
    }

    UnloadRenderTexture(rtex_flipped);
    return rtex_nflipped;
}

void app_update(void) {
    size_t temp_checkpoint = temp_save();

    Clay_SetLayoutDimensions((Clay_Dimensions) { GetScreenWidth(), GetScreenHeight() });
    Clay_SetPointerState((Clay_Vector2) { GetMouseX(), GetMouseY() }, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    Vector2 wheel_v = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(true, (Clay_Vector2) { wheel_v.x, wheel_v.y }, GetFrameTime());

    if (IsKeyPressed(KEY_D)) {
        Clay_SetDebugModeEnabled(!Clay_IsDebugModeEnabled());
    }

    Rectangle main_area;
    CustomLayoutElement get_bounding_box = {
        .type = CUSTOM_LAYOUT_ELEMENT_TYPE_GET_BOUNDING_BOX,
        .customData.boundingBoxPtr = &main_area,
    };
    Rectangle color_picker;
    CustomLayoutElement get_color_picker = {
        .type = CUSTOM_LAYOUT_ELEMENT_TYPE_GET_BOUNDING_BOX,
        .customData.boundingBoxPtr = &color_picker,
    };
    Rectangle hue_picker;
    CustomLayoutElement get_hue_picker = {
        .type = CUSTOM_LAYOUT_ELEMENT_TYPE_GET_BOUNDING_BOX,
        .customData.boundingBoxPtr = &hue_picker,
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
            CLAY({
                .id = CLAY_ID("FileOptions"),
                .layout.layoutDirection = CLAY_LEFT_TO_RIGHT,
                .layout.childGap = 5,
            }) {
#ifndef PLATFORM_WEB
                if (button(CLAY_ID("OpenImageButton"), CLAY_STRING("Open Image")).pressed) {
                    const char *filter_patterns[] = { "*.png", "*.jpg", "*.tga", "*.bmp", "*.psd", "*.gif", "*.hdr", "*.pic", "*.ppm" };
                    const char *path = tinyfd_openFileDialog("Add Image", NULL, ARRAY_LEN(filter_patterns), filter_patterns, "Image", 0);
                    if (path != NULL) {
                        da_foreach(Object, object, &g->objects) {
                            object_unload(object);
                        }
                        g->objects.count = 0;
                        int size;
                        unsigned char *data = LoadFileData(path, &size);
                        if (data == NULL) {
                            tinyfd_messageBox("Error opening image", temp_sprintf("Could not load image from %s", path), "ok", "error", 1);
                        } else {
                            add_image_object(path);
                        }
                        g->canvas_bounds = g->objects.items[0].as_texture.rec;
                    }
                }
                if (button(CLAY_ID("ExportButton"), CLAY_STRING("Export Image")).pressed) {
                    const char *filter_patterns[] = {"*.png", "*.bmp", "*.tga", "*.jpg", "*.hdr"};
                    const char *path = tinyfd_saveFileDialog("Export Image", NULL, ARRAY_LEN(filter_patterns), filter_patterns, "Image file");
                    if (path != NULL) {
                        RenderTexture rtex = export_image_to_render_texture();
                        Image img = LoadImageFromTexture(rtex.texture);
                        if (!ExportImage(img, path)) {
                            tinyfd_messageBox("Error exporting image", temp_sprintf("Could not export image to %s", path), "ok", "error", 1);
                        }
                        UnloadImage(img);
                        UnloadRenderTexture(rtex);
                    }
                }
#else // defined(PLATFORM_WEB)
                if (button(CLAY_ID("ExportButton"), CLAY_STRING("Export Image")).pressed) {
                    RenderTexture rtex = export_image_to_render_texture();
                    Image img = LoadImageFromTexture(rtex.texture);
                    int file_size;
                    unsigned char *data = ExportImageToMemory(img, ".png", &file_size);
                    if (data == NULL) {
                        // TODO: report error
                    } else {
                        save_file(data, file_size);
                        free(data);
                    }
                    UnloadImage(img);
                    UnloadRenderTexture(rtex);
                }
#endif // PLATFORM_WEB
            }

            tool_button(CLAY_ID("ChangeCanvasButton"), CLAY_STRING("ChangeCanvas"), TOOL_CHANGE_CANVAS);
            tool_button(CLAY_ID("MoveButton"), CLAY_STRING("Move"), TOOL_MOVE);
            tool_button(CLAY_ID("RectangleButton"), CLAY_STRING("Rectangle"), TOOL_RECT);
            CLAY({
                .id = CLAY_ID("DrawButtonContainer"),
                .layout.layoutDirection = CLAY_LEFT_TO_RIGHT,
                .layout.childAlignment.y = CLAY_ALIGN_Y_CENTER,
                .layout.childGap = 5,
            }) {
                tool_button(CLAY_ID("DrawButton"), CLAY_STRING("Draw"), TOOL_DRAW);
                if (g->tool == TOOL_DRAW) {
                    const float slider_width = 100;
                    const float max_stroke_weight = 20;
                    const float knob_size = 20;
                    CLAY({
                        .id = CLAY_ID("StrokeWeightSlider"),
                        .layout.sizing = { CLAY_SIZING_FIXED(slider_width), CLAY_SIZING_FIXED(3) },
                        .layout.childAlignment.y = CLAY_ALIGN_Y_CENTER,
                        .backgroundColor = {255, 255, 255, 255},
                    }) {
                        bool hovered = Clay_Hovered();
                        float pos = Lerp(0, slider_width, g->stroke_weight / max_stroke_weight);
                        CLAY({ .layout.sizing.width = CLAY_SIZING_FIXED(pos - knob_size / 2) });
                        CLAY({
                            .layout.sizing = { CLAY_SIZING_FIXED(knob_size), CLAY_SIZING_FIXED(knob_size) },
                            .cornerRadius = CLAY_CORNER_RADIUS(knob_size),
                            .backgroundColor = {255, 255, 255, 255},
                        }) hovered |= Clay_Hovered();

                        if (hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                            float mouse_x = GetMouseX();
                            Clay_BoundingBox bounding_box = Clay_GetElementData(CLAY_ID("StrokeWeightSlider")).boundingBox;
                            g->stroke_weight = Lerp(1, max_stroke_weight, (mouse_x - bounding_box.x) / slider_width);
                        }
                    }
                    CLAY_TEXT(clay_string_from_cstr(temp_sprintf("%f", g->stroke_weight)), CLAY_TEXT_CONFIG({
                        .fontSize = 30,
                        .textColor = {255, 255, 255, 255},
                    }));
                }
            }
#ifndef PLATFORM_WEB
            if (button(CLAY_ID("AddImageButton"), CLAY_STRING("Add Image")).pressed) {
                const char *filter_patterns[] = { "*.png", "*.jpg", "*.tga", "*.bmp", "*.psd", "*.gif", "*.hdr", "*.pic", "*.ppm" };
                const char *path = tinyfd_openFileDialog("Add Image", NULL, ARRAY_LEN(filter_patterns), filter_patterns, "Image", 0);
                if (path != NULL) {
                    int size;
                    unsigned char *data = LoadFileData(path, &size);
                    if (data == NULL) {
                        tinyfd_messageBox("Error opening image", temp_sprintf("Could not load image from %s", path), "ok", "error", 1);
                    } else {
                        add_image_object(path);
                    }
                }
            }
#endif // PLATFORM_WEB

            CLAY({
                .id = CLAY_ID("ColorPickerLabelContainer"),
                .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_FIT() },
                .layout.layoutDirection = CLAY_LEFT_TO_RIGHT,
            }) {
                CLAY_TEXT(CLAY_STRING("Pick Color:"), CLAY_TEXT_CONFIG({
                    .fontSize = 30,
                    .textColor = {255, 255, 255, 255},
                }));
                CLAY({
                    .id = CLAY_ID("ColorDisplay"),
                    .layout.sizing = { CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(30) },
                    .backgroundColor = {g->current_color.r, g->current_color.g, g->current_color.b, g->current_color.a},
                    .cornerRadius = CLAY_CORNER_RADIUS(10),
                }) {
                    if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        g->color_picker_open = !g->color_picker_open;
                    }
                }
            }
            if (g->color_picker_open) {
                CLAY({
                    .id = CLAY_ID("HuePicker"),
                    .layout.sizing = {CLAY_SIZING_FIXED(128), CLAY_SIZING_FIXED(30)},
                    .custom = { &get_hue_picker },
                });
                CLAY({
                    .id = CLAY_ID("ColorPicker"),
                    .layout.sizing = {CLAY_SIZING_FIXED(128), CLAY_SIZING_FIXED(128)},
                    .custom = { &get_color_picker },
                });
            }

            CLAY({ .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() }});

            CLAY_TEXT(clay_string_from_cstr(temp_sprintf("Current Zoom Level: %f", g->camera.zoom)), CLAY_TEXT_CONFIG({
                .fontSize = 30,
                .textColor = {255, 255, 255, 255},
            }));


            if (g->objects.count > 0) {
                CLAY_TEXT(CLAY_STRING("Objects in Scene:"), CLAY_TEXT_CONFIG({
                    .fontSize = 30,
                    .textColor = {255, 255, 255, 255},
                }));

                Clay_TextElementConfig *text_config = CLAY_TEXT_CONFIG({
                    .fontSize = 25,
                    .textColor = {255, 255, 255, 255},
                });

                CLAY({
                    .id = CLAY_ID("ObjectList"),
                    .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_FIT() },
                    .layout.layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .scroll.vertical = true,
                }) {
                    g->hovered_object = -1;
                    for (Object *object = g->objects.items + g->objects.count - 1; object >= g->objects.items; object--) {
                        CLAY({
                            .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_FIT() },
                            .layout.childGap = 3,
                            .layout.layoutDirection = CLAY_LEFT_TO_RIGHT,
                        }) {
                            if (Clay_Hovered()) g->hovered_object = object - g->objects.items;
                            Clay_String name = {
                                .chars = object->name,
                                .length = object->name_len,
                                .isStaticallyAllocated = false,
                            };
                            CLAY_TEXT(name, text_config);

                            CLAY({ .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_GROW() } });

                            Button_State up_button = button((Clay_ElementId) {0}, CLAY_STRING("^"));
                            if (object != g->objects.items + g->objects.count - 1 && up_button.pressed) {
                                Object tmp = *object;
                                *object = *(object + 1);
                                *(object + 1) = tmp;
                            }
                            Button_State down_button = button((Clay_ElementId) {0}, CLAY_STRING("v"));
                            if (object != g->objects.items && down_button.pressed) {
                                Object tmp = *object;
                                *object = *(object - 1);
                                *(object - 1) = tmp;
                            }
                            if (button((Clay_ElementId) {0}, CLAY_STRING("Remove")).pressed) {
                                size_t i = object - g->objects.items;
                                object_unload(object);
                                nob_log(INFO, "Removing object %zu (%.*s)", i, (int)object->name_len, object->name);
                                memmove(object, object + 1, (g->objects.count - i - 1) * sizeof(*object));
                                g->objects.count -= 1;
                            }
                        }
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

            draw_scene();

            if (CheckCollisionPointRec(GetMousePosition(), main_area)) {
                if (g->tool == TOOL_RECT && IsMouseButtonDown(MOUSE_BUTTON_DRAW_RECT)) {
                    DrawRectangleRec(get_current_rect(), g->current_color);
                }
                if (g->tool == TOOL_CHANGE_CANVAS && IsMouseButtonDown(MOUSE_BUTTON_DRAW_RECT)) {
                    DrawRectangleLinesEx(get_current_rect(), 5, WHITE);
                }
                if (g->tool == TOOL_DRAW && IsMouseButtonDown(MOUSE_BUTTON_DRAW)) {
                    draw_stroke(g->current_stroke);
                }
            }

            if (g->hovered_object < 0 || (size_t)g->hovered_object >= g->objects.count) g->hovered_object = -1;
            if (g->hovered_object != -1) {
                Rectangle rec = object_get_bounding_box(&g->objects.items[g->hovered_object]);
                DrawRectangleLinesEx(rec, HOVERED_OBJECT_OUTLINE_THICKNESS / g->camera.zoom, WHITE);
            }

            DrawRectangleLinesEx(g->canvas_bounds, 5, WHITE);
        }

        if (g->color_picker_open) {
            Vector2 mouse = GetMousePosition();
            if (CheckCollisionPointRec(mouse, hue_picker) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                g->curr_hue = (mouse.x - hue_picker.x) / hue_picker.width;
            }

            ShaderMode(g->hue_picker_shader) {
                DrawTexturePro(g->one_by_one_texture, (Rectangle) {0, 0, 1, 1}, hue_picker, Vector2Zero(), 0, WHITE);
            }
            int x = hue_picker.x + g->curr_hue*hue_picker.width;
            DrawLine(x, hue_picker.y, x, hue_picker.y + hue_picker.height, WHITE);

            int loc = GetShaderLocation(g->color_picker_shader, "hue");
            SetShaderValue(g->color_picker_shader, loc, &g->curr_hue, SHADER_UNIFORM_FLOAT);
            ShaderMode(g->color_picker_shader) {
                DrawTexturePro(g->one_by_one_texture, (Rectangle) {0, 0, 1, 1}, color_picker, Vector2Zero(), 0, WHITE);
            }

            Vector2 corner = { color_picker.x, color_picker.y };
            Vector2 actual_pos = Vector2Add(corner, g->color_picker_pos);
            float radius = 10;

            if (CheckCollisionPointRec(mouse, color_picker) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                g->color_picker_pos = Vector2Subtract(mouse, corner);
            }
            ScissorModeRec(color_picker) {
                DrawCircleLinesV(actual_pos, radius, WHITE);
            }

            g->current_color = ColorFromHSV(g->curr_hue * 360, g->color_picker_pos.x / color_picker.width, 1 - g->color_picker_pos.y / color_picker.height);
        }
    }

    temp_rewind(temp_checkpoint);
}

// TODO: round the sizes of the objects to the nearest integer pixel (to better reflect how they'll look exported)
