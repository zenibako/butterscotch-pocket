/*
 * Deltarune: what the port does differently for it. See of_game.h.
 */

#include "of_game.h"
#include "of_buttons.h"
#include "gml_array.h"
#include "sw_renderer.h"

#include <stdlib.h>

#include "instance.h"
#include "gml_array.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "rvalue.h"
#include "vm.h"
#include "vm_builtins.h"
#include "stb_ds.h"
#include "of_state_requests.h"
#include "runner_keyboard.h"
#include <string.h>

#define DELTARUNE_GRASS_MAX_CELLS 4096

/* a < b as the VM's Cmp does it. */
static bool lessThan(GMLReal a, GMLReal b, GMLReal epsilon) {
    GMLReal diff = a - b;
    return GMLReal_fabs(diff) > epsilon && diff < 0;
}

static bool isNumber(RValue value) {
    return value.type == RVALUE_REAL || value.type == RVALUE_INT32;
}

#include "game_deltarune_sentry.h"

/* gml_Object_obj_purplegrass_Draw_0: the floor of the field, and
 * what is behind its battles. About 11,000 operations a frame for:
 *
 *   index += 0.2
 *   for (i = 0; i < length; i += 1)
 *       for (j = 0; j < height; j += 1)
 *           draw_sprite(sprite_index, index + x / 320 + i * 0.125 + j * 0.125 + y / 320,
 *                       x + 40 * i, y + 40 * j)
 */
static bool purpleGrassDraw(VMContext *ctx) {
    static int32_t indexVar = -1, lengthVar, heightVar, iVar, jVar;
    Instance *self = ctx->currentCallFrame->self;
    Runner *runner = ctx->runner;
    if (self == NULL || runner->renderer == NULL) return false;
    if (indexVar < 0) {
        indexVar = VM_getOrAllocateVarID(ctx, "index");
        lengthVar = VM_getOrAllocateVarID(ctx, "length");
        heightVar = VM_getOrAllocateVarID(ctx, "height");
        iVar = VM_getOrAllocateVarID(ctx, "i");
        jVar = VM_getOrAllocateVarID(ctx, "j");
    }
    RValue indexValue = Instance_getSelfVar(self, indexVar);
    RValue lengthValue = Instance_getSelfVar(self, lengthVar);
    RValue heightValue = Instance_getSelfVar(self, heightVar);
    if (!isNumber(indexValue) || !isNumber(lengthValue) || !isNumber(heightValue)) return false;

    GMLReal index = RValue_toReal(indexValue) + (GMLReal) 0.2;
    Instance_setSelfVar(self, indexVar, RValue_makeReal(index));
    GMLReal length = RValue_toReal(lengthValue), height = RValue_toReal(heightValue);
    GMLReal x = (GMLReal) self->x, y = (GMLReal) self->y;
    int32_t sprite = self->spriteIndex;
    GMLReal epsilon = runner->mathEpsilon;

    /* The loops' counters come out the same however many cells are drawn. */
    GMLReal i = 0, j = 0;
    int32_t cols = 0, rows = 0;
    for (; lessThan(i, length, epsilon); i = i + 1) cols++;
    bool innerRan = cols > 0;
    if (innerRan) for (; lessThan(j, height, epsilon); j = j + 1) rows++;

    /* The image of each cell, in the order the script draws them; then the
     * whole grid in one call if the renderer takes it, else cell by cell. */
    static int32_t subimgs[DELTARUNE_GRASS_MAX_CELLS];
    bool whole = cols > 0 && rows > 0 && cols <= DELTARUNE_GRASS_MAX_CELLS && rows <= DELTARUNE_GRASS_MAX_CELLS &&
                 cols * rows <= DELTARUNE_GRASS_MAX_CELLS && runner->renderer->vtable->drawSpriteGrid != NULL;
    GMLReal ci = 0;
    for (int32_t col = 0; col < cols; col++, ci = ci + 1) {
        GMLReal cj = 0;
        for (int32_t row = 0; row < rows; row++, cj = cj + 1) {
            GMLReal image = index + x / (GMLReal) 320;
            image = image + ci * (GMLReal) 0.125;
            image = image + cj * (GMLReal) 0.125;
            image = image + y / (GMLReal) 320;
            int32_t subimg = (int32_t) image;
            if (0 > subimg) subimg = (int32_t) self->imageIndex; /* as draw_sprite does */
            if (whole) subimgs[col * rows + row] = subimg;
            else Renderer_drawSprite(runner->renderer, sprite, subimg, (float) (x + (GMLReal) 40 * ci), (float) (y + (GMLReal) 40 * cj));
        }
    }
    if (whole && !runner->renderer->vtable->drawSpriteGrid(runner->renderer, sprite, subimgs, cols, rows, (float) x, (float) y, 40.0f, 40.0f)) {
        ci = 0;
        for (int32_t col = 0; col < cols; col++, ci = ci + 1) {
            GMLReal cj = 0;
            for (int32_t row = 0; row < rows; row++, cj = cj + 1)
                Renderer_drawSprite(runner->renderer, sprite, subimgs[col * rows + row], (float) (x + (GMLReal) 40 * ci), (float) (y + (GMLReal) 40 * cj));
        }
    }
    Instance_setSelfVar(self, iVar, RValue_makeReal(i));
    if (innerRan) Instance_setSelfVar(self, jVar, RValue_makeReal(j));
    return true;
}

