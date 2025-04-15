#include "app.h"

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
        Color as_rect_color;
    };
} Object;

void object_unload(Object *object) {
    switch (object->type) {
        case OBJ_TEXTURE:
            UnloadTexture(object->as_texture);
            break;
        case OBJ_RECT:
            break;
        default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
    }
}

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
};

App *g;

void handle_clay_error(Clay_ErrorData error) {
    nob_log(ERROR, "Clay Error: %.*s", error.errorText.length, error.errorText.chars);
}

// https://gist.github.com/983/e170a24ae8eba2cd174f
#define RGB_TO_HSV_IN_GLSL  \
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

    g->font = LoadFont("./fonts/Alegreya-Regular.ttf");
    g->clay = Clay_Initialize(clay_arena, (Clay_Dimensions) { GetScreenWidth(), GetScreenHeight() }, (Clay_ErrorHandler) { handle_clay_error, 0 });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, &g->font);

    g->current_color = WHITE;

    Image one_by_one_image = GenImageColor(1, 1, WHITE);
    g->one_by_one_texture = LoadTextureFromImage(one_by_one_image);
    UnloadImage(one_by_one_image);

    g->hue_picker_shader = LoadShaderFromMemory(NULL,
"#version 330\n"
RGB_TO_HSV_IN_GLSL
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"

"out vec4 finalColor;\n"

"void main() {\n"
"    float hue = fragTexCoord.x;\n"
"    vec3 rgb = hsv2rgb(vec3(hue, 1.0, 1.0));\n"
"    finalColor = vec4(rgb, 1.0);\n"
"}\n"
"\n");
    g->color_picker_shader = LoadShaderFromMemory(NULL,
"#version 330\n"
RGB_TO_HSV_IN_GLSL
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"

"uniform float hue;\n"

"out vec4 finalColor;\n"

"void main() {\n"
"    vec3 rgb = hsv2rgb(vec3(hue, fragTexCoord.x, 1-fragTexCoord.y));\n"
"    finalColor = vec4(rgb, 1.0);\n"
"}\n"
"\n");

    g->canvas_bounds = (Rectangle) {0, 0, 1920, 1080};
}

App* app_pre_reload(void) {
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
    g->camera.zoom *= wheel / 20.0f + 1;

    Vector2 mouse_delta = Vector2Scale(GetMouseDelta(), 1/g->camera.zoom);
    if (IsMouseButtonDown(MOUSE_BUTTON_PAN)) {
        g->camera.target = Vector2Subtract(g->camera.target, mouse_delta);
    }

    float object_resize_hitbox_size = OBJECT_RESIZE_HITBOX_SIZE / g->camera.zoom;
    bool is_move_down = IsMouseButtonDown(MOUSE_BUTTON_MOVE_OBJECT);
    Vector2 mouse_pos = GetScreenToWorld2D(GetMousePosition(), g->camera);
    int mouse_cursor = MOUSE_CURSOR_DEFAULT;
    if (g->tool == TOOL_MOVE && g->objects.count > 0) {
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

            if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox) && CheckCollisionPointRec(mouse_pos, left_resize_hitbox)) {
                mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                if (is_move_down) {
                    object->rec.y += mouse_delta.y;
                    object->rec.height -= mouse_delta.y;
                    object->rec.x += mouse_delta.x;
                    object->rec.width -= mouse_delta.x;
                }
            } else if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox) && CheckCollisionPointRec(mouse_pos, right_resize_hitbox)) {
                mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                if (is_move_down) {
                    object->rec.y += mouse_delta.y;
                    object->rec.height -= mouse_delta.y;
                    object->rec.width += mouse_delta.x;
                }
            } else if (CheckCollisionPointRec(mouse_pos, bottom_resize_hitbox) && CheckCollisionPointRec(mouse_pos, left_resize_hitbox)) {
                mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                if (is_move_down) {
                    object->rec.height += mouse_delta.y;
                    object->rec.x += mouse_delta.x;
                    object->rec.width -= mouse_delta.x;
                }
            } else if (CheckCollisionPointRec(mouse_pos, bottom_resize_hitbox) && CheckCollisionPointRec(mouse_pos, right_resize_hitbox)) {
                mouse_cursor = MOUSE_CURSOR_CROSSHAIR;

                if (is_move_down) {
                    object->rec.height += mouse_delta.y;
                    object->rec.width += mouse_delta.x;
                }
            } else if (CheckCollisionPointRec(mouse_pos, top_resize_hitbox)) {
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
                .as_rect_color = g->current_color,
            };
            object_set_name(&object, sv_from_cstr(temp_sprintf("Rectangle (#%02hhx%02hhx%02hhx)", g->current_color.r, g->current_color.g, g->current_color.b)));
            da_append(&g->objects, object);
        }
    }
    set_cursor(mouse_cursor);
}

