/* Asset-free checks through the real registry. Count strdup only in this
 * translation unit's builtin implementation, including its inline helpers. */
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static unsigned inputCopies;
static int trackStructCopies;
static void *structCopies[8];
static unsigned liveStructCopies;
static char *countedStrdup(const char *text) {
    size_t bytes = strlen(text) + 1;
    char *copy = (char *)malloc(bytes);
    assert(copy != NULL);
    memcpy(copy, text, bytes);
    ++inputCopies;
    if (trackStructCopies) {
        assert(liveStructCopies < sizeof(structCopies) / sizeof(structCopies[0]));
        structCopies[liveStructCopies++] = copy;
    }
    return copy;
}
static void countedFree(void *pointer) {
    for (unsigned i = 0; i < liveStructCopies; ++i) {
        if (structCopies[i] == pointer) {
            structCopies[i] = structCopies[--liveStructCopies];
            break;
        }
    }
    free(pointer);
}
/* Count actual whole-string traversals made by the builtin translation unit.
 * Conversion/allocation checks below remain independent of this work bound. */
static size_t lengthScanBytes;
static size_t countedStrlen(const char *text) {
    size_t bytes = strlen(text);
    lengthScanBytes += bytes;
    return bytes;
}
#define strlen countedStrlen
#define strdup countedStrdup
#define free countedFree
#include "../../vm_builtins.c"
#undef free
#undef strdup
#undef strlen

static unsigned checked, allocationFailures, workFailures;
static bool behaviorOnly;

static RValue call(VMContext *ctx, const char *name, RValue *args, int count,
                   unsigned expectedCopies) {
    BuiltinFunc builtin = VM_findBuiltin(ctx, name);
    assert(builtin != NULL);
    inputCopies = 0;
    lengthScanBytes = 0;
    RValue result = builtin(ctx, args, count);
    if (!behaviorOnly && inputCopies != expectedCopies) {
        fprintf(stderr, "%s: expected %u strdup calls, got %u\n",
                name, expectedCopies, inputCopies);
        ++allocationFailures;
    }
    ++checked;
    return result;
}

static void expectString(RValue *result, const char *expected) {
    assert(result->type == RVALUE_STRING);
    assert(result->gmlStackType == GML_TYPE_STRING);
    assert(result->ownsReference);
    assert(result->string != NULL && !strcmp(result->string, expected));
    RValue_free(result);
}

static void expectOrdinal(RValue *result, int expected) {
    assert(result->type == RVALUE_REAL);
    assert(result->gmlStackType == GML_TYPE_DOUBLE);
    assert(result->real == (GMLReal)expected);
    RValue_free(result);
}

typedef struct {
    const char *text;
    int index, length;
    const char *character;
    int ordinal;
    const char *copy;
} StringCase;

/* Literal expectations preserve the current decoder, including malformed UTF-8
 * and its different byte ranges for char_at and copy. */
static const StringCase stringCases[] = {
    {"ABC", -3, 2, "A", 65, "AB"},
    {"ABC", 0, 2, "A", 65, "AB"},
    {"ABC", 1, 2, "A", 65, "AB"},
    {"ABC", 2, 1, "B", 66, "B"},
    {"ABC", 3, 9, "C", 67, "C"},
    {"ABC", 4, 2, "", -1, ""},
    {"ABC", 99, 2, "", -1, ""},
    {"ABC", 1, 0, "A", 65, ""},
    {"ABC", 1, -2, "A", 65, ""},
    {"ABC", 1, 99, "A", 65, "ABC"},
    {"", 1, 1, "", -1, ""},
    {"", 0, 1, "", -1, ""},
    {"", -3, 0, "", -1, ""},
    {NULL, 1, 1, "", -1, ""},
    {NULL, 0, 1, "", -1, ""},
    {NULL, -3, -1, "", -1, ""},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 0, 2, "A", 65, "A\xc3\xa9"},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 2, 2, "\xc3\xa9", 233, "\xc3\xa9\xe2\x82\xac"},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 3, 1, "\xe2\x82\xac", 8364, "\xe2\x82\xac"},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 4, 1, "\xf0\x9f\x98\x80", 65533, "\xf0\x9f\x98\x80"},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 5, 99, "Z", 90, "Z"},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 6, 1, "", -1, ""},
    {"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80Z", 99, 1, "", -1, ""},
    {"\xe2\x82X", 1, 1, "\xe2", 65533, "\xe2\x82"},
    {"\xe2\x82X", 2, 1, "X", 88, "X"},
    {"\xe2\x82", 1, 1, "\xe2", 65533, "\xe2\x82"},
    {"\x80\x81Z", 1, 1, "\x80", 65533, "\x80\x81"},
    {"\x80\x81Z", 2, 1, "Z", 90, "Z"},
    {"\xc0\xafQ", 1, 1, "\xc0\xaf", 47, "\xc0\xaf"},
    {"\xed\xa0\x80Q", 1, 1, "\xed\xa0\x80", 55296, "\xed\xa0\x80"},
    {"\xffQ", 1, 1, "\xff", 65533, "\xff"},
    {"A", INT_MAX, 1, "", -1, ""},
    {"\xf0\x9f\x98", 1, 1, "\xf0", 65533, "\xf0\x9f\x98"},
    {"\xf4\xbf\xbf\xbfQ", 1, 1, "\xf4\xbf\xbf\xbf", 65533, "\xf4\xbf\xbf\xbf"},
    {"\x80\x81", 2, 1, "", -1, ""},
    {"\xc2Z", 1, 1, "\xc2", 65533, "\xc2"},
    {"A\0B", 2, 1, "", -1, ""},
};