/* gml_Script___view_get(prop, index): GameMaker's own shim for the
 * old view_xview[] family, a chain of seventeen compares that ends in one or
 * two built-in calls. Battle scripts call it some twenty times a frame.
 *
 *   switch (prop) { case 0: return camera_get_view_x(view_get_camera(index)); ... }
 *
 * Anything but a number from 0 to 16 for prop is left to the bytecode. */
static bool viewGet(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    static const char *const names[17] = {
        "camera_get_view_x", "camera_get_view_y", "camera_get_view_width", "camera_get_view_height", "camera_get_view_angle",
        "camera_get_view_border_x", "camera_get_view_border_y", "camera_get_view_speed_x", "camera_get_view_speed_y",
        "camera_get_view_target", "view_get_visible", "view_get_xport", "view_get_yport", "view_get_wport", "view_get_hport",
        "view_get_camera", "view_get_surface_id",
    };
    static BuiltinFunc functions[17];
    static int resolved = 0; /* 1: all found, -1: not all */
    if (resolved == 0) {
        resolved = 1;
        for (int k = 0; k < 17; k++) {
            functions[k] = VM_findBuiltin(ctx, names[k]);
            if (functions[k] == NULL) resolved = -1;
        }
    }
    if (resolved < 0 || argCount < 2 || args == NULL || !isNumber(args[0])) return false;

    /* Cmp EQ against 0, 1, 2, ... in turn, as the bytecode does it. */
    GMLReal prop = RValue_toReal(args[0]);
    GMLReal epsilon = ctx->runner ? ctx->runner->mathEpsilon : GML_MATH_EPSILON;
    int which = -1;
    for (int k = 0; k < 17 && which < 0; k++) {
        if (GMLReal_fabs(prop - (GMLReal) k) <= epsilon) which = k;
    }
    if (which < 0) return false;

    RValue index = args[1];
    if (which <= 9) {
        /* The camera's property: the view's camera first. */
        RValue camera = functions[15](ctx, &index, 1);
        *result = functions[which](ctx, &camera, 1);
        RValue_free(&camera);
    } else {
        *result = functions[which](ctx, &index, 1);
    }
    return true;
}

/* gml_Script_scr_84_get_sprite(name):
 *
 *   return ds_map_find_value(global.chemg_sprite_map, name)
 */
static int32_t g_spriteMapVar = -1; /* the VM's own id for the name; forgetVm forgets it for each new VM */
static bool get84Sprite(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    static BuiltinFunc find = NULL;
    if (find == NULL) find = VM_findBuiltin(ctx, "ds_map_find_value");
    if (g_spriteMapVar < 0) g_spriteMapVar = VM_getOrAllocateVarID(ctx, "chemg_sprite_map");
    int32_t mapVar = g_spriteMapVar;
    if (find == NULL || argCount < 1 || args == NULL || ctx->globalScopeInstance == NULL) return false;
    RValue call[2] = { Instance_getSelfVar(ctx->globalScopeInstance, mapVar), args[0] };
    *result = find(ctx, call, 2);
    return true;
}

/* gml_Script_tile_layer_shift(depth, x, y), callable body at offset 4.
 * All ten variables are locals; the separate global-init wrapper installs the
 * method. Keep the builtin order and their owned arrays: duplicate IDs, omitted
 * dead instances, missing tiles and integer coordinate conversion belong to the
 * existing builtins. This removes VM dispatch, not layer-element searches. */
static bool tileLayerShift(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    if (ctx->runner == NULL || args == NULL || argCount < 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!isNumber(args[i]) || !isfinite(RValue_toReal(args[i]))) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    if (!isfinite(epsilon) || epsilon < 0) return false;

    static const char *const names[] = {
        "layer_get_all", "array_length_1d", "layer_get_depth", "layer_get_all_elements",
        "layer_get_element_type", "layer_tile_get_x", "layer_tile_x", "layer_tile_get_y", "layer_tile_y",
    };
    BuiltinFunc functions[9];
    for (int i = 0; i < 9; ++i) {
        functions[i] = VM_findBuiltin(ctx, names[i]);
        if (functions[i] == NULL) return false;
    }

    /* No fallback after this point: replaying the bytecode would shift twice. */
    GMLReal depth = RValue_toReal(args[0]);
    GMLReal x = RValue_toReal(args[1]), y = RValue_toReal(args[2]);
    RValue layers = functions[0](ctx, NULL, 0);
    RValue layerCount = functions[1](ctx, &layers, 1);
    for (int32_t i = 0; lessThan((GMLReal)i, RValue_toReal(layerCount), epsilon); ++i) {
        RValue layer = GMLArray_getOnArrayRef(&layers, i);
        RValue layerDepth = functions[2](ctx, &layer, 1);
        /* IDs returned as REAL may round to a missing ID. Undefined is
         * unequal to every supported numeric depth, including zero. */
        bool different = layerDepth.type == RVALUE_UNDEFINED ||
                         !(GMLReal_fabs(RValue_toReal(layerDepth) - depth) <= epsilon);
        RValue_free(&layerDepth);
        if (different) continue;
        RValue elements = functions[3](ctx, &layer, 1);
        RValue elementCount = functions[1](ctx, &elements, 1);
        for (int32_t j = 0; lessThan((GMLReal)j, RValue_toReal(elementCount), epsilon); ++j) {
            RValue element = GMLArray_getOnArrayRef(&elements, j);
            RValue type = functions[4](ctx, &element, 1);
            bool tile = GMLReal_fabs(RValue_toReal(type) - (GMLReal)7) <= epsilon;
            RValue_free(&type);
            if (!tile) continue;
            RValue oldX = functions[5](ctx, &element, 1);
            RValue setX[2] = { element, RValue_makeReal(RValue_toReal(oldX) + x) };
            RValue_free(&oldX);
            RValue unused = functions[6](ctx, setX, 2);
            RValue_free(&unused);
            RValue oldY = functions[7](ctx, &element, 1);
            RValue setY[2] = { element, RValue_makeReal(RValue_toReal(oldY) + y) };
            RValue_free(&oldY);
            unused = functions[8](ctx, setY, 2);
            RValue_free(&unused);
        }
        RValue_free(&elementCount);
        RValue_free(&elements);
    }
    RValue_free(&layerCount);
    RValue_free(&layers);
    *result = RValue_makeUndefined();
    return true;
}

