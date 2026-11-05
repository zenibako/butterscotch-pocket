/* gml_Script_scr_charbox: the party's HP boxes under a battle or the Dark
 * World menu, and the five command buttons above the box of whoever is
 * choosing. About 2,300 operations a battle frame for three characters:
 *
 *   for (c = 0; c < 3; c += 1) if (havechar[c] == 1) {
 *       charcolor = hpcolor[c]; gc = global.charturn; xchunk from charpos[c] and chartotal
 *       if (gc == charpos[c]) { if (global.myfight == 0) scr_selectionmatrix(...); raise mmy[c] }
 *       else lower mmy[c]
 *       btc[0..4] = 0; if (gc == charpos[c]) btc[global.bmenucoord[0][global.charturn]] = 1
 *       if (global.fighting == 1) { spare/pacify glow, five buttons }
 *       two rectangles, face, name, HP numbers and bar
 *   }
 *
 * Written out operation for operation: the stores keep the VM's types (a
 * Pop.v.i literal becomes real, array stores and Pop.v.v keep what was
 * computed), arithmetic keeps integers only when both operands are, compares
 * use the runner's epsilon, and SETOWNER is set where the bytecode sets it so
 * the two array stores fork exactly when the VM's would. Builtins and the two
 * called scripts run as they would from the bytecode. Every value the script
 * reads is checked before anything is written; anything unusual (a missing
 * variable, a non-number, an index out of range) is left to the interpreter. */
enum {
    CB_C, CB_HAVECHAR, CB_CHARCOLOR, CB_HPCOLOR, CB_GC, CB_XCHUNK, CB_CHARPOS, CB_CHARTOTAL,
    CB_BP, CB_YY, CB_XX, CB_MMY, CB_BTC, CB_SPAREGLOW, CB_SGLOWI, CB_PACIFYGLOW, CB_BCOLOR,
    CB_BOFFSET, CB_BPOFF,
    CB_CHARTURN /* the globals from here on */, CB_MYFIGHT, CB_BMENUCOORD, CB_FIGHTING, CB_MONSTER, CB_MERCYMOD, CB_MONSTERSTATUS,
    CB_TENSION, CB_ENCOUNTERNO, CB_TIME, CB_CHARSELECT, CB_FACEACTION, CB_HPFONT, CB_HP, CB_MAXHP, CB_COUNT
};
enum { CBF_SPRITE, CBF_SPRITE_EXT, CBF_SIN, CBF_COLOR, CBF_RECT, CBF_FONT, CBF_HALIGN, CBF_NEWLINE, CBF_TEXT, CBF_CEIL, CBF_COUNT };
static int32_t charboxVars[CB_COUNT];
static BuiltinFunc charboxFn[CBF_COUNT];
static int32_t charboxSelection, charboxSprite; /* code indices of the two scripts it calls */
static int charboxResolved; /* 0 unresolved, 1 compatible, -1 incompatible */

static RValue cbGet(VMContext *ctx, int field) {
    return Instance_getSelfVar(field >= CB_CHARTURN ? ctx->globalScopeInstance : ctx->currentCallFrame->self, charboxVars[field]);
}
static RValue cbAt(VMContext *ctx, int field, int32_t index) {
    RValue array = cbGet(ctx, field);
    return GMLArray_getOnArrayRef(&array, index);
}
static void cbSet(VMContext *ctx, int field, RValue value) {
    Instance_setSelfVar(ctx->currentCallFrame->self, charboxVars[field], value);
}
static bool cbNumber(RValue value) {
    return isNumber(value) && isfinite(RValue_toReal(value)) && GMLReal_fabs(RValue_toReal(value)) <= (GMLReal) 1000000;
}
static RValue cbInt(int32_t value) { return RValue_makeInt32(value); }
static RValue cbReal(GMLReal value) { return RValue_makeReal(value); }
/* The VM's Add, Sub and Mul on two numbers: integer only when both are. */
static RValue cbAdd(RValue a, RValue b) {
    if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) return cbInt((int32_t) ((uint32_t) a.int32 + (uint32_t) b.int32));
    return cbReal(RValue_toReal(a) + RValue_toReal(b));
}
static RValue cbSub(RValue a, RValue b) {
    if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) return cbInt((int32_t) ((uint32_t) a.int32 - (uint32_t) b.int32));
    return cbReal(RValue_toReal(a) - RValue_toReal(b));
}
static RValue cbMul(RValue a, RValue b) {
    if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) return cbInt((int32_t) ((uint32_t) a.int32 * (uint32_t) b.int32));
    return cbReal(RValue_toReal(a) * RValue_toReal(b));
}
static RValue cbDiv(RValue a, RValue b) {
    return cbReal(RValue_toReal(a) / RValue_toReal(b));
}
/* Cmp's numeric path: 0 within epsilon, else the sign of a - b. */
static int cbCmp(RValue a, RValue b) {
    GMLReal diff = RValue_toReal(a) - RValue_toReal(b);
    return GMLReal_fabs(diff) <= GML_MATH_EPSILON ? 0 : diff < 0 ? -1 : 1;
}
/* A Pop into self.<field>[index] in BC17: fork the array if this scope does
 * not own it, grow it, store. The guards made sure the slot holds an array. */