static RValue ownedInput(const char *text) {
    return text ? RValue_makeOwnedString(strdup(text)) : RValue_makeString(NULL);
}

static void testStrings(VMContext *ctx) {
    for (size_t i = 0; i < sizeof(stringCases) / sizeof(stringCases[0]); ++i) {
        const StringCase *test = &stringCases[i];
        RValue args[] = {ownedInput(test->text), RValue_makeInt32(test->index),
                         RValue_makeInt32(test->length)};
        RValue result = call(ctx, "string_copy", args, 3, test->copy[0] ? 0 : 1);
        RValue_free(&args[0]);
        expectString(&result, test->copy);

        args[0] = ownedInput(test->text);
        result = call(ctx, "string_char_at", args, 2, test->character[0] ? 0 : 1);
        RValue_free(&args[0]);
        expectString(&result, test->character);

        args[0] = ownedInput(test->text);
        result = call(ctx, "string_ord_at", args, 2, 0);
        RValue_free(&args[0]);
        expectOrdinal(&result, test->ordinal);
    }
}

static void testConversion(VMContext *ctx, RValue input, const char *text) {
    RValue args[] = {input, RValue_makeInt32(1), RValue_makeInt32(99)};
    RValue result = call(ctx, "string_copy", args, 3, 1);
    expectString(&result, text);
    char first[] = {text[0], '\0'};
    result = call(ctx, "string_char_at", args, 2, 1);
    expectString(&result, first);
    result = call(ctx, "string_ord_at", args, 2, 1);
    expectOrdinal(&result, (unsigned char)text[0]);
    /* Exercise cleanup of converted temporaries on each post-conversion exit. */
    args[1] = RValue_makeInt32(99);
    result = call(ctx, "string_copy", args, 3, 2);
    expectString(&result, "");
    result = call(ctx, "string_char_at", args, 2, 2);
    expectString(&result, "");
    result = call(ctx, "string_ord_at", args, 2, 1);
    expectOrdinal(&result, -1);
}

static void testMissingArguments(VMContext *ctx) {
    RValue args[] = {RValue_makeString("ABC"), RValue_makeInt32(1)};
    for (int count = 0; count < 3; ++count) {
        RValue result = call(ctx, "string_copy", args, count, 1);
        expectString(&result, "");
        if (count < 2) {
            result = call(ctx, "string_char_at", args, count, 1);
            expectString(&result, "");
            result = call(ctx, "string_ord_at", args, count, 0);
            expectOrdinal(&result, -1);
        }
    }
}

static void testAllocationProbes(VMContext *ctx) {
    const char *names[] = {"string_copy", "string_char_at", "string_ord_at"};
    RValue args[] = {ownedInput("borrow me"), RValue_makeInt32(1), RValue_makeInt32(3)};
    for (int i = 0; i < 3; ++i) {
        RValue result = call(ctx, names[i], args, i == 0 ? 3 : 2, 0);
        printf("%s string input: %u strdup calls\n", names[i], inputCopies);
        RValue_free(&result);
    }
    RValue_free(&args[0]);
    args[0] = RValue_makeReal(123);
    for (int i = 0; i < 3; ++i) {
        RValue result = call(ctx, names[i], args, i == 0 ? 3 : 2, 1);
        printf("%s numeric input: %u strdup calls\n", names[i], inputCopies);
        RValue_free(&result);
    }
}

/* The original length-bounded traversal is a scalar behavioral oracle. The
 * unchanged decoder intentionally accepts overlong sequences and surrogates. */
static void oracleCharacter(const char *text, int index, char out[5]) {
    int length = (int)strlen(text);
    int position = index - 1;
    if (position < 0) position = 0;
    int start = TextUtils_utf8AdvanceCodepoints(text, length, position);
    int end = start;
    if (start < length) TextUtils_decodeUtf8(text, length, &end);
    memcpy(out, text + start, (size_t)(end - start));
    out[end - start] = '\0';
}