/* The common, already formatted English battle writer. Other styles and
 * command strings stay in the interpreter. The event and its input callees
 * are fingerprinted before any state changes; the same builtins retain draw
 * ordering, conversion, ownership and random-number advancement. */
enum {
    W_FORMATTED, W_FONT, W_SPECIAL, W_SHAKE, W_HALT, W_COLOR, W_STRING,
    W_LENGTH, W_POS, W_X, W_Y, W_HSPACE, W_VSPACE, W_WXSKIP, W_SKIPPABLE,
    W_SKIPME, W_SINER, W_AUTOMASH, W_BUTTON1, W_BUTTON2, W_BUTTON3,
    W_ACCEPT, W_WX, W_WY, W_COLORCHANGE, W_N, W_CHAR, W_LANG, W_FLAG, W_COUNT
};
static int32_t writerVars[W_COUNT];
static int writerResolved; /* 0 unresolved, 1 compatible, -1 incompatible */
static int32_t writerScripts[4];
static RValue writerGet(VMContext *ctx, int field) {
    return Instance_getSelfVar(field >= W_LANG ? ctx->globalScopeInstance : ctx->currentCallFrame->self, writerVars[field]);
}
static void writerSet(VMContext *ctx, int field, RValue value) {
    Instance_setSelfVar(ctx->currentCallFrame->self, writerVars[field], value);
}
static void writerNumber(VMContext *ctx, int field, GMLReal value) {
    writerSet(ctx, field, RValue_makeReal(value));
}
static bool writerFinite(RValue value) {
    return isNumber(value) && isfinite(RValue_toReal(value));
}
static bool writerEqual(RValue value, GMLReal number, GMLReal epsilon) {
    return GMLReal_fabs(RValue_toReal(value) - number) <= epsilon;
}
static void writerDiscard(VMContext *ctx, BuiltinFunc fn, RValue *args, int count) {
    RValue value = fn(ctx, args, count);
    RValue_free(&value);
}
static bool writerDraw(VMContext *ctx) {
    if (ctx->currentCallFrame->self == NULL || ctx->globalScopeInstance == NULL || ctx->runner == NULL ||
        ctx->runner->renderer == NULL || ctx->runner->mathEpsilon != GML_MATH_EPSILON) return false;
    if (writerResolved == 0) {
        static const char *const scripts[] = {
            "gml_Script_button1_p", "gml_Script_button2_h", "gml_Script_scr_debug", "gml_Script_button3_h"
        };
        static const uint32_t lengths[] = {204,128,68,128};
        static const uint64_t hashes[] = {
            0xd467a2dc432f29b7ull,0x72767ee209807646ull,0x847afa0367d28889ull,0x6287575da25d418eull
        };
        writerResolved = -1;
        for (int i = 0; i < 4; ++i) {
            uint32_t length = 0;
            if (VM_codeHash(ctx, scripts[i], &length) != hashes[i] || length != lengths[i]) return false;
            ptrdiff_t slot = shgeti(ctx->codeIndexByName, scripts[i]);
            if (slot < 0) return false;
            writerScripts[i] = ctx->codeIndexByName[slot].value;
        }
        static const char *const names[W_COUNT] = {
            "formatted","myfont","special","shake","halt","mycolor","mystring",
            "length","pos","writingx","writingy","hspace","vspace","wxskip","skippable",
            "skipme","siner","automash_timer","button1","button2","button3",
            "accept","wx","wy","colorchange","n","mychar","lang","flag"
        };
        for (int i = 0; i < W_COUNT; ++i) writerVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
        writerResolved = 1;
    }
    if (writerResolved < 0) return false;
    static const int fields[] = {W_FORMATTED,W_FONT,W_SPECIAL,W_SHAKE,W_HALT,W_COLOR};
    static const GMLReal values[] = {1,5,1,0,0,16777215};
    for (int i = 0; i < 6; ++i) {
        RValue value = writerGet(ctx, fields[i]);
        if (!writerFinite(value) || RValue_toReal(value) != values[i]) return false;
    }
    RValue language = writerGet(ctx, W_LANG), string = writerGet(ctx, W_STRING);
    if (language.type != RVALUE_STRING || language.string == NULL || strcmp(language.string,"en") ||
        string.type != RVALUE_STRING || string.string == NULL) return false;
    size_t length = 0;
    for (const unsigned char *s = (const unsigned char *)string.string; *s; ++s) {
        if (++length > 1024 || *s < 32 || *s > 126 || strchr("/%^\\#", *s) != NULL) return false;
    }
    for (int i = W_LENGTH; i <= W_AUTOMASH; ++i)
        if (!writerFinite(writerGet(ctx,i))) return false;
    if (RValue_toReal(writerGet(ctx,W_LENGTH)) != (GMLReal)length) return false;
    GMLReal pos = RValue_toReal(writerGet(ctx,W_POS));
    if (pos < 1 || pos > (GMLReal)length + 2 || pos != GMLReal_floor(pos)) return false;
    if (writerGet(ctx,W_X).type != RVALUE_REAL || writerGet(ctx,W_Y).type != RVALUE_REAL ||
        writerGet(ctx,W_SINER).type != RVALUE_REAL) return false;
    for (int i = W_WXSKIP; i <= W_SKIPME; ++i) {
        GMLReal value = RValue_toReal(writerGet(ctx,i));
        if (value != 0 && value != 1) return false;
    }
    GMLReal automash = RValue_toReal(writerGet(ctx,W_AUTOMASH));
    if (automash != 0 && automash != 1) return false;
    RValue flags = writerGet(ctx,W_FLAG);
    if (flags.type != RVALUE_ARRAY || flags.array == NULL) return false;
    RValue debugFlag = GMLArray_get(flags.array,10);
    if (!writerFinite(debugFlag)) return false;
    enum { FONT,COLOR,LENGTH,CHAR,GET_COLOR,NEWLINE,RANDOM,TEXT_COLOR,TEXT };
    static const char *const builtins[] = {
        "draw_set_font","draw_set_color","string_length","string_char_at","draw_get_color",
        "string_hash_to_newline","random","draw_text_color","draw_text"
    };
    BuiltinFunc fn[9];
    for (int i = 0; i < 9; ++i) {
        fn[i] = VM_findBuiltin(ctx,builtins[i]);
        if (fn[i] == NULL) return false;
    }
    /* No fallback after this point. Input calls use their original scripts. */
    GMLReal epsilon = ctx->runner->mathEpsilon;
    writerNumber(ctx,W_BUTTON1,0); writerNumber(ctx,W_BUTTON2,0); writerNumber(ctx,W_BUTTON3,0);
    RValue result = VM_callCodeIndex(ctx,writerScripts[0],NULL,0);
    if (writerEqual(result,1,epsilon)) writerNumber(ctx,W_BUTTON1,1);
    RValue_free(&result);
    result = VM_callCodeIndex(ctx,writerScripts[1],NULL,0);
    if (writerEqual(result,1,epsilon)) writerNumber(ctx,W_BUTTON2,1);
    RValue_free(&result);
    bool debugEnabled = writerEqual(debugFlag,1,epsilon);
    if (!debugEnabled) {
        result = VM_callCodeIndex(ctx,writerScripts[2],NULL,0);
        debugEnabled = RValue_toBool(result); RValue_free(&result);
    }
    if (debugEnabled) {
        result = VM_callCodeIndex(ctx,writerScripts[3],NULL,0);
        bool mash = writerEqual(result,1,epsilon); RValue_free(&result);
        if (mash) {
            automash = automash == 0 ? 1 : 0; writerNumber(ctx,W_AUTOMASH,automash);
            if (automash == 0) { writerNumber(ctx,W_BUTTON1,1); writerNumber(ctx,W_BUTTON2,1); }
        }
    }
    writerNumber(ctx,W_ACCEPT,0);
    writerSet(ctx,W_WX,writerGet(ctx,W_X)); writerSet(ctx,W_WY,writerGet(ctx,W_Y));
    writerNumber(ctx,W_COLORCHANGE,0);
    RValue argument = writerGet(ctx,W_FONT); writerDiscard(ctx,fn[FONT],&argument,1);
    argument = writerGet(ctx,W_COLOR); writerDiscard(ctx,fn[COLOR],&argument,1);
    if (writerEqual(writerGet(ctx,W_BUTTON2),1,epsilon) && lessThan(pos,(GMLReal)length,epsilon) &&
        writerEqual(writerGet(ctx,W_SKIPPABLE),1,epsilon)) writerNumber(ctx,W_SKIPME,1);
    if (writerEqual(writerGet(ctx,W_SKIPME),1,epsilon)) {
        result = fn[LENGTH](ctx,&string,1); pos = RValue_toReal(result) + 1;
        // ADD retains INT32 when string_length and the literal are integers;
        // this Pop.v.v does not perform the literal-store REAL coercion.
        writerSet(ctx,W_POS,result.type == RVALUE_INT32 ? RValue_makeInt32(result.int32 + 1) : RValue_makeReal(pos));
        RValue_free(&result);
        /* SETOWNER precedes the two builtin alarm stores in the bytecode. */
        ctx->currentArrayOwner = (void *)(intptr_t)68121;
        ctx->currentCallFrame->self->alarm[0] = ctx->currentCallFrame->self->alarm[1] = -1;
        ctx->currentCallFrame->self->activeAlarmMask &= (uint16_t)~3u;
    }
    writerNumber(ctx,W_N,1);
    for (GMLReal n = 1; lessThan(n,pos,epsilon); n = n + 1) {
        writerNumber(ctx,W_ACCEPT,1);
        RValue args[2] = {string,writerGet(ctx,W_N)};
        RValue character = fn[CHAR](ctx,args,2);
        writerSet(ctx,W_CHAR,character); RValue_free(&character);
        character = writerGet(ctx,W_CHAR);
        if (!strcmp(character.string,"&")) {
            writerNumber(ctx,W_ACCEPT,0); writerSet(ctx,W_WX,writerGet(ctx,W_X));
            if (writerEqual(writerGet(ctx,W_WXSKIP),1,epsilon))
                writerNumber(ctx,W_WX,RValue_toReal(writerGet(ctx,W_X)) + 58);
            writerNumber(ctx,W_WY,RValue_toReal(writerGet(ctx,W_WY)) + RValue_toReal(writerGet(ctx,W_VSPACE)));
        }
        if (!strcmp(character.string,"|")) {
            writerNumber(ctx,W_ACCEPT,0);
            writerNumber(ctx,W_WX,RValue_toReal(writerGet(ctx,W_WX)) + RValue_toReal(writerGet(ctx,W_HSPACE)));
        }
        if (writerEqual(writerGet(ctx,W_ACCEPT),1,epsilon)) {
            writerDiscard(ctx,fn[GET_COLOR],NULL,0);
            for (int shadow = 1; shadow >= 0; --shadow) {
                RValue text = fn[NEWLINE](ctx,&character,1);
                argument = writerGet(ctx,W_SHAKE);
                result = fn[RANDOM](ctx,&argument,1);
                GMLReal y = RValue_toReal(writerGet(ctx,W_WY)) + RValue_toReal(result); RValue_free(&result);
                result = fn[RANDOM](ctx,&argument,1);
                GMLReal x = RValue_toReal(writerGet(ctx,W_WX)) + RValue_toReal(result); RValue_free(&result);
                if (shadow) { x = x + 1; y = y + 1; }
                RValue draw[8] = {RValue_makeReal(x),RValue_makeReal(y),text,RValue_makeInt32(4210752),
                    RValue_makeInt32(4210752),RValue_makeInt32(8388608),RValue_makeInt32(8388608),RValue_makeInt32(1)};
                writerDiscard(ctx,fn[shadow ? TEXT_COLOR : TEXT],draw,shadow ? 8 : 3);
                RValue_free(&text);
            }
            writerNumber(ctx,W_WX,RValue_toReal(writerGet(ctx,W_WX)) + RValue_toReal(writerGet(ctx,W_HSPACE)));
        }
        writerNumber(ctx,W_N,n + 1);
    }
    writerNumber(ctx,W_SKIPME,0);
    writerNumber(ctx,W_SINER,RValue_toReal(writerGet(ctx,W_SINER)) + 1);
    return true;
}