static void cbStore(VMContext *ctx, int field, int32_t index, RValue value) {
    RValue *slot = IntRValueHashMap_getOrInsertUndefined(&ctx->currentCallFrame->self->selfVars, charboxVars[field]);
    if (slot->array->owner != ctx->currentArrayOwner) {
        GMLArray *clone = GMLArray_clone(slot->array, ctx->currentArrayOwner);
        GMLArray_decRef(slot->array);
        slot->array = clone;
        slot->ownsReference = true;
    }
    GMLArray_growTo(slot->array, index + 1);
    RValue_writeIntoSlotStealingOwnershipOrCopying(GMLArray_slot(slot->array, index), RValue_makeIndependent(value));
}
static void cbCall(VMContext *ctx, int fn, RValue *args, int count) {
    RValue value = charboxFn[fn](ctx, args, count);
    RValue_free(&value);
}
static RValue cbSprite(VMContext *ctx, const char *name) {
    RValue arg = RValue_makeString(name);
    return VM_callCodeIndex(ctx, charboxSprite, &arg, 1);
}
static void cbDrawSprite(VMContext *ctx, RValue sprite, RValue subimg, RValue x, RValue y) {
    RValue args[4] = { sprite, subimg, x, y };
    cbCall(ctx, CBF_SPRITE, args, 4);
    RValue_free(&sprite);
}
static void cbRectangle(VMContext *ctx, RValue x1, RValue y1, RValue x2, RValue y2) {
    RValue args[5] = { x1, y1, x2, y2, cbInt(0) };
    cbCall(ctx, CBF_RECT, args, 5);
}
static void cbColor(VMContext *ctx, RValue color) {
    cbCall(ctx, CBF_COLOR, &color, 1);
}

static bool charboxResolve(VMContext *ctx) {
    static const char *const names[CB_COUNT] = {
        "c", "havechar", "charcolor", "hpcolor", "gc", "xchunk", "charpos", "chartotal",
        "bp", "yy", "xx", "mmy", "btc", "spare_glow", "sglowi", "pacify_glow", "bcolor",
        "b_offset", "bpoff",
        "charturn", "myfight", "bmenucoord", "fighting", "monster", "mercymod", "monsterstatus",
        "tension", "encounterno", "time", "charselect", "faceaction", "hpfont", "hp", "maxhp"
    };
    static const char *const builtins[CBF_COUNT] = {
        "draw_sprite", "draw_sprite_ext", "sin", "draw_set_color", "draw_rectangle", "draw_set_font",
        "draw_set_halign", "string_hash_to_newline", "draw_text", "ceil"
    };
    /* The guards rest on what the called scripts touch: scr_selectionmatrix
     * draws and writes only its own variables, scr_84_get_sprite looks up a map. */
    static const char *const scripts[2] = { "gml_Script_scr_selectionmatrix", "gml_Script_scr_84_get_sprite" };
    static const uint32_t lengths[2] = { 868, 84 };
    static const uint64_t hashes[2] = { 0xf266a3a114d01cb5ull, 0x4121b98b835bc977ull };
    int32_t *indices[2] = { &charboxSelection, &charboxSprite };
    charboxResolved = -1;
    if (!IS_WAD17_OR_HIGHER(ctx) || !DataWin_isVersionAtLeast(ctx->dataWin, 2, 3, 1, 0)) return false;
    for (int i = 0; i < 2; ++i) {
        uint32_t length = 0;
        if (VM_codeHash(ctx, scripts[i], &length) != hashes[i] || length != lengths[i]) return false;
        ptrdiff_t slot = shgeti(ctx->codeIndexByName, scripts[i]);
        if (slot < 0) return false;
        *indices[i] = ctx->codeIndexByName[slot].value;
    }
    for (int i = 0; i < CBF_COUNT; ++i) {
        charboxFn[i] = VM_findBuiltin(ctx, builtins[i]);
        if (charboxFn[i] == NULL) return false;
    }
    for (int i = 0; i < CB_COUNT; ++i) charboxVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
    charboxResolved = 1;
    return true;
}