void draw_scene(void) {
    da_foreach(Object, object, &g->objects) {
        switch (object->type) {
            case OBJ_TEXTURE: {
                Texture texture = object->as_texture;
                Rectangle source = { 0, 0, texture.width, texture.height };
                DrawTexturePro(texture, source, object->rec, Vector2Zero(), 0.0f, WHITE);
            } break;
            case OBJ_RECT: {
                DrawRectangleRec(object->rec, object->as_rect_color);
            } break;
            default: UNREACHABLE("invalid object type: you have a memory corruption somewhere. good luck");
        }
    }
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
            tool_button(CLAY_ID("MoveButton"), CLAY_STRING("Move"), TOOL_MOVE);
            tool_button(CLAY_ID("RectangleButton"), CLAY_STRING("Rectangle"), TOOL_RECT);
            if (button(CLAY_ID("AddImageButton"), CLAY_STRING("Add Image")).pressed) {
                const char *filter_patterns[] = { "*.png", "*.jpg", "*.tga", "*.bmp", "*.psd", "*.gif", "*.hdr", "*.pic", "*.ppm" };
                const char *path = tinyfd_openFileDialog("Add Image", NULL, ARRAY_LEN(filter_patterns), filter_patterns, "Image", 0);
                if (path != NULL) {
                    Texture texture = LoadTexture(path);
                    Object object = {
                        .as_texture = texture,
                        .rec = { 0, 0, texture.width, texture.height },
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
            }
            if (button(CLAY_ID("ExportButton"), CLAY_STRING("Export Image")).pressed) {
                const char *filter_patterns[] = {"*.png", "*.bmp", "*.tga", "*.jpg", "*.hdr"};
                const char *path = tinyfd_saveFileDialog("Export Image", NULL, ARRAY_LEN(filter_patterns), filter_patterns, "Image file");
                if (path != NULL) {
                    Camera2D camera = {
                        .zoom = 1.0f,
                        .offset = {g->canvas_bounds.x, g->canvas_bounds.y},
                    };

                    RenderTexture rtex = LoadRenderTexture(g->canvas_bounds.width, g->canvas_bounds.height);
                    Mode2D(camera) TextureMode(rtex) {
                        draw_scene();
                    }

                    Image img = LoadImageFromTexture(rtex.texture);
                    ImageFlipVertical(&img);
                    if (!ExportImage(img, path)) {
                        tinyfd_messageBox("Error exporting image", temp_sprintf("Could not export image to %s", path), "ok", "error", 1);
                    }
                    UnloadImage(img);

                    UnloadRenderTexture(rtex);
                }
            }

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
                    for (Object *object = g->objects.items + g->objects.count - 1; object >= g->objects.items; object--) {
                        CLAY({
                            .layout.sizing = { CLAY_SIZING_GROW(), CLAY_SIZING_FIT() },
                            .layout.childGap = 3,
                            .layout.layoutDirection = CLAY_LEFT_TO_RIGHT,
                        }) {
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
                                object_unload(object);

                                size_t i = object - g->objects.items;
                                memmove(object, object + 1, g->objects.count - i - 1);
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

            if (g->tool == TOOL_RECT && CheckCollisionPointRec(GetMousePosition(), main_area) && IsMouseButtonDown(MOUSE_BUTTON_DRAW_RECT)) {
                DrawRectangleRec(get_current_rect(), g->current_color);
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