/* Live lookup for the compatibility backgrounds. Preserve layer/element
 * builtin ordering and ID conversion; remove bytecode and temporary strings.
 * No cached names or IDs survive a call, room change, or Memory restore. */
static bool backgroundGetElement(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    if (!ctx->runner || !args || argCount < 1 || !isNumber(args[0]) ||
        !isfinite(RValue_toReal(args[0])) || ctx->runner->mathEpsilon != GML_MATH_EPSILON) return false;
    static const char *const names[] = {
        "layer_get_all", "layer_get_name", "layer_get_all_elements", "layer_get_element_type",
    };
    BuiltinFunc fn[4];
    for (int i=0;i<4;++i) {
        fn[i]=VM_findBuiltin(ctx,names[i]);
        if (!fn[i]) return false;
    }
    RValue layers=fn[0](ctx,NULL,0), layer=RValue_makeInt32(-1);
    bool foreground=false, found=false;
    for (int i=0;i<GMLArray_length1D(layers.array);++i) {
        RValue id=GMLArray_get(layers.array,i), name=fn[1](ctx,&id,1);
        if (name.type!=RVALUE_STRING || !name.string) {
            RValue_free(&name);RValue_free(&layers);return false;
        }
        /* Byte and codepoint offsets agree only for ASCII. Unusual names use
         * the bytecode's UTF-8 and numeric conversion behavior. */
        const char *text=name.string;size_t length=0;
        while (text[length] && (unsigned char)text[length]<128) ++length;
        if (text[length]) {RValue_free(&name);RValue_free(&layers);return false;}
        foreground=strstr(text,"Compatibility_Foreground_")!=NULL;
        bool background=!foreground && strstr(text,"Compatibility_Background_")!=NULL;
        size_t slot=sizeof("Compatibility_Background_")-1;
        /* Both prefixes have the same length. The original takes the slot at
         * that absolute offset, even when the substring occurs later. */
        bool match=false;
        if ((foreground || background) && length>slot) {
            unsigned char digit=(unsigned char)text[slot];
            if (digit<'0' || digit>'9') {RValue_free(&name);RValue_free(&layers);return false;}
            match=GMLReal_fabs((GMLReal)(digit-'0')-RValue_toReal(args[0]))<=GML_MATH_EPSILON;
        }
        RValue_free(&name);
        if (match) {layer=id;found=true;break;}
    }
    RValue_free(&layers);
    /* Missing backgrounds may be created by the script; decline before any
     * semantic state change so that only the original creation path runs. */
    if (!found || GMLReal_fabs(RValue_toReal(layer)+(GMLReal)1)<=GML_MATH_EPSILON) return false;
    RValue values[3]={RValue_makeInt32(-1),RValue_makeInt32(-1),RValue_makeInt32(-1)};
    RValue elements=fn[2](ctx,&layer,1);
    for (int i=0;i<GMLArray_length1D(elements.array);++i) {
        RValue id=GMLArray_get(elements.array,i), type=fn[3](ctx,&id,1);
        bool background=GMLReal_fabs(RValue_toReal(type)-(GMLReal)1)<=GML_MATH_EPSILON;
        RValue_free(&type);
        if (background) {values[0]=id;values[1]=layer;values[2]=RValue_makeInt32(foreground ? 1 : 0);}
    }
    RValue_free(&elements);
    *result=RValue_makeArray(GMLArray_create(ctx->dataWin,3));
    ctx->currentArrayOwner=(void*)(intptr_t)18385354;
    result->array->owner=ctx->currentArrayOwner;
    for (int i=0;i<3;++i) {
        values[i].gmlStackType=GML_TYPE_VARIABLE; /* bytecode's Pop.v.v array writes */
        GMLArray_set(result->array,i,values[i]);
    }
    return true;
}