static void testByteSequences(VMContext *ctx) {
    const unsigned char bytes[] = {0, 'A', 0x80, 0xbf, 0xc0, 0xc2,
                                   0xe0, 0xed, 0xf0, 0xf4, 0xff};
    const int indices[] = {-3, 0, 1, 2, 3, 4, 9, INT_MAX};
    for (size_t a = 0; a < sizeof(bytes); ++a)
    for (size_t b = 0; b < sizeof(bytes); ++b)
    for (size_t c = 0; c < sizeof(bytes); ++c) {
        char text[] = {(char)bytes[a], (char)bytes[b], (char)bytes[c], 0};
        char before[sizeof(text)];
        memcpy(before, text, sizeof(text));
        for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
            char expected[5];
            oracleCharacter(text, indices[i], expected);
            RValue args[] = {RValue_makeString(text), RValue_makeInt32(indices[i])};
            RValue result = call(ctx, "string_char_at", args, 2, expected[0] ? 0 : 1);
            assert(result.string != text);
            assert(!memcmp(text, before, sizeof(text)));
            expectString(&result, expected);
        }
    }
}

static void testTraversalWork(VMContext *ctx) {
    char text[2049];
    memset(text, 'A', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    const int indices[] = {-3, 0, 1, 1024, 2048, 2049, INT_MAX};
    for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
        RValue args[] = {RValue_makeString(text), RValue_makeInt32(indices[i])};
        bool inRange = indices[i] <= 2048;
        RValue result = call(ctx, "string_char_at", args, 2, inRange ? 0 : 1);
        size_t scanned = lengthScanBytes;
        expectString(&result, inRange ? "A" : "");
        printf("char_at index %d: %zu whole-length bytes\n", indices[i], scanned);
        if (!behaviorOnly && scanned != 0) {
            fprintf(stderr, "char_at index %d: expected no whole-length scan, got %zu bytes\n",
                    indices[i], scanned);
            ++workFailures;
        }
    }
    /* Borrow an embedded-NUL buffer, including bytes beyond that NUL. */
    char embedded[] = {'A', 0, (char)0xf0, (char)0x9f, (char)0x98, (char)0x80, 0};
    RValue args[] = {RValue_makeString(embedded), RValue_makeInt32(1)};
    RValue result = call(ctx, "string_char_at", args, 2, 0);
    embedded[0] = 'B';
    expectString(&result, "A");
    args[1] = RValue_makeInt32(2);
    result = call(ctx, "string_char_at", args, 2, 1);
    expectString(&result, "");
}

/* A read-only search must borrow both existing strings, preserve codepoint
 * indexing, and convert nonstrings without taking ownership of caller data. */
static void testStringPosition(VMContext *ctx) {
    static const struct { const char *needle, *text; int want; } cases[] = {
        {"ABC", "ABCABC", 1}, {"BC", "ABCABC", 2}, {"Z", "ABC", 0},
        {"", "ABC", 1}, {"", "", 1}, {"A", "", 0},
        {NULL, "ABC", 1}, {"A", NULL, 0}, {NULL, NULL, 1},
        {"Z", "A\xc3\xa9Z", 3}, {"\xc3\xa9", "A\xc3\xa9Z", 2},
        {"Q", "\xf0\x9f\x98\x80Q", 2},
        {"Z", "\x80\x81Z", 1}, {"A", "A\0B", 1}, {"B", "A\0B", 0},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        for (int owned=0;owned<2;++owned) {
            RValue args[] = {owned ? ownedInput(cases[i].needle) : RValue_makeString(cases[i].needle),
                             owned ? ownedInput(cases[i].text) : RValue_makeString(cases[i].text)};
            const char *needle=args[0].string, *text=args[1].string;
            RValue result=call(ctx,"string_pos",args,2,0);
            expectOrdinal(&result,cases[i].want);
            assert(args[0].string==needle && args[1].string==text);
            if (needle) assert(!strcmp(needle,cases[i].needle));
            if (text) assert(!strcmp(text,cases[i].text));
            RValue_free(&args[0]);RValue_free(&args[1]);
        }
    }
    RValue alias=ownedInput("same string"), args[]={alias,alias};
    RValue result=call(ctx,"string_pos",args,2,0);
    expectOrdinal(&result,1);RValue_free(&alias);
    RValue convert[][2] = {
        {RValue_makeInt32(42),RValue_makeString("1420")},
        {RValue_makeString("2"),RValue_makeInt32(42)},
        {RValue_makeInt32(42),RValue_makeInt32(1420)},
        {RValue_makeBool(true),RValue_makeString("010")},
        {RValue_makeUndefined(),RValue_makeString("is undefined")},
    };
    const int want[]={2,2,2,2,4};
    for (unsigned i=0;i<sizeof(want)/sizeof(want[0]);++i) {
        result=call(ctx,"string_pos",convert[i],2,i==2 ? 2 : 1);
        expectOrdinal(&result,want[i]);
    }
    for (int count=0;count<2;++count) {
        result=call(ctx,"string_pos",NULL,count,0);expectOrdinal(&result,0);
    }
}