/* Everything the script will read, checked before it writes anything. Nothing
 * it calls changes these, so they hold for the whole run. */
static bool charboxReady(VMContext *ctx) {
    static const int numbers[] = {
        CB_CHARTOTAL, CB_BP, CB_YY, CB_XX, CB_BPOFF, CB_CHARTURN, CB_MYFIGHT, CB_FIGHTING,
        CB_TENSION, CB_ENCOUNTERNO, CB_TIME, CB_CHARSELECT
    };
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i)
        if (!cbNumber(cbGet(ctx, numbers[i]))) return false;
    /* Colours and the font only go to builtins, as whatever they hold. */
    if (cbGet(ctx, CB_HPFONT).type == RVALUE_UNDEFINED || cbGet(ctx, CB_BCOLOR).type == RVALUE_UNDEFINED) return false;
    RValue btc = cbGet(ctx, CB_BTC), mmy = cbGet(ctx, CB_MMY);
    if (btc.type != RVALUE_ARRAY || btc.array == NULL || mmy.type != RVALUE_ARRAY || mmy.array == NULL) return false;
    bool fighting = cbCmp(cbGet(ctx, CB_FIGHTING), cbInt(1)) == 0;
    RValue turn = cbGet(ctx, CB_CHARTURN);
    for (int32_t c = 0; c < 3; ++c) {
        if (!cbNumber(cbAt(ctx, CB_HAVECHAR, c)) || !cbNumber(cbAt(ctx, CB_MMY, c))) return false;
        if (fighting && (!cbNumber(cbAt(ctx, CB_MONSTER, c)) || !cbNumber(cbAt(ctx, CB_MERCYMOD, c)) ||
                         !cbNumber(cbAt(ctx, CB_MONSTERSTATUS, c)))) return false;
        if (cbCmp(cbAt(ctx, CB_HAVECHAR, c), cbInt(1)) != 0) continue;
        RValue pos = cbAt(ctx, CB_CHARPOS, c);
        if (!cbNumber(pos) || cbAt(ctx, CB_HPCOLOR, c).type == RVALUE_UNDEFINED ||
            !cbNumber(cbAt(ctx, CB_HP, c + 1)) || !cbNumber(cbAt(ctx, CB_MAXHP, c + 1))) return false;
        RValue faces = cbGet(ctx, CB_FACEACTION);
        if (faces.type != RVALUE_ARRAY || GMLArray_slot(faces.array, RValue_toInt32(pos)) == NULL) return false;
        if (cbCmp(turn, pos) == 0) {
            RValue coords = cbGet(ctx, CB_BMENUCOORD);
            RValue row = GMLArray_getOnArrayRef(&coords, 0);
            RValue button = GMLArray_getOnArrayRef(&row, RValue_toInt32(turn));
            if (row.type != RVALUE_ARRAY || !cbNumber(button) || RValue_toInt32(button) < 0 ||
                RValue_toInt32(button) > 64) return false;
        }
    }
    return true;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool charboxScript(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    (void) args; (void) argCount;
    if (ctx->currentCallFrame->self == NULL || ctx->globalScopeInstance == NULL || ctx->runner == NULL ||
        ctx->runner->mathEpsilon != GML_MATH_EPSILON) return false;
    if (charboxResolved == 0) charboxResolve(ctx);
    if (charboxResolved < 0 || !charboxReady(ctx)) return false;

    /* No fallback after this point. */
#define CB(field) cbGet(ctx, CB_##field)
#define CB_AT(field, index) cbAt(ctx, CB_##field, index)
    cbSet(ctx, CB_C, cbReal(0));
    while (cbCmp(CB(C), cbInt(3)) < 0) {
        RValue c = CB(C);
        int32_t ci = RValue_toInt32(c);
        if (cbCmp(CB_AT(HAVECHAR, ci), cbInt(1)) == 0) {
            for (int32_t k = 0; k < 3; ++k)
                if (cbCmp(c, cbInt(k)) == 0) cbSet(ctx, CB_CHARCOLOR, CB_AT(HPCOLOR, k));
            cbSet(ctx, CB_GC, CB(CHARTURN));
            cbSet(ctx, CB_XCHUNK, cbReal(0));
            static const struct { int32_t pos, total, xchunk; } chunks[6] = {
                {0, 3, 0}, {1, 3, 212}, {2, 3, 424}, {0, 2, 106}, {1, 2, 326}, {0, 1, 212}
            };
            for (int k = 0; k < 6; ++k)
                if (cbCmp(CB_AT(CHARPOS, ci), cbInt(chunks[k].pos)) == 0 && cbCmp(CB(CHARTOTAL), cbInt(chunks[k].total)) == 0)
                    cbSet(ctx, CB_XCHUNK, cbReal((GMLReal) chunks[k].xchunk));

            if (cbCmp(CB(GC), CB_AT(CHARPOS, ci)) == 0) {
                if (cbCmp(CB(MYFIGHT), cbInt(0)) == 0) {
                    RValue at[2] = { cbAdd(CB(XX), CB(XCHUNK)), cbAdd(cbSub(cbInt(480), CB(BP)), CB(YY)) };
                    RValue unused = VM_callCodeIndex(ctx, charboxSelection, at, 2);
                    RValue_free(&unused);
                }
                if (cbCmp(CB_AT(MMY, ci), cbInt(-32)) > 0) {
                    ctx->currentArrayOwner = (void *) (intptr_t) 8751160;
                    cbStore(ctx, CB_MMY, ci, cbSub(CB_AT(MMY, ci), cbInt(2)));
                }
                /* No SETOWNER before these three: the bytecode has only the one above. */
                if (cbCmp(CB_AT(MMY, ci), cbInt(-24)) > 0) cbStore(ctx, CB_MMY, ci, cbSub(CB_AT(MMY, ci), cbInt(4)));
                if (cbCmp(CB_AT(MMY, ci), cbInt(-16)) > 0) cbStore(ctx, CB_MMY, ci, cbSub(CB_AT(MMY, ci), cbInt(6)));
                if (cbCmp(CB_AT(MMY, ci), cbInt(-8)) > 0) cbStore(ctx, CB_MMY, ci, cbSub(CB_AT(MMY, ci), cbInt(8)));
                if (cbCmp(CB_AT(MMY, ci), cbInt(-32)) < 0) cbStore(ctx, CB_MMY, ci, cbInt(-64));
            } else {
                ctx->currentArrayOwner = (void *) (intptr_t) 8751160;
                if (cbCmp(CB_AT(MMY, ci), cbInt(-14)) < 0) cbStore(ctx, CB_MMY, ci, cbAdd(CB_AT(MMY, ci), cbInt(15)));
                else cbStore(ctx, CB_MMY, ci, cbInt(0));
            }

            ctx->currentArrayOwner = (void *) (intptr_t) 8751161;
            for (int32_t k = 0; k < 5; ++k) cbStore(ctx, CB_BTC, k, cbInt(0));
            if (cbCmp(CB(GC), CB_AT(CHARPOS, ci)) == 0) {
                RValue coords = CB(BMENUCOORD);
                RValue row = GMLArray_getOnArrayRef(&coords, 0);
                cbStore(ctx, CB_BTC, RValue_toInt32(GMLArray_getOnArrayRef(&row, RValue_toInt32(CB(CHARTURN)))), cbInt(1));
            }

            if (cbCmp(CB(FIGHTING), cbInt(1)) == 0) {
                cbSet(ctx, CB_SPAREGLOW, cbReal(0));
                for (cbSet(ctx, CB_SGLOWI, cbReal(0)); cbCmp(CB(SGLOWI), cbInt(3)) < 0;
                     cbSet(ctx, CB_SGLOWI, cbAdd(CB(SGLOWI), cbInt(1)))) {
                    int32_t m = RValue_toInt32(CB(SGLOWI));
                    if (cbCmp(CB_AT(MONSTER, m), cbInt(1)) == 0 && cbCmp(CB_AT(MERCYMOD, m), cbInt(100)) >= 0)
                        cbSet(ctx, CB_SPAREGLOW, cbReal(1));
                }
                cbSet(ctx, CB_PACIFYGLOW, cbReal(0));
                if (cbCmp(c, cbInt(2)) == 0) {
                    for (cbSet(ctx, CB_SGLOWI, cbReal(0)); cbCmp(CB(SGLOWI), cbInt(3)) < 0;
                         cbSet(ctx, CB_SGLOWI, cbAdd(CB(SGLOWI), cbInt(1)))) {
                        int32_t m = RValue_toInt32(CB(SGLOWI));
                        if (cbCmp(CB_AT(MONSTER, m), cbInt(1)) == 0 && cbCmp(CB_AT(MONSTERSTATUS, m), cbInt(1)) == 0 &&
                            cbCmp(CB(TENSION), cbInt(40)) >= 0) cbSet(ctx, CB_PACIFYGLOW, cbReal(1));
                        if (cbCmp(CB(ENCOUNTERNO), cbInt(31)) == 0) cbSet(ctx, CB_PACIFYGLOW, cbReal(0));
                    }
                }
                static const char *const buttons[5] = { "spr_btfight", NULL, "spr_btitem", "spr_btspare", "spr_btdefend" };
                static const int32_t offsets[5] = { 15, 50, 85, 120, 155 };
                for (int32_t k = 0; k < 5; ++k) {
                    const char *name = buttons[k] != NULL ? buttons[k] : cbCmp(c, cbInt(0)) == 0 ? "spr_btact" : "spr_bttech";
                    RValue y = cbAdd(cbSub(cbInt(485), CB(BP)), CB(YY));
                    RValue x = cbAdd(cbAdd(CB(XX), CB(XCHUNK)), cbInt(offsets[k]));
                    cbDrawSprite(ctx, cbSprite(ctx, name), CB_AT(BTC, k), x, y);
                }
                /* Spare, then the pacify glow on the second button. */
                static const int globs[2] = { CB_SPAREGLOW, CB_PACIFYGLOW };
                static const char *const glowNames[2] = { "spr_btspare", "spr_bttech" };
                static const int32_t glowOffsets[2] = { 120, 50 };
                for (int k = 0; k < 2; ++k) {
                    if (!(cbCmp(cbGet(ctx, globs[k]), cbInt(1)) == 0 && cbCmp(CB(GC), CB_AT(CHARPOS, ci)) == 0)) continue;
                    RValue angle = cbDiv(CB(TIME), cbReal(6));
                    RValue wave = charboxFn[CBF_SIN](ctx, &angle, 1);
                    RValue alpha = cbAdd(cbReal((GMLReal) 0.4), cbMul(wave, cbReal((GMLReal) 0.4)));
                    RValue_free(&wave);
                    RValue y = cbAdd(cbSub(cbInt(485), CB(BP)), CB(YY));
                    RValue x = cbAdd(cbAdd(CB(XX), CB(XCHUNK)), cbInt(glowOffsets[k]));
                    RValue sprite = cbSprite(ctx, glowNames[k]);
                    RValue ext[9] = { sprite, cbInt(2), x, y, cbInt(1), cbInt(1), cbInt(0), cbInt(16777215), alpha };
                    cbCall(ctx, CBF_SPRITE_EXT, ext, 9);
                    RValue_free(&sprite);
                }
            }

            cbColor(ctx, cbCmp(CB(GC), CB_AT(CHARPOS, ci)) == 0 ? CB(CHARCOLOR) : CB(BCOLOR));
            if (cbCmp(CB(CHARSELECT), CB_AT(CHARPOS, ci)) == 0 || cbCmp(CB(CHARSELECT), cbInt(3)) == 0)
                cbColor(ctx, CB(CHARCOLOR));
            cbRectangle(ctx, cbAdd(CB(XX), CB(XCHUNK)),
                        cbAdd(cbAdd(cbSub(cbSub(cbInt(480), CB(BP)), cbInt(2)), CB(YY)), CB_AT(MMY, ci)),
                        cbAdd(cbAdd(CB(XX), CB(XCHUNK)), cbInt(212)),
                        cbAdd(cbSub(cbInt(480), CB(BP)), CB(YY)));
            cbColor(ctx, cbInt(0));
            cbRectangle(ctx, cbAdd(cbAdd(CB(XX), CB(XCHUNK)), cbInt(2)),
                        cbAdd(cbAdd(cbSub(cbInt(480), CB(BP)), CB(YY)), CB_AT(MMY, ci)),
                        cbAdd(cbAdd(CB(XX), CB(XCHUNK)), cbInt(210)),
                        cbAdd(cbAdd(cbAdd(cbSub(cbInt(480), CB(BP)), CB(YY)), CB_AT(MMY, ci)), cbInt(33)));

            cbSet(ctx, CB_BOFFSET, cbReal(480));
            if (cbCmp(CB(FIGHTING), cbInt(0)) == 0) cbSet(ctx, CB_BOFFSET, cbReal(430));
            if (cbCmp(CB(FIGHTING), cbInt(1)) == 0) cbSet(ctx, CB_BOFFSET, cbReal(336));
#define CB_BOX(dy) cbAdd(cbAdd(cbAdd(CB(BPOFF), CB(BOFFSET)), cbInt(dy)), CB_AT(MMY, ci))
#define CB_COL(dx) cbAdd(cbAdd(CB(XX), cbInt(dx)), CB(XCHUNK))
            static const int32_t faces[3] = { 682, 684, 683 };
            static const char *const nameSprites[3] = { "spr_bnamekris", "spr_bnamesusie", "spr_bnameralsei" };
            for (int32_t k = 0; k < 3; ++k) {
                if (cbCmp(c, cbInt(k)) != 0) continue;
                RValue faces2 = CB(FACEACTION);
                RValue face = GMLArray_getOnArrayRef(&faces2, RValue_toInt32(CB_AT(CHARPOS, ci)));
                cbDrawSprite(ctx, cbInt(faces[k]), face, CB_COL(13), cbAdd(cbAdd(CB(BPOFF), CB(BOFFSET)), CB_AT(MMY, ci)));
                cbDrawSprite(ctx, cbSprite(ctx, nameSprites[k]), cbInt(0), CB_COL(51), CB_BOX(3));
            }
            cbDrawSprite(ctx, cbInt(685), cbInt(0), CB_COL(109), CB_BOX(11));
            cbColor(ctx, cbInt(16777215));
            RValue font = CB(HPFONT);
            cbCall(ctx, CBF_FONT, &font, 1);
            int32_t member = RValue_toInt32(cbAdd(c, cbInt(1)));
            if (cbCmp(cbDiv(CB_AT(HP, member), CB_AT(MAXHP, member)), cbReal((GMLReal) 0.25)) <= 0) cbColor(ctx, cbInt(65535));
            if (cbCmp(CB_AT(HP, member), cbInt(0)) <= 0) cbColor(ctx, cbInt(255));
            RValue align = cbInt(2);
            cbCall(ctx, CBF_HALIGN, &align, 1);
            for (int k = 0; k < 2; ++k) {
                RValue number = k == 0 ? CB_AT(HP, member) : CB_AT(MAXHP, member);
                RValue text = charboxFn[CBF_NEWLINE](ctx, &number, 1);
                RValue draw[3] = { CB_COL(k == 0 ? 160 : 205), CB_BOX(-2), text };
                cbCall(ctx, CBF_TEXT, draw, 3);
                RValue_free(&text);
                if (k == 0) cbDrawSprite(ctx, cbInt(686), cbInt(0), CB_COL(159), CB_BOX(-4));
            }
            align = cbInt(0);
            cbCall(ctx, CBF_HALIGN, &align, 1);
            cbColor(ctx, cbInt(128));
            cbRectangle(ctx, CB_COL(128), CB_BOX(11), CB_COL(203), CB_BOX(19));
            if (cbCmp(CB_AT(HP, member), cbInt(0)) > 0 && cbCmp(CB_AT(MAXHP, member), cbInt(0)) > 0) {
                cbColor(ctx, CB(CHARCOLOR));
                RValue fill = cbMul(cbDiv(CB_AT(HP, member), CB_AT(MAXHP, member)), cbInt(75));
                RValue width = charboxFn[CBF_CEIL](ctx, &fill, 1);
                cbRectangle(ctx, CB_COL(128), CB_BOX(11), cbAdd(cbAdd(cbAdd(CB(XX), CB(XCHUNK)), cbInt(128)), width), CB_BOX(19));
                RValue_free(&width);
            }
#undef CB_BOX
#undef CB_COL
        }
        cbSet(ctx, CB_C, cbAdd(CB(C), cbInt(1)));
    }
#undef CB
#undef CB_AT
    *result = RValue_makeUndefined();
    return true;
}