/* Input names are assembled outside STRG. Follow the active binding array,
 * retaining bytecode for foreign controllers or buttons the Pocket lacks. */
static bool inputName(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    if (argCount < 1 || args == NULL || ctx->globalScopeInstance == NULL || !isNumber(args[0])) return false;
    Instance *globals = ctx->globalScopeInstance;
    if (RValue_toBool(Instance_getSelfVar(globals, VM_getOrAllocateVarID(ctx, "is_console")))) return false;
    Runner *runner = ctx->runner;
    bool gamepad = false;
    for (ptrdiff_t i = 0; i < arrlen(runner->instances); i++) {
        Instance *inst = runner->instances[i];
        if (!inst->active || inst->destroyed || inst->objectIndex < 0 ||
            (uint32_t)inst->objectIndex >= runner->dataWin->objt.count) continue;
        if (!strcmp(runner->dataWin->objt.objects[inst->objectIndex].name, "obj_gamecontroller") &&
            RValue_toBool(Instance_getSelfVar(inst, VM_getOrAllocateVarID(ctx, "gamepad_active")))) {
            int pad = RValue_toInt32(Instance_getSelfVar(inst, VM_getOrAllocateVarID(ctx, "gamepad_id")));
            if (!runner->gamepads || pad != 0 || !RunnerGamepad_isConnected(runner->gamepads, pad) ||
                strcmp(RunnerGamepad_getGuid(runner->gamepads, pad), "openfpga-pocket")) return false;
            gamepad = true;
            break;
        }
    }
    RValue input = Instance_getSelfVar(globals, VM_getOrAllocateVarID(ctx, gamepad ? "input_g" : "input_k"));
    int32_t action = RValue_toInt32(args[0]);
    if (input.type != RVALUE_ARRAY || action < 0 || action >= GMLArray_length1D(input.array)) return false;
    RValue key = GMLArray_get(input.array, action);
    if (!isNumber(key)) return false;
    const char *icon = NULL;
    if (gamepad) {
        switch (RawToGPDelta(RValue_toInt32(key))) {
            case GP_PADU: icon = OF_ICON_UP " "; break;
            case GP_PADD: icon = OF_ICON_DOWN " "; break;
            case GP_PADL: icon = OF_ICON_LEFT " "; break;
            case GP_PADR: icon = OF_ICON_RIGHT " "; break;
            case GP_FACE1: icon = OF_ICON_A " "; break;
            case GP_FACE2: icon = OF_ICON_B " "; break;
            case GP_FACE4: icon = OF_ICON_X " "; break;
            case GP_START: icon = OF_ICON_START " "; break;
        }
    } else icon = ofButtonForKey(RValue_toInt32(key));
    if (icon == NULL) return false;
    *result = RValue_makeString(icon);
    return true;
}