static void testStructNames(VMContext *ctx) {
    Instance instance = {0};
    instance.objectIndex = STRUCT_OBJECT_INDEX;
    VM_structSet(ctx, &instance, "answer", RValue_makeReal(42), -1);
    VM_structSet(ctx, &instance, "7", RValue_makeReal(7), -1);
    BuiltinFunc get = VM_findBuiltin(ctx, "struct_get");
    BuiltinFunc set = VM_findBuiltin(ctx, "struct_set");
    assert(get != NULL && set != NULL);
    RValue args[] = {RValue_makeStructWeak(&instance), RValue_makeString("answer"), RValue_makeUndefined()};
    trackStructCopies = 1;
    for (int count = 0; count < 2; ++count) {
        RValue missing = get(ctx, NULL, count);
        assert(missing.type == RVALUE_UNDEFINED && liveStructCopies == 0);
    }
    for (int count = 0; count < 3; ++count) {
        RValue missing = set(ctx, NULL, count);
        assert(missing.type == RVALUE_UNDEFINED && liveStructCopies == 0);
    }
    RValue result = get(ctx, args, 2);
    assert(result.type == RVALUE_REAL && result.real == 42);
    assert(liveStructCopies == 0);
    RValue_free(&result);
    args[1] = RValue_makeReal(7);
    result = get(ctx, args, 2);
    assert(result.type == RVALUE_REAL && result.real == 7);
    assert(liveStructCopies == 0);
    args[1] = RValue_makeString("missing");
    result = get(ctx, args, 2);
    assert(result.type == RVALUE_UNDEFINED && liveStructCopies == 0);
    args[0] = RValue_makeStructWeak(NULL);
    result = get(ctx, args, 2);
    assert(result.type == RVALUE_UNDEFINED && liveStructCopies == 0);
    args[0] = RValue_makeStructWeak(&instance);
    args[1] = RValue_makeString("value");
    args[2] = RValue_makeOwnedString(strdup("kept"));
    result = set(ctx, args, 3);
    assert(result.type == RVALUE_UNDEFINED && liveStructCopies == 0);
    RValue_free(&args[2]);
    result = get(ctx, args, 2);
    assert(result.type == RVALUE_STRING && result.ownsReference);
    IntRValueHashMap_freeAllValues(&instance.selfVars);
    assert(!strcmp(result.string, "kept"));
    RValue_free(&result);
    assert(liveStructCopies == 0);
    trackStructCopies = 0;
    for (ptrdiff_t i = 0; i < shlen(ctx->varNameMap); ++i) free(ctx->varNameMap[i].key);
    shfree(ctx->varNameMap);
    puts("PASS: struct builtin arity, value ownership and temporary-name cleanup");
}

int main(int argc, char **argv) {
    DataWin data = {0};
    Runner runner = {0};
    VMContext ctx = {0};
    behaviorOnly = argc == 2 && !strcmp(argv[1], "--behavior-only");
#ifdef GAME_DELTARUNE
    data.gen8.wadVersion = 17;
#else
    data.gen8.wadVersion = 16;
#endif
    runner.dataWin = &data;
    ctx.dataWin = &data;
    ctx.runner = &runner;
    VMBuiltins_registerAll(&ctx);
    testStructNames(&ctx);
    testStrings(&ctx);
    testConversion(&ctx, RValue_makeReal(42), "42");
    testConversion(&ctx, RValue_makeReal(3.5), "3.50");
    testConversion(&ctx, RValue_makeInt32(-23), "-23");
    testConversion(&ctx, RValue_makeInt64(123), "123");
    testConversion(&ctx, RValue_makeBool(true), "1");
    testConversion(&ctx, RValue_makeBool(false), "0");
    testConversion(&ctx, RValue_makeUndefined(), "undefined");
    testMissingArguments(&ctx);
    testAllocationProbes(&ctx);
    testByteSequences(&ctx);
    testTraversalWork(&ctx);
    testStringPosition(&ctx);
    shfree(ctx.builtinMap);
    printf("WAD %u: %u behavior/ownership checks, %u allocation failures, %u work failures (%s)\n",
           (unsigned)data.gen8.wadVersion, checked, allocationFailures, workFailures,
           behaviorOnly ? "behavior only" : "allocation assertions enabled");
    return allocationFailures == 0 && workFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