static void installPrompts(Runner *runner) {
    bool installed = VM_setNativeScript(runner->vmContext, "gml_Script_scr_get_input_name",
                                       596, 0x9edb73f2ab55ed00ull, inputName);
    logInfo("Buttons: generated input names %s\n", installed ? "installed" : "left unchanged (different code)");
}

static const OfNativeScript g_nativeScripts[] = {
    { "gml_Script___background_get_element", 1340, 0xfd927fdac407744cull, backgroundGetElement },
    { "gml_Script_tile_layer_shift", 608, 0x7ba14ffb5cf975f3ull, tileLayerShift },
    { "gml_Script___view_get", 1260, 0xeff822f793176fb9ull, viewGet },
    { "gml_Script_scr_84_get_sprite", 84, 0x4121b98b835bc977ull, get84Sprite },
};

static const OfNativeCode g_natives[] = {
    { "gml_Object_obj_darkponman_ow_Step_0", 2236, 0x2e65f505e4e7b03eull, forestSentryStep },
    { "gml_Object_obj_writer_Draw_0", 12780, 0xad8c918f7aad19f8ull, writerDraw },
    { "gml_Object_obj_purplegrass_Draw_0", 348, 0x8cf611b685a27620ull, purpleGrassDraw },
};

static void forgetVm(void) {
    sentryResolved = false;
    writerResolved = 0;
    g_spriteMapVar = -1;
}

/* The opening is played as it comes (left and confirm, which gets through the
 * first text and into the character creation screens). The town and the Dark
 * World are a long way into the game, so the run jumps to them: the party and
 * the dark-zone flag are set the frame before each jump. In the field the
 * walk to the right runs into the room's enemy, and confirm plays the battle
 * out. Room numbers and frame counts are for the chapter 1 data file. */
#define DELTARUNE_YARD_FRAME 2100
#define DELTARUNE_FIELD_FRAME 2600
static const char g_script[] =
    "150:L*4,160:Z,174:L*4,184:Z,198:L*4,208:Z,222:L*4,232:Z,246:L*4,256:Z,270:L*4,280:Z,294:L*4,304:Z,318:L*4,"
    "328:Z,342:L*4,352:Z,366:L*4,376:Z,390:L*4,400:Z,414:L*4,424:Z,438:L*4,448:Z,462:L*4,472:Z,486:L*4,496:Z,"
    "510:L*4,520:Z,534:L*4,544:Z,558:L*4,568:Z,582:L*4,592:Z,606:L*4,616:Z,630:L*4,640:Z,654:L*4,664:Z,678:L*4,"
    "688:Z,702:L*4,712:Z,726:L*4,736:Z,750:L*4,760:Z,774:L*4,784:Z,798:L*4,808:Z,822:L*4,832:Z,846:L*4,856:Z,"
    "870:L*4,880:Z,894:L*4,904:Z,918:L*4,928:Z,942:L*4,952:Z,966:L*4,976:Z,990:L*4,1000:Z,1014:L*4,1024:Z,1038:L*4,"
    "1048:Z,1062:L*4,1072:Z,1086:L*4,1096:Z,1110:L*4,1120:Z,1134:L*4,1144:Z,1158:L*4,1168:Z,1182:L*4,1192:Z,"
    "1206:L*4,1216:Z,1230:L*4,1240:Z,1254:L*4,1264:Z,1278:L*4,1288:Z,1302:L*4,1312:Z,1326:L*4,1336:Z,1350:L*4,"
    "1360:Z,1374:L*4,1384:Z,1398:L*4,1408:Z,1422:L*4,1432:Z,1446:L*4,1456:Z,1470:L*4,1480:Z,1494:L*4,1504:Z,"
    "1518:L*4,1528:Z,1542:L*4,1552:Z,1566:L*4,1576:Z,1590:L*4,1600:Z,1614:L*4,1624:Z,1638:L*4,1648:Z,1662:L*4,"
    "1672:Z,1686:L*4,1696:Z,1710:L*4,1720:Z,1734:L*4,1744:Z,1758:L*4,1768:Z,1782:L*4,1792:Z,1806:L*4,1816:Z,"
    "1830:L*4,1840:Z,1854:L*4,1864:Z,1878:L*4,1888:Z,1902:L*4,1912:Z,1926:L*4,1936:Z,1950:L*4,1960:Z,1974:L*4,"
    "1984:Z,1998:L*4,2008:Z,2022:L*4,2032:Z,2046:L*4,2056:Z,2150:R*120,2290:D*60,2370:L*100,2490:U*60,2720:R*500,"
    "3000:Z,3015:Z,3030:Z,3045:Z,3060:Z,3075:Z,3090:Z,3105:Z,3120:Z,3135:Z,3150:Z,3165:Z,3180:Z,3195:Z,3210:Z,"
    "3225:Z,3240:Z,3255:Z,3270:Z,3285:Z,3300:Z,3315:Z,3330:Z,3345:Z,3360:Z,3375:Z,3390:Z,3405:Z,3420:Z,3435:Z,"
    "3450:Z,3465:Z,3480:Z,3495:Z,3510:Z,3525:Z,3540:Z,3555:Z,3570:Z,3585:Z";

static const OfJump g_jumps[] = {
    { DELTARUNE_YARD_FRAME - 1, -1, "plot=60,interact=0" }, /* past the scene that would otherwise start in the yard */
    { DELTARUNE_YARD_FRAME, 7, NULL },
    { DELTARUNE_FIELD_FRAME - 1, -1, "darkzone=1,char[1]=2,char[2]=3,plot=60,interact=0" },
    { DELTARUNE_FIELD_FRAME, 52, NULL },
};

static const OfBenchSection g_sections[] = {
    { "opening text 320x240", 1700, "text" },
    { "creation screens 320", 2100, "creat" },
    { "town yard 320x240", 2600, "yard" },
    { "field (640x480)", 3050, "field" },
    /* The battle in three: choosing and attacking, the enemy's turn (the
     * heart in its box among the bullets), and the turn after. */
    { "battle: menu, attack", 3225, "bmenu" },
    { "battle: dodging", 3380, "dodge" },
    { "battle: next turn", 3600, "bnext" },
};

/* The cutscene marker describes player control, not a missing snapshot
 * resource. The runner checks the actual live resources at frame boundaries.
 * Only the build this was measured on (Chapter 1, WAD 17) is accepted. */
static const char *savestateReason(Runner *r) {
    if (r->dataWin->gen8.wadVersion != 17 || strcmp(r->dataWin->gen8.name, "DELTARUNE"))
        return "Unsupported game version";
    return NULL;
}

static bool inCutscene(Runner *r) {
    for (ptrdiff_t i = 0; i < arrlen(r->instances); i++) {
        Instance *inst = r->instances[i];
        if (!inst->active || inst->destroyed || inst->objectIndex < 0 ||
            (uint32_t)inst->objectIndex >= r->dataWin->objt.count) continue;
        const char *name = r->dataWin->objt.objects[inst->objectIndex].name;
        if (!strcmp(name, "obj_mainchara") && ofStateNumber(r, inst, "cutscene") != 0)
            return true;
    }
    return false;
}

/* The phase lives in a GML global so loading A after reaching B also rewinds
 * the test itself. The game sets cutscene naturally;
 * the test changes only the story flag used to enter the real yard scene. */
static const char *sleepTest(Runner *r) {
    if (r->dataWin->room.count <= 7 ||
        strcmp(r->dataWin->room.rooms[7].name, "room_town_krisyard"))
        return "Sleep test needs Chapter 1";
    if (r->frameCount < 30) return "Preparing sleep test";
    VMContext *vm = r->vmContext;
    Instance *globals = vm->globalScopeInstance;
    int32_t phaseId = VM_getOrAllocateVarID(vm, "__pocket_sleep_test_phase");
    RValue phase = Instance_getSelfVar(globals, phaseId);
    bool start = phase.type == RVALUE_UNDEFINED;
    bool advance = !start && RValue_toInt32(phase) == 1 && r->currentRoomIndex == 7 &&
        r->pendingRoom < 0 &&
        (RunnerKeyboard_checkPressed(r->keyboard, VK_ENTER) ||
         RunnerGamepad_buttonCheckPressed(r->gamepads, 0, GP_START));
    if (start || advance) {
        Instance_setSelfVar(globals, VM_getOrAllocateVarID(vm, "plot"), RValue_makeReal(start ? 60 : 0));
        Instance_setSelfVar(globals, VM_getOrAllocateVarID(vm, "interact"), RValue_makeReal(0));
        phase = RValue_makeReal(start ? 1 : 2);
        Instance_setSelfVar(globals, phaseId, phase);
        r->pendingRoom = 7;
        RunnerKeyboard_clear(r->keyboard, VK_ENTER);
        logInfo("Sleep test: checkpoint %s\n", start ? "A (allowed)" : "B (cutscene)");
    }
    if (RValue_toInt32(phase) == 1) return "A: sleep/wake, then " OF_ICON_START;
    return inCutscene(r) ? "B: sleep/wake in cutscene" : "B: waiting for cutscene";
}

static const DataWin *g_battleData;
static int32_t g_battleController = -1;

static void battleUiInstall(Runner *runner) {
    static const char *const sprites[] = {
        "spr_btact", "spr_btdefend", "spr_btfight", "spr_btspare", "spr_btitem", "spr_bttech"
    };
    static const char *const glyphSprites[] = {
        "spr_bnamekris", "spr_bnameralsei", "spr_bnamesusie", "spr_hpname", "spr_hpslash"
    };
    static const char *const bubbleFonts[] = { "fnt_dotumche" };
    SWRenderer_setContrastAssets(runner->renderer, sprites, sizeof(sprites) / sizeof(sprites[0]),
                                bubbleFonts, sizeof(bubbleFonts) / sizeof(bubbleFonts[0]));
    SWRenderer_setContrastGlyphAssets(runner->renderer, glyphSprites, sizeof(glyphSprites) / sizeof(glyphSprites[0]),
                                     NULL, 0);
    // Half-size names are not doubled pixel art (15 source rows). Point sampling
    // keeps their counters open; a five-pixel R gives its leg a distinct diagonal.
    // Fingerprints reject modified/localized labels before deriving any pixels.
    static const SWHintedGlyphAsset hintedNames[] = {
        {"spr_bnamekris", 43, 15, 0x8c29ca15u, 6, 5, {30, 17, 17, 30, 20, 18, 17, 17}},
        {"spr_bnameralsei", 61, 15, 0xd514b019u, 0, 5, {30, 17, 17, 30, 20, 18, 17, 17}},
        {"spr_bnamesusie", 48, 15, 0x6af54445u, 0, 0, {0}}
    };
    SWRenderer_setHintedGlyphAssets(runner->renderer, hintedNames, sizeof(hintedNames) / sizeof(hintedNames[0]));
    g_battleData = runner->dataWin;
    g_battleController = -1;
    for (uint32_t i = 0; i < runner->dataWin->objt.count; ++i) {
        const char *name = runner->dataWin->objt.objects[i].name;
        if (name != NULL && strcmp(name, "obj_battlecontroller") == 0) {
            g_battleController = (int32_t) i;
            break;
        }
    }
}

static bool battleUiActive(Runner *runner) {
    bool enabled = false;
    if (runner->dataWin == g_battleData && runner->instancesByObject != NULL &&
        g_battleController >= 0 && (uint32_t) g_battleController < runner->dataWin->objt.count) {
        Instance **instances = runner->instancesByObject[g_battleController];
        for (ptrdiff_t i = 0; i < arrlen(instances); ++i)
            enabled |= instances[i]->active && !instances[i]->destroyed;
    }
    return enabled;
}

const OfGame ofGame = {
    .installPrompts = installPrompts,
    .battleUiInstall = battleUiInstall,
    .battleUiActive = battleUiActive,
    .natives = g_natives,
    .nativeCount = sizeof(g_natives) / sizeof(g_natives[0]),
    .nativeScripts = g_nativeScripts,
    .nativeScriptCount = sizeof(g_nativeScripts) / sizeof(g_nativeScripts[0]),
    .nativesForgetVm = forgetVm,
    .benchTitle = "Deltarune ch. 1",
    .benchScript = g_script,
    .benchJumps = g_jumps,
    .benchJumpCount = (int) (sizeof(g_jumps) / sizeof(g_jumps[0])),
    .benchSections = g_sections,
    .benchSectionCount = (int) (sizeof(g_sections) / sizeof(g_sections[0])),
    /* The audio pack is about 43 MB against Undertale's 134; the read tests use the same offsets, scaled. */
    .ioDivisor = 3u,
    .ioAsyncBase = 35u,
    .ioAsyncStep = 1u,
    .savestateReason = savestateReason,
    .sleepTest = sleepTest,
};
