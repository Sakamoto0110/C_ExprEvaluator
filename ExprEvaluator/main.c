/*
 * ExprEvaluator - step-by-step visual evaluator for + - * / ^ expressions,
 * plus $ as a prefix sqrt ($16, $(7+9), $$16).
 *
 * Not built for speed: every phase moves needles over a raw char buffer and
 * stops after each move so the process can be watched.
 *
 * Usage:
 *   ExprEvaluator [options]   reads one expression per line from stdin
 *
 * Options (defaults come from ExprEvaluator.h):
 *   --trace                        plain step log: no VT, no key waits
 *   --delay=<ms|max>               initial autoplay delay; max = no delay
 *   --depth-content=on|off         d=N rows show their chars, or only '^'
 *   --depth-hide-brackets=on|off   blank out [ ] in the d=N rows
 *   --help, -h                     print the options and exit
 *
 * "exit" ends the loop with 0. Any failure prints "Failure. Exit code: N"
 * and ends the loop returning N (an ExprError value).
 *
 * Keys: [space] next step  [enter] finish phase  [a] autoplay
 *       [+]/[-] autoplay faster/slower  [esc] quit
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ExprEvaluator.h"

#define LABEL_WIDTH 6                   /* longest label: "result" */
#define TEXT_COL (LABEL_WIDTH + 6)      /* " " + label + "  " + "| " -> first text column */

#define TRY(call) do { ExprError tryErr_ = (call); if (tryErr_ != EXPR_OK) return tryErr_; } while (0)

/* EXPR_STR(EXPR_MAX_INPUT) -> "1024", so messages follow the header limits */
#define EXPR_STR_(x) #x
#define EXPR_STR(x) EXPR_STR_(x)

static const Rgb C_TEXT     = { 200, 200, 200 };
static const Rgb C_DIM      = { 110, 110, 120 };
static const Rgb C_LABEL    = { 140, 150, 170 };
static const Rgb C_TITLE    = {  97, 175, 239 };
static const Rgb C_OPERAND  = {  97, 175, 239 };
static const Rgb C_BRACKET  = {  80, 105, 140 };
static const Rgb C_OPERATOR = { 229, 192, 123 };
static const Rgb C_NEEDLE   = { 255,  85,  85 };
static const Rgb C_NEEDLE2  = { 255, 170,   0 };
static const Rgb C_OK       = { 152, 195, 121 };
static const Rgb C_ERROR    = { 224, 108, 117 };
static const Rgb C_BG_SPACE = {  55,  55,  60 };
static const Rgb C_BG_LEFT  = {  40,  60, 100 };
static const Rgb C_BG_RIGHT = {  90,  60,  30 };
static const Rgb C_BG_GROUP = {  45,  80,  45 };

static const Rgb DEPTH_COLORS[] =
{
    { 198, 120, 221 }, {  86, 182, 194 }, { 152, 195, 121 },
    { 224, 108, 117 }, { 209, 154, 102 }, {  97, 175, 239 }
};

static const char* const PHASE_NAMES[PHASE_COUNT] =
{
    "copy const char* into a mutable buffer",
    "normalize (remove whitespace)",
    "explicit operands",
    "explicit expressions",
    "needle walk: expression depth",
    "push innermost expression, replace with \"\"",
    "text drained to '\\0'",
    "pop first expression",
    "convert, operate, feed the next pop",
    "result"
};

static const char* const NEEDLE_NAMES[] =
{
    "", "operand", "unary operator", "operator", "needleExpressionStart",
    "needleExpressionEnd", "whitespace", "'\\0'"
};

/* ========================================================================= */
/* Character classes                                                         */
/* ========================================================================= */

static int IsSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int IsDigitChar(char c) { return c >= '0' && c <= '9'; }
static int IsNumberChar(char c) { return IsDigitChar(c) || c == '.'; }

static int IsBinaryOperator(char c)
{
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '^';
}

/* '$' is sqrt: a prefix operator on the operand or group right after it. */
static int IsUnaryOperator(char c) { return c == '$'; }
static int IsOperatorChar(char c) { return IsBinaryOperator(c) || IsUnaryOperator(c); }

static NeedleKind ClassifyChar(char c)
{
    if (IsSpaceChar(c)) return NEEDLE_WHITESPACE;
    if (IsNumberChar(c) || c == '[' || c == ']') return NEEDLE_OPERAND;
    if (IsUnaryOperator(c)) return NEEDLE_UNARY;
    if (IsBinaryOperator(c)) return NEEDLE_OPERATOR;
    if (c == '(') return NEEDLE_EXPR_START;
    if (c == ')') return NEEDLE_EXPR_END;
    if (c == '\0') return NEEDLE_TERMINATOR;
    return NEEDLE_NONE;
}

static const char* ExprErrorString(ExprError err)
{
    switch (err)
    {
    case EXPR_OK:                       return "ok";
    case EXPR_ERR_NULL_INPUT:           return "input is NULL";
    case EXPR_ERR_INPUT_TOO_LONG:       return "input longer than " EXPR_STR(EXPR_MAX_INPUT) " chars (not truncated)";
    case EXPR_ERR_EMPTY:                return "expression is empty";
    case EXPR_ERR_OUT_OF_MEMORY:        return "out of memory";
    case EXPR_ERR_INVALID_CHAR:         return "invalid character";
    case EXPR_ERR_INVALID_NUMBER:       return "invalid number";
    case EXPR_ERR_UNEXPECTED_TOKEN:     return "unexpected token";
    case EXPR_ERR_UNBALANCED_PARENS:    return "unbalanced parentheses";
    case EXPR_ERR_UNSUPPORTED_UNARY:    return "unary '-' must be followed by an operand";
    case EXPR_ERR_BUFFER_OVERFLOW:      return "working buffer overflow";
    case EXPR_ERR_STACK_OVERFLOW:       return "expression stack overflow";
    case EXPR_ERR_STACK_ENTRY_TOO_LONG: return "stack entry does not fit in " EXPR_STR(EXPR_STACK_ENTRY_SIZE) " chars";
    case EXPR_ERR_STACK_UNDERFLOW:      return "value stack underflow";
    case EXPR_ERR_DIVISION_BY_ZERO:     return "division by zero";
    case EXPR_ERR_MALFORMED_ENTRY:      return "malformed stack entry";
    case EXPR_ERR_ABORTED:              return "aborted by user";
    case EXPR_ERR_MATH_DOMAIN:          return "result is not a finite number (sqrt of negative, invalid power, overflow)";
    case EXPR_ERR_INVALID_ARGUMENT:     return "invalid command-line argument (see --help)";
    }
    return "unknown error";
}

/* ========================================================================= */
/* Console output                                                            */
/* ========================================================================= */

static void EnableVt(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;

    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
        SetConsoleMode(out, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static void ConsoleSize(int* cols, int* rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    *cols = 120;
    *rows = 30;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
    {
        *cols = info.srWindow.Right - info.srWindow.Left + 1;
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    if (*cols < 40) *cols = 40;
    if (*rows < 20) *rows = 20;
}

static void Fg(Rgb c) { printf(vtForegroundColorA, c.r, c.g, c.b); }
static void Bg(Rgb c) { printf(vtBackgroundColorA, c.r, c.g, c.b); }
static void MoveTo(int row, int col) { printf(vtCurMOVEA, row, col); }

static Rgb DepthColor(int depth)
{
    const int count = (int)(sizeof DEPTH_COLORS / sizeof DEPTH_COLORS[0]);
    if (depth < 1) depth = 1;
    return DEPTH_COLORS[(depth - 1) % count];
}

static int SameRgb(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static void PenFg(Pen* p, Rgb c)
{
    if (p->hasFg && SameRgb(p->fg, c)) return;
    Fg(c);
    p->fg = c;
    p->hasFg = 1;
}

/* NULL selects the terminal's default background. */
static void PenBg(Pen* p, const Rgb* c)
{
    if (c == NULL)
    {
        if (p->bgState == PEN_BG_DEFAULT) return;
        fputs(vtBgDefaultA, stdout);
        p->bgState = PEN_BG_DEFAULT;
        return;
    }
    if (p->bgState == PEN_BG_COLOR && SameRgb(p->bg, *c)) return;
    Bg(*c);
    p->bg = *c;
    p->bgState = PEN_BG_COLOR;
}

/* Advances the paren/bracket state over c and returns the depth c belongs to:
   '(' and ')' belong to the depth they open/close. */
static int ScanChar(char c, int* depth, int* inBracket)
{
    if (c == '(') return ++*depth;
    if (c == ')') return (*depth)--;
    if (c == '[') *inBracket = 1;
    else if (c == ']') *inBracket = 0;
    return *depth;
}

static Rgb CharColor(char c, int depth, int inBracket)
{
    if (c == '(' || c == ')') return DepthColor(depth);
    if (c == '[' || c == ']') return C_BRACKET;
    if (inBracket || IsNumberChar(c)) return C_OPERAND;
    if (IsOperatorChar(c)) return C_OPERATOR;
    return C_TEXT;
}

static const Rgb* HighlightAt(const Highlight* hl, int hlCount, size_t i)
{
    int h;
    for (h = 0; h < hlCount; ++h)
        if (i >= hl[h].begin && i < hl[h].end) return &hl[h].color;
    return NULL;
}

/* Prints at most maxCols chars of s[0..n), control chars as blanks. */
static void PrintClipped(const char* s, size_t n, size_t maxCols)
{
    size_t i;
    for (i = 0; i < n && i < maxCols; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        putchar(c < 32 ? ' ' : c);
    }
}

/* First index of a `width`-wide window over [0, len) that keeps `focus` visible. */
static size_t WindowStart(size_t len, size_t focus, size_t width)
{
    size_t start;
    if (len <= width) return 0;
    start = focus > width / 2 ? focus - width / 2 : 0;
    if (start + width > len) start = len - width;
    return start;
}

/* " label   | " - left aligned, so every row's separator and text line up. */
static void PrintLabel(int row, const char* label, Rgb color, char separator)
{
    MoveTo(row, 1);
    Fg(color);
    printf(" %-*.*s  ", LABEL_WIDTH, LABEL_WIDTH, label != NULL ? label : "");
    Fg(C_DIM);
    putchar(separator);
    putchar(' ');
}

static void DrawTextRow(int row, const char* label, const char* s, size_t len,
                        size_t start, size_t width, const Highlight* hl, int hlCount)
{
    int depth = 0, inBracket = 0;
    size_t i;
    Pen pen;

    PrintLabel(row, label, C_LABEL, start > 0 ? '<' : '|');
    memset(&pen, 0, sizeof pen);

    /* state carried in from the part left of the window */
    for (i = 0; i < start && i < len; ++i)
        (void)ScanChar(s[i], &depth, &inBracket);

    for (i = start; i < len && i < start + width; ++i)
    {
        char c = s[i];
        int cd = ScanChar(c, &depth, &inBracket);
        const Rgb* bg = HighlightAt(hl, hlCount, i);

        if (bg == NULL && IsSpaceChar(c)) bg = &C_BG_SPACE;
        PenBg(&pen, bg);
        PenFg(&pen, CharColor(c, cd, inBracket));
        putchar((unsigned char)c < 32 ? ' ' : c);
    }
    fputs(vtBgDefaultA, stdout);

    if (start + width < len)
    {
        MoveTo(row, TEXT_COL + (int)width);
        Fg(C_DIM);
        putchar('>');
    }
}

static void DrawCaret(int row, size_t pos, size_t start, size_t width, Rgb color)
{
    if (pos < start || pos - start >= width) return;
    MoveTo(row, TEXT_COL + (int)(pos - start));
    Fg(color);
    putchar('^');
}

static void DrawNeedleRow(int row, size_t start, size_t width, const Frame* f)
{
    if (f->hasNeedle && f->needle.pos >= start)
    {
        size_t last = f->needle.pos;
        size_t col;
        const char* name = NEEDLE_NAMES[f->needle.kind];

        if (f->hasNeedle2 && f->needle2.pos > last) last = f->needle2.pos;
        col = last - start + 2;
        if (col < width)
        {
            MoveTo(row, TEXT_COL + (int)col);
            Fg(C_DIM);
            PrintClipped(name, strlen(name), width - col);
        }
    }
    if (f->hasNeedle2) DrawCaret(row, f->needle2.pos, start, width, C_NEEDLE2);
    if (f->hasNeedle) DrawCaret(row, f->needle.pos, start, width, C_NEEDLE);
}

/* Prints only the chars sitting at depth d, each in its own column. Every
   char of the text lands on exactly one row, so the cost per frame is about
   one extra copy of the text row plus a cursor move per gap. */
static void DrawDepthContent(int row, const Frame* f, size_t lim, size_t start, size_t width,
                             int d, int depth, int inBracket, int hideBrackets)
{
    size_t i, col = 0;      /* cursor column, relative to TEXT_COL */
    Pen pen;

    memset(&pen, 0, sizeof pen);
    for (i = start; i < lim && i < start + width; ++i)
    {
        char c = f->text[i];
        int cd = ScanChar(c, &depth, &inBracket);

        if (cd != d) continue;
        if (i - start != col) MoveTo(row, TEXT_COL + (int)(i - start));
        PenBg(&pen, HighlightAt(f->hl, f->hlCount, i));
        col = i - start + 1;
        if (hideBrackets && (c == '[' || c == ']'))
        {
            putchar(' ');   /* keeps the column, and skips a color switch */
            continue;
        }
        PenFg(&pen, CharColor(c, cd, inBracket));
        putchar(c);
    }
    fputs(vtBgDefaultA, stdout);
}

/* One row per depth: the chars at that depth, or a caret under each of its
   '(' and ')' (Viz.depthContent). */
static int DrawDepthRows(const Viz* v, int row, int maxRows, const Frame* f, size_t start, size_t width)
{
    size_t lim = f->depthUpto < f->textLen ? f->depthUpto : f->textLen;
    int maxDepth = 0, depth = 0, d, shown;
    int depthAtStart = 0, inBracketAtStart = 0;
    size_t i;

    for (i = 0; i < lim; ++i)
    {
        if (f->text[i] == '(') { if (++depth > maxDepth) maxDepth = depth; }
        else if (f->text[i] == ')') --depth;
    }

    if (maxRows < 1) maxRows = 1;
    shown = maxDepth < maxRows ? maxDepth : maxRows;

    if (v->depthContent)
    {
        for (i = 0; i < start && i < lim; ++i)
            (void)ScanChar(f->text[i], &depthAtStart, &inBracketAtStart);
    }

    for (d = 1; d <= shown; ++d, ++row)
    {
        char label[16];

        snprintf(label, sizeof label, "d=%d", d);
        PrintLabel(row, label, DepthColor(d), '|');
        if (v->depthContent)
        {
            DrawDepthContent(row, f, lim, start, width, d, depthAtStart, inBracketAtStart, v->hideBrackets);
            continue;
        }

        depth = 0;
        for (i = 0; i < lim; ++i)
        {
            if (f->text[i] == '(')
            {
                if (++depth == d) DrawCaret(row, i, start, width, DepthColor(d));
            }
            else if (f->text[i] == ')')
            {
                if (depth == d) DrawCaret(row, i, start, width, DepthColor(d));
                --depth;
            }
        }
    }
    if (maxDepth > shown)
    {
        MoveTo(row++, TEXT_COL);
        Fg(C_DIM);
        printf("... %d deeper level(s) not shown", maxDepth - shown);
    }
    return row;
}

static void DrawExprEntry(int row, const ExprStack* es, size_t idx, long mark, size_t colW)
{
    int marked = mark >= 0 && (size_t)mark == idx;

    MoveTo(row, 2);
    Fg(marked ? C_NEEDLE : C_DIM);
    printf("%c[%3zu] ", marked ? '>' : ' ', idx);
    Fg(marked ? C_OPERATOR : C_TEXT);
    PrintClipped(es->items[idx], strlen(es->items[idx]), colW);
}

static void DrawPanels(int row, int lastRow, int cols, const Viz* v, const Frame* f)
{
    const ExprStack* es = v->exprStack;
    const ValueStack* vs = v->valueStack;
    int half = cols / 2;
    int avail, r;
    size_t n, k, shownTop, colW = (size_t)(half - 10);

    MoveTo(row, 2);
    Fg(C_LABEL);
    printf("expr stack (%zu, top first)", es->count - es->base);
    MoveTo(row, half + 1);
    printf("value stack (%zu, top first)", vs->count);
    ++row;

    avail = lastRow - row + 1;
    if (avail < 1) return;

    /* expr stack: top entries, and always the bottom one (the next pop) */
    n = es->count - es->base;
    r = row;
    if (n == 0)
    {
        MoveTo(r, 2);
        Fg(C_DIM);
        fputs("(empty)", stdout);
    }
    else if (n <= (size_t)avail || avail < 3)
    {
        for (k = es->count; k > es->base && r <= lastRow; --k)
            DrawExprEntry(r++, es, k - 1, f->exprMark, colW);
    }
    else
    {
        shownTop = (size_t)avail - 2;
        for (k = 0; k < shownTop; ++k)
            DrawExprEntry(r++, es, es->count - 1 - k, f->exprMark, colW);
        MoveTo(r++, 2);
        Fg(C_DIM);
        printf("  ... %zu more", n - shownTop - 1);
        DrawExprEntry(r, es, es->base, f->exprMark, colW);
    }

    /* value stack */
    r = row;
    if (vs->count == 0)
    {
        MoveTo(r, half + 1);
        Fg(C_DIM);
        fputs("(empty)", stdout);
    }
    for (k = vs->count; k > 0 && r <= lastRow; --k)
    {
        size_t idx = k - 1;
        int marked = f->valueMark >= 0 && (size_t)f->valueMark == idx;

        MoveTo(r, half + 1);
        if (r == lastRow && idx > 0)
        {
            Fg(C_DIM);
            printf("  ... %zu more", k);
            break;
        }
        Fg(marked ? C_NEEDLE : C_DIM);
        printf("%c[%3zu] ", marked ? '>' : ' ', idx);
        Fg(marked ? C_OK : C_TEXT);
        printf("%.10g", vs->items[idx]);
        ++r;
    }
}

static void Render(const Viz* v, const Frame* f)
{
    int cols, rows, row;
    size_t width;

    ConsoleSize(&cols, &rows);
    width = (size_t)(cols - TEXT_COL - 1);

    fputs(vtResetA vtClearA, stdout);

    MoveTo(1, 1);
    Fg(C_TITLE);
    fputs(" ExprEvaluator", stdout);
    Fg(C_DIM);
    printf("  |  step %lu  |  phase %d/9  ", v->step, (int)v->phase);
    Fg(C_TEXT);
    fputs(PHASE_NAMES[v->phase], stdout);

    PrintLabel(2, "input", C_LABEL, '|');
    PrintClipped(v->input, strlen(v->input), width);

    row = 4;
    if (f->text != NULL)
    {
        size_t start = WindowStart(f->textLen + 1, f->hasNeedle ? f->needle.pos : 0, width);

        DrawTextRow(row++, f->label, f->text, f->textLen, start, width, f->hl, f->hlCount);
        DrawNeedleRow(row++, start, width, f);
        if (f->depthRows)
        {
            int reserved = 4 + (v->phase >= PHASE_STACK ? 7 : 0);
            row = DrawDepthRows(v, row, rows - row - reserved, f, start, width);
        }
    }
    if (f->text2 != NULL)
    {
        size_t start2 = WindowStart(f->text2Len + 1, f->text2Len, width);
        ++row;
        DrawTextRow(row++, f->label2, f->text2, f->text2Len, start2, width, NULL, 0);
    }
    if (v->phase >= PHASE_STACK && v->exprStack != NULL)
        DrawPanels(row + 1, rows - 3, cols, v, f);

    MoveTo(rows - 1, 1);
    Fg(f->isError ? C_ERROR : C_OK);
    fputs(" > ", stdout);
    if (f->message != NULL) PrintClipped(f->message, strlen(f->message), (size_t)(cols - 4));

    {
        char footer[EXPR_MSG_SIZE];
        char speed[16];
        int n;

        if (v->autoDelayMs == 0) strcpy(speed, "max");
        else snprintf(speed, sizeof speed, "%dms", v->autoDelayMs);
        n = snprintf(footer, sizeof footer,
                     " [space] step  [enter] finish phase  [a] autoplay  [+/-] speed: %s  [esc] quit",
                     speed);
        size_t used = n > 0 ? (size_t)n : 0;
        size_t room = (size_t)(cols - 1);   /* never touch the last column: it can scroll */

        MoveTo(rows, 1);
        Fg(C_DIM);
        PrintClipped(footer, used, room);
        if (v->mode == STEP_AUTO && used < room)
        {
            static const char autoTag[] = "   AUTO (other keys pause)";
            Fg(C_NEEDLE2);
            PrintClipped(autoTag, sizeof autoTag - 1, room - used);
        }
    }

    fputs(vtResetA, stdout);
    fflush(stdout);
}

static void TracePrint(const Viz* v, const Frame* f)
{
    printf("%5lu  P%d  %s%s\n", v->step, (int)v->phase,
           f->isError ? "ERROR: " : "", f->message != NULL ? f->message : "");
    if (f->text != NULL)
    {
        printf("             %-5s \"", f->label != NULL ? f->label : "");
        fwrite(f->text, 1, f->textLen, stdout);
        putchar('"');
        if (f->hasNeedle) printf("  needle@%zu", f->needle.pos);
        if (f->hasNeedle2) printf("  needle2@%zu", f->needle2.pos);
        putchar('\n');
    }
    if (f->text2 != NULL)
    {
        printf("             %-5s \"", f->label2 != NULL ? f->label2 : "");
        fwrite(f->text2, 1, f->text2Len, stdout);
        fputs("\"\n", stdout);
    }
}

/* ========================================================================= */
/* Stepping                                                                  */
/* ========================================================================= */

static int ReadKey(void)
{
    int c = _getch();
    if (c == 0 || c == 0xE0)
    {
        (void)_getch();     /* swallow the second byte of arrow/function keys */
        return 0;
    }
    return c;
}

/* [+] (or [=], same key unshifted) faster, [-] slower. Going below the
   minimum switches to 0 = full speed; [-] from there resumes at the minimum.
   Returns 1 if handled. */
static int AdjustSpeed(Viz* v, int key)
{
    int delay = v->autoDelayMs;

    if (key == '+' || key == '=')
    {
        delay = delay * 2 / 3;
        if (delay < EXPR_AUTO_DELAY_MIN_MS) delay = 0;
    }
    else if (key == '-')
    {
        delay = delay == 0 ? EXPR_AUTO_DELAY_MIN_MS : delay * 3 / 2;
        if (delay > EXPR_AUTO_DELAY_MAX_MS) delay = EXPR_AUTO_DELAY_MAX_MS;
    }
    else
    {
        return 0;
    }

    v->autoDelayMs = delay;
    return 1;
}

static ExprError VizStep(Viz* v, const Frame* f)
{
    int key;

    ++v->step;
    if (v->trace)
    {
        TracePrint(v, f);
        return EXPR_OK;
    }
    if (v->mode == STEP_SKIP_PHASE) return EXPR_OK;

    Render(v, f);

    if (v->mode == STEP_AUTO)
    {
        if (v->autoDelayMs > 0) Sleep((DWORD)v->autoDelayMs);
        if (!_kbhit()) return EXPR_OK;
        key = ReadKey();
        if (AdjustSpeed(v, key)) return EXPR_OK;    /* keeps playing */
        if (key == 27 || key == 'q' || key == 'Q') return EXPR_ERR_ABORTED;
        v->mode = key == '\r' ? STEP_SKIP_PHASE : STEP_MANUAL;
        return EXPR_OK;
    }

    for (;;)
    {
        key = ReadKey();
        if (key == ' ') return EXPR_OK;
        if (key == '\r') { v->mode = STEP_SKIP_PHASE; return EXPR_OK; }
        if (key == 'a' || key == 'A') { v->mode = STEP_AUTO; return EXPR_OK; }
        if (key == 27 || key == 'q' || key == 'Q') return EXPR_ERR_ABORTED;
        if (AdjustSpeed(v, key)) Render(v, f);      /* same step, updated footer */
    }
}

static void VizPhase(Viz* v, ExprPhase phase)
{
    v->phase = phase;
}

/* The closing frame of a phase is always shown, even after [enter]. */
static ExprError VizPhaseEnd(Viz* v, const Frame* f)
{
    if (v->mode == STEP_SKIP_PHASE) v->mode = STEP_MANUAL;
    return VizStep(v, f);
}

static Frame FrameText(const char* label, const char* text, size_t len)
{
    Frame f;
    memset(&f, 0, sizeof f);
    f.label = label;
    f.text = text;
    f.textLen = len;
    f.depthUpto = SIZE_MAX;
    f.exprMark = -1;
    f.valueMark = -1;
    return f;
}

static void FrameNeedle(Frame* f, size_t pos, NeedleKind kind)
{
    f->hasNeedle = 1;
    f->needle.pos = pos;
    f->needle.kind = kind;
}

static void FrameNeedle2(Frame* f, size_t pos, NeedleKind kind)
{
    f->hasNeedle2 = 1;
    f->needle2.pos = pos;
    f->needle2.kind = kind;
}

static void FrameHighlight(Frame* f, size_t begin, size_t end, Rgb color)
{
    if (f->hlCount >= (int)(sizeof f->hl / sizeof f->hl[0])) return;
    f->hl[f->hlCount].begin = begin;
    f->hl[f->hlCount].end = end;
    f->hl[f->hlCount].color = color;
    ++f->hlCount;
}

/* ========================================================================= */
/* Buffer helpers                                                            */
/* ========================================================================= */

static ExprError Fail(Evaluator* ev, ExprError err, const char* text, size_t len, size_t pos)
{
    ev->errText = text;
    ev->errLen = len;
    ev->errPos = pos;
    ev->hasErrPos = 1;
    return err;
}

/* Appends n chars and keeps dst '\0'-terminated. */
static ExprError Emit(char* dst, size_t cap, size_t* w, const char* s, size_t n)
{
    if (*w + n + 1 > cap) return EXPR_ERR_BUFFER_OVERFLOW;
    memcpy(dst + *w, s, n);
    *w += n;
    dst[*w] = '\0';
    return EXPR_OK;
}

static ExprError EmitChar(char* dst, size_t cap, size_t* w, char c)
{
    return Emit(dst, cap, w, &c, 1);
}

/* Caller guarantees cap >= *len + 2. Moves the '\0' too. */
static void InsertChar(char* s, size_t* len, size_t pos, char c)
{
    memmove(s + pos + 1, s + pos, *len - pos + 1);
    s[pos] = c;
    ++*len;
}

static void RemoveChars(char* s, size_t* len, size_t pos, size_t n)
{
    memmove(s + pos, s + pos + n, *len - pos - n + 1);
    *len -= n;
}

/* s[pos] is '[' or '('; returns one past its matching ']' or ')'. */
static size_t AtomEnd(const char* s, size_t len, size_t pos)
{
    int depth = 0;

    if (s[pos] == '[')
    {
        while (pos < len && s[pos] != ']') ++pos;
        return pos + 1;
    }
    for (; pos < len; ++pos)
    {
        if (s[pos] == '(') ++depth;
        else if (s[pos] == ')' && --depth == 0) return pos + 1;
    }
    return len;
}

/* s[end - 1] is ']' or ')'; returns the index of its matching '[' or '('. */
static size_t AtomStart(const char* s, size_t end)
{
    size_t p = end - 1;
    int depth = 0;

    if (s[p] == ']')
    {
        while (p > 0 && s[p] != '[') --p;
        return p;
    }
    for (;;)
    {
        if (s[p] == ')') ++depth;
        else if (s[p] == '(' && --depth == 0) return p;
        if (p == 0) return 0;
        --p;
    }
}

static int DepthAt(const char* s, size_t pos)
{
    int depth = 0;
    size_t i;
    for (i = 0; i <= pos; ++i)
    {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
    }
    return depth;
}

/* ========================================================================= */
/* Stacks                                                                    */
/* ========================================================================= */

static char* ExprStackPopFirst(ExprStack* s)
{
    char* p;
    if (s->base >= s->count) return NULL;
    p = s->items[s->base];
    s->items[s->base] = NULL;
    ++s->base;
    return p;
}

static ExprError ValuePush(ValueStack* s, double value)
{
    if (s->count >= EXPR_VALUE_STACK_MAX) return EXPR_ERR_STACK_OVERFLOW;
    s->items[s->count++] = value;
    return EXPR_OK;
}

static ExprError ValuePop(ValueStack* s, double* value)
{
    if (s->count == 0) return EXPR_ERR_STACK_UNDERFLOW;
    *value = s->items[--s->count];
    return EXPR_OK;
}

static void EvaluatorFree(Evaluator* ev)
{
    size_t i;
    for (i = 0; i < ev->exprStack.count; ++i)
    {
        free(ev->exprStack.items[i]);
        ev->exprStack.items[i] = NULL;
    }
    free(ev->buf);
    free(ev->scratch);
    free(ev->groups);
    ev->buf = NULL;
    ev->scratch = NULL;
    ev->groups = NULL;
}

/* ========================================================================= */
/* 0. copy                                                                   */
/* ========================================================================= */

static ExprError Phase0Copy(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    size_t len = 0;
    Frame f;

    VizPhase(v, PHASE_COPY);
    if (ev->input == NULL) return EXPR_ERR_NULL_INPUT;

    /* bounded length scan: never reads past index EXPR_MAX_INPUT */
    while (ev->input[len] != '\0')
    {
        if (len >= EXPR_MAX_INPUT) return EXPR_ERR_INPUT_TOO_LONG;
        ++len;
    }
    if (len == 0) return EXPR_ERR_EMPTY;

    ev->buf = (char*)malloc(len + 1);
    if (ev->buf == NULL) return EXPR_ERR_OUT_OF_MEMORY;
    memcpy(ev->buf, ev->input, len);
    ev->buf[len] = '\0';
    ev->len = len;
    ev->cap = len + 1;

    f = FrameText("buf", ev->buf, ev->len);
    FrameNeedle(&f, len, NEEDLE_TERMINATOR);
    FrameHighlight(&f, 0, len, C_BG_GROUP);
    snprintf(msg, sizeof msg, "copied %zu chars into char[%zu]; the extra slot buf[%zu] holds '\\0'",
             len, len + 1, len);
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

/* ========================================================================= */
/* 1. normalize                                                              */
/* ========================================================================= */

static ExprError Phase1Normalize(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    size_t r, w = 0;
    char* dst;
    Frame f;

    VizPhase(v, PHASE_NORMALIZE);
    dst = (char*)malloc(ev->len + 1);
    if (dst == NULL) return EXPR_ERR_OUT_OF_MEMORY;
    dst[0] = '\0';
    ev->scratch = dst;

    for (r = 0; r < ev->len; ++r)
    {
        char c = ev->buf[r];

        f = FrameText("src", ev->buf, ev->len);
        FrameNeedle(&f, r, ClassifyChar(c));
        if (IsSpaceChar(c))
        {
            snprintf(msg, sizeof msg, "src[%zu] is whitespace -> skipped", r);
        }
        else
        {
            dst[w++] = c;
            dst[w] = '\0';
            snprintf(msg, sizeof msg, "src[%zu] = '%c' -> dst[%zu]", r, c, w - 1);
        }
        f.label2 = "dst";
        f.text2 = dst;
        f.text2Len = w;
        f.message = msg;
        TRY(VizStep(v, &f));
    }

    free(ev->buf);
    ev->buf = dst;
    ev->scratch = NULL;
    snprintf(msg, sizeof msg, "normalized: %zu -> %zu chars", ev->len, w);
    ev->len = w;
    if (w == 0) return EXPR_ERR_EMPTY;

    f = FrameText("norm", ev->buf, ev->len);
    FrameNeedle(&f, ev->len, NEEDLE_TERMINATOR);
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

/* ========================================================================= */
/* 2. explicit operands (also validates the grammar)                         */
/* ========================================================================= */

/* An operand just ended at `depth`: close every "([-1]*" opened at that depth. */
static ExprError CloseNegations(char* dst, size_t cap, size_t* w,
                                const int* closeAt, size_t* closeCount, int depth, size_t* closed)
{
    *closed = 0;
    while (*closeCount > 0 && closeAt[*closeCount - 1] == depth)
    {
        TRY(EmitChar(dst, cap, w, ')'));
        --*closeCount;
        ++*closed;
    }
    return EXPR_OK;
}

static ExprError Phase2Operands(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    const char* src = ev->buf;
    size_t len = ev->len;
    size_t cap = 8 * len + 16;      /* worst case: each '-' becomes "([-1]*" + ')' */
    size_t w = 0, i = 0, closed;
    int expectOperand = 1, depth = 0;
    int closeAt[EXPR_MAX_INPUT];    /* depth each pending "([-1]*" wrapper closes at */
    size_t closeCount = 0;
    char* dst;
    Frame f;

    VizPhase(v, PHASE_OPERANDS);
    dst = (char*)malloc(cap);
    if (dst == NULL) return EXPR_ERR_OUT_OF_MEMORY;
    dst[0] = '\0';
    ev->scratch = dst;

    while (i < len)
    {
        char c = src[i];
        size_t tokStart = i;
        NeedleKind kind;

        if (!IsNumberChar(c) && !IsOperatorChar(c) && c != '(' && c != ')')
            return Fail(ev, EXPR_ERR_INVALID_CHAR, src, len, i);

        if (expectOperand)
        {
            if (IsNumberChar(c) || (c == '-' && i + 1 < len && IsNumberChar(src[i + 1])))
            {
                size_t digits = 0, dots = 0;
                int n;

                if (c == '-') ++i;
                while (i < len && IsNumberChar(src[i]))
                {
                    if (src[i] == '.') ++dots;
                    else ++digits;
                    ++i;
                }
                if (digits == 0 || dots > 1)
                    return Fail(ev, EXPR_ERR_INVALID_NUMBER, src, len, tokStart);

                TRY(EmitChar(dst, cap, &w, '['));
                TRY(Emit(dst, cap, &w, src + tokStart, i - tokStart));
                TRY(EmitChar(dst, cap, &w, ']'));
                TRY(CloseNegations(dst, cap, &w, closeAt, &closeCount, depth, &closed));
                kind = c == '-' ? NEEDLE_UNARY : NEEDLE_OPERAND;
                n = snprintf(msg, sizeof msg, "%s \"%.*s\" -> [%.*s]",
                             c == '-' ? "unary '-' + operand" : "operand",
                             (int)(i - tokStart), src + tokStart, (int)(i - tokStart), src + tokStart);
                if (closed > 0 && n > 0 && (size_t)n < sizeof msg)
                    snprintf(msg + n, sizeof msg - (size_t)n, ", closes %zu ([-1]* wrapper(s)", closed);
                expectOperand = 0;
            }
            else if (c == '(')
            {
                TRY(EmitChar(dst, cap, &w, '('));
                ++depth;
                ++i;
                kind = NEEDLE_EXPR_START;
                snprintf(msg, sizeof msg, "'(' opens an expression (depth %d)", depth);
            }
            else if (IsUnaryOperator(c))
            {
                TRY(EmitChar(dst, cap, &w, c));
                ++i;
                kind = NEEDLE_UNARY;
                snprintf(msg, sizeof msg, "'%c' (sqrt) applies to the next operand, still expecting one", c);
            }
            else if (c == '-' && i + 1 < len && (src[i + 1] == '(' || src[i + 1] == '-' || IsUnaryOperator(src[i + 1])))
            {
                /* -X where X is not a plain number: ([-1]*X), closed once X ends at this depth */
                if (closeCount >= EXPR_MAX_INPUT) return EXPR_ERR_BUFFER_OVERFLOW;
                TRY(Emit(dst, cap, &w, "([-1]*", 6));
                closeAt[closeCount++] = depth;
                ++i;
                kind = NEEDLE_UNARY;
                snprintf(msg, sizeof msg, "unary '-' before '%c' -> ([-1]* ... ), closed after that operand", src[i]);
            }
            else if (c == '-')
            {
                return Fail(ev, EXPR_ERR_UNSUPPORTED_UNARY, src, len, i);
            }
            else
            {
                return Fail(ev, EXPR_ERR_UNEXPECTED_TOKEN, src, len, i);
            }
        }
        else
        {
            if (IsBinaryOperator(c))
            {
                TRY(EmitChar(dst, cap, &w, c));
                ++i;
                expectOperand = 1;
                kind = NEEDLE_OPERATOR;
                snprintf(msg, sizeof msg, "binary operator '%c'", c);
            }
            else if (c == ')')
            {
                if (depth == 0) return Fail(ev, EXPR_ERR_UNBALANCED_PARENS, src, len, i);
                TRY(EmitChar(dst, cap, &w, ')'));
                --depth;
                TRY(CloseNegations(dst, cap, &w, closeAt, &closeCount, depth, &closed));
                if (closed > 0)
                    snprintf(msg, sizeof msg, "')' closes depth %d and %zu ([-1]* wrapper(s)", depth + 1, closed);
                else
                    snprintf(msg, sizeof msg, "')' closes depth %d", depth + 1);
                ++i;
                kind = NEEDLE_EXPR_END;
            }
            else
            {
                /* e.g. "2(3)" or "(1)2": implicit multiplication is not supported */
                return Fail(ev, EXPR_ERR_UNEXPECTED_TOKEN, src, len, i);
            }
        }

        f = FrameText("src", src, len);
        FrameNeedle(&f, tokStart, kind);
        FrameHighlight(&f, tokStart, i, C_BG_LEFT);
        f.label2 = "dst";
        f.text2 = dst;
        f.text2Len = w;
        f.message = msg;
        TRY(VizStep(v, &f));
    }

    if (expectOperand) return Fail(ev, EXPR_ERR_UNEXPECTED_TOKEN, src, len, len);
    if (depth != 0) return Fail(ev, EXPR_ERR_UNBALANCED_PARENS, src, len, len);

    free(ev->buf);
    ev->buf = dst;
    ev->scratch = NULL;
    ev->len = w;
    ev->cap = cap;

    f = FrameText("expl", ev->buf, ev->len);
    FrameNeedle(&f, ev->len, NEEDLE_TERMINATOR);
    snprintf(msg, sizeof msg, "all operands explicit: %zu chars", ev->len);
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

/* ========================================================================= */
/* 3. explicit expressions: every expression becomes (operand op operand)    */
/* ========================================================================= */

/* Wraps the operator at *i, unless it already is exactly "(operand op operand)"
   or "($operand)". On a wrap the operator moves one char right; *i follows it. */
static ExprError WrapOperator(Evaluator* ev, Viz* v, const char* passName, size_t* i)
{
    char msg[EXPR_MSG_SIZE];
    char c = ev->buf[*i];
    int unary = IsUnaryOperator(c);
    size_t ls = unary ? *i : AtomStart(ev->buf, *i);
    size_t re = AtomEnd(ev->buf, ev->len, *i + 1);
    int wrapped = ls > 0 && ev->buf[ls - 1] == '(' && re < ev->len && ev->buf[re] == ')';
    Frame f;

    f = FrameText("work", ev->buf, ev->len);
    FrameNeedle(&f, *i, ClassifyChar(c));
    if (!unary) FrameHighlight(&f, ls, *i, C_BG_LEFT);
    FrameHighlight(&f, *i + 1, re, C_BG_RIGHT);
    f.depthRows = 1;
    if (unary)
        snprintf(msg, sizeof msg, "pass %s: '%c' operand=%.*s -> %s", passName, c,
                 (int)(re - *i - 1), ev->buf + *i + 1, wrapped ? "already explicit" : "wrap in ( )");
    else
        snprintf(msg, sizeof msg, "pass %s: '%c' left=%.*s right=%.*s -> %s", passName, c,
                 (int)(*i - ls), ev->buf + ls, (int)(re - *i - 1), ev->buf + *i + 1,
                 wrapped ? "already explicit" : "wrap in ( )");
    f.message = msg;
    TRY(VizStep(v, &f));

    if (wrapped) return EXPR_OK;

    InsertChar(ev->buf, &ev->len, re, ')');
    InsertChar(ev->buf, &ev->len, ls, '(');
    ++*i;

    f = FrameText("work", ev->buf, ev->len);
    FrameNeedle(&f, *i, ClassifyChar(c));
    FrameNeedle2(&f, ls, NEEDLE_EXPR_START);
    FrameHighlight(&f, ls, re + 2, C_BG_GROUP);
    f.depthRows = 1;
    snprintf(msg, sizeof msg, "now explicit: %.*s", (int)(re + 2 - ls), ev->buf + ls);
    f.message = msg;
    return VizStep(v, &f);
}

static ExprError Phase3Expressions(Evaluator* ev, Viz* v)
{
    /* highest precedence first; $ and ^ group right to left */
    static const char* const passOps[] = { "$", "^", "*/", "+-" };
    static const int rightToLeft[] = { 1, 1, 0, 0 };
    char msg[EXPR_MSG_SIZE];
    size_t i, need;
    int p;
    Frame f;

    VizPhase(v, PHASE_EXPRESSIONS);

    /* every wrap adds 2 chars, once per operator */
    ev->opCount = 0;
    for (i = 0; i < ev->len; ++i)
    {
        if (ev->buf[i] == '[') i = AtomEnd(ev->buf, ev->len, i) - 1;
        else if (IsOperatorChar(ev->buf[i])) ++ev->opCount;
    }
    need = ev->len + 2 * ev->opCount + 1;
    if (need > ev->cap)
    {
        char* grown = (char*)realloc(ev->buf, need);
        if (grown == NULL) return EXPR_ERR_OUT_OF_MEMORY;
        ev->buf = grown;
        ev->cap = need;
    }

    /* 3a: drop redundant groups such as ([5]) or ((...)) */
    i = 0;
    while (i < ev->len)
    {
        char c = ev->buf[i];
        size_t end;
        int redundant;

        if (c == '[') { i = AtomEnd(ev->buf, ev->len, i); continue; }
        if (c != '(') { ++i; continue; }

        end = AtomEnd(ev->buf, ev->len, i);
        redundant = (ev->buf[i + 1] == '[' || ev->buf[i + 1] == '(')
                    && AtomEnd(ev->buf, ev->len, i + 1) == end - 1;

        f = FrameText("work", ev->buf, ev->len);
        FrameNeedle(&f, i, NEEDLE_EXPR_START);
        FrameNeedle2(&f, end - 1, NEEDLE_EXPR_END);
        FrameHighlight(&f, i, end, C_BG_GROUP);
        f.depthRows = 1;
        snprintf(msg, sizeof msg, "group %.*s %s", (int)(end - i), ev->buf + i,
                 redundant ? "wraps a single operand -> redundant" : "holds an expression -> kept");
        f.message = msg;
        TRY(VizStep(v, &f));

        if (!redundant) { ++i; continue; }

        RemoveChars(ev->buf, &ev->len, end - 1, 1);
        RemoveChars(ev->buf, &ev->len, i, 1);
        f = FrameText("work", ev->buf, ev->len);
        FrameNeedle(&f, i, ClassifyChar(ev->buf[i]));
        f.depthRows = 1;
        f.message = "redundant '(' ')' removed";
        TRY(VizStep(v, &f));
    }

    /* 3b: $   3c: ^   3d: * /   3e: + -   (scan direction = associativity) */
    for (p = 0; p < (int)(sizeof passOps / sizeof passOps[0]); ++p)
    {
        const char* ops = passOps[p];

        if (rightToLeft[p])
        {
            /* after a wrap the needle continues inside the left operand,
               so operators nested there are still visited */
            i = ev->len;
            while (i > 0)
            {
                char c = ev->buf[--i];
                if (c == ']') { while (ev->buf[i] != '[') --i; continue; }
                if (strchr(ops, c) != NULL) TRY(WrapOperator(ev, v, ops, &i));
            }
        }
        else
        {
            i = 0;
            while (i < ev->len)
            {
                char c = ev->buf[i];
                if (c == '[') { i = AtomEnd(ev->buf, ev->len, i); continue; }
                if (strchr(ops, c) != NULL) TRY(WrapOperator(ev, v, ops, &i));
                ++i;
            }
        }
    }

    f = FrameText("expl", ev->buf, ev->len);
    FrameNeedle(&f, ev->len, NEEDLE_TERMINATOR);
    f.depthRows = 1;
    snprintf(msg, sizeof msg, "%zu operator(s), every expression is (operand op operand)", ev->opCount);
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

/* ========================================================================= */
/* 4. needle walk counting depth, building the NeedleExpressions             */
/* ========================================================================= */

static ExprError Phase4Depth(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    size_t i, g = 0, count = 0;
    int depth = 0;
    Frame f;

    VizPhase(v, PHASE_DEPTH);

    for (i = 0; i < ev->len; ++i)
        if (ev->buf[i] == '(') ++count;
    ev->groups = (NeedleExpression*)calloc(count > 0 ? count : 1, sizeof *ev->groups);
    if (ev->groups == NULL) return EXPR_ERR_OUT_OF_MEMORY;
    ev->groupCount = count;
    ev->maxDepth = 0;

    i = 0;
    while (i < ev->len)
    {
        char c = ev->buf[i];
        size_t next = i + 1;

        f = FrameText("work", ev->buf, ev->len);
        f.depthRows = 1;

        if (c == '[')
        {
            next = AtomEnd(ev->buf, ev->len, i);
            FrameNeedle(&f, i, NEEDLE_OPERAND);
            snprintf(msg, sizeof msg, "operand %.*s at depth %d", (int)(next - i), ev->buf + i, depth);
        }
        else if (c == '(')
        {
            ++depth;
            if (depth > ev->maxDepth) ev->maxDepth = depth;
            ev->groups[g].start.pos = i;
            ev->groups[g].start.kind = NEEDLE_EXPR_START;
            ev->groups[g].end.kind = NEEDLE_NONE;
            ev->groups[g].depth = depth;
            ++g;
            FrameNeedle(&f, i, NEEDLE_EXPR_START);
            snprintf(msg, sizeof msg, "needleExpressionStart, depth -> %d", depth);
        }
        else if (c == ')')
        {
            size_t k = g;
            while (k > 0 && ev->groups[k - 1].end.kind != NEEDLE_NONE) --k;
            if (k == 0) return Fail(ev, EXPR_ERR_UNBALANCED_PARENS, ev->buf, ev->len, i);
            --k;
            ev->groups[k].end.pos = i;
            ev->groups[k].end.kind = NEEDLE_EXPR_END;
            FrameNeedle(&f, i, NEEDLE_EXPR_END);
            FrameNeedle2(&f, ev->groups[k].start.pos, NEEDLE_EXPR_START);
            FrameHighlight(&f, ev->groups[k].start.pos, i + 1, C_BG_GROUP);
            snprintf(msg, sizeof msg, "needleExpressionEnd: pair [%zu..%zu] at depth %d",
                     ev->groups[k].start.pos, i, depth);
            --depth;
        }
        else
        {
            FrameNeedle(&f, i, NEEDLE_OPERATOR);
            snprintf(msg, sizeof msg, "operator '%c' at depth %d", c, depth);
        }

        f.depthUpto = next;
        f.message = msg;
        TRY(VizStep(v, &f));
        i = next;
    }

    f = FrameText("work", ev->buf, ev->len);
    FrameNeedle(&f, ev->len, NEEDLE_TERMINATOR);
    f.depthRows = 1;
    snprintf(msg, sizeof msg, "%zu needleExpression(s), max depth %d", ev->groupCount, ev->maxDepth);
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

/* ========================================================================= */
/* 5-6. push innermost expression, replace its text with "" until '\0'       */
/* ========================================================================= */

/* "([4]+[-1])" -> "4 + -1"; s[open] == '(' and s[close] == ')'. */
static ExprError BuildEntry(const char* s, size_t open, size_t close, char* out, size_t* outLen)
{
    size_t i = open + 1, w = 0;

    out[0] = '\0';
    while (i < close)
    {
        size_t b = i, e;
        size_t need;

        if (s[i] == '[')
        {
            b = i + 1;
            e = b;
            while (e < close && s[e] != ']') ++e;
            i = e + 1;
        }
        else
        {
            e = i + 1;
            i = e;
        }

        need = (w > 0 ? 1 : 0) + (e - b);
        if (w + need + 1 > EXPR_STACK_ENTRY_SIZE) return EXPR_ERR_STACK_ENTRY_TOO_LONG;
        if (w > 0) out[w++] = ' ';
        memcpy(out + w, s + b, e - b);
        w += e - b;
        out[w] = '\0';
    }
    *outLen = w;
    return EXPR_OK;
}

static ExprError Phase5Stack(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    ExprStack* es = &ev->exprStack;
    Frame f;

    VizPhase(v, PHASE_STACK);

    while (ev->len > 0)
    {
        size_t i = 0, open, close, entryLen = 0;
        char* entry;
        ExprError err;

        /* needle runs to the first ')': the leftmost innermost expression */
        while (i < ev->len && ev->buf[i] != ')')
        {
            size_t next = ev->buf[i] == '[' ? AtomEnd(ev->buf, ev->len, i) : i + 1;

            f = FrameText("work", ev->buf, ev->len);
            FrameNeedle(&f, i, ClassifyChar(ev->buf[i]));
            f.depthRows = 1;
            f.message = "needle looking for the first ')'";
            TRY(VizStep(v, &f));
            i = next;
        }
        if (i >= ev->len) return Fail(ev, EXPR_ERR_MALFORMED_ENTRY, ev->buf, ev->len, i);

        close = i;
        open = close;
        while (ev->buf[open] != '(') --open;    /* no ')' before close, so the last '(' matches */

        f = FrameText("work", ev->buf, ev->len);
        FrameNeedle(&f, open, NEEDLE_EXPR_START);
        FrameNeedle2(&f, close, NEEDLE_EXPR_END);
        FrameHighlight(&f, open, close + 1, C_BG_GROUP);
        f.depthRows = 1;
        snprintf(msg, sizeof msg, "needleExpression [%zu..%zu] at depth %d holds no other expression",
                 open, close, DepthAt(ev->buf, open));
        f.message = msg;
        TRY(VizStep(v, &f));

        if (close - open > 1)
        {
            if (es->count >= EXPR_STACK_MAX) return EXPR_ERR_STACK_OVERFLOW;
            entry = (char*)malloc(EXPR_STACK_ENTRY_SIZE);
            if (entry == NULL) return EXPR_ERR_OUT_OF_MEMORY;
            err = BuildEntry(ev->buf, open, close, entry, &entryLen);
            if (err != EXPR_OK)
            {
                free(entry);
                return Fail(ev, err, ev->buf, ev->len, open);
            }
            es->items[es->count++] = entry;

            f = FrameText("work", ev->buf, ev->len);
            FrameNeedle(&f, open, NEEDLE_EXPR_START);
            FrameNeedle2(&f, close, NEEDLE_EXPR_END);
            FrameHighlight(&f, open, close + 1, C_BG_GROUP);
            f.depthRows = 1;
            f.exprMark = (long)(es->count - 1);
            snprintf(msg, sizeof msg, "push \"%s\" (%zu/%d chars)", entry, entryLen + 1, EXPR_STACK_ENTRY_SIZE);
            f.message = msg;
            TRY(VizStep(v, &f));
        }

        RemoveChars(ev->buf, &ev->len, open, close - open + 1);

        f = FrameText("work", ev->buf, ev->len);
        FrameNeedle(&f, open, ClassifyChar(ev->buf[open]));
        f.depthRows = 1;
        f.message = "expression text replaced by \"\"";
        TRY(VizStep(v, &f));
    }

    VizPhase(v, PHASE_DRAINED);
    f = FrameText("work", ev->buf, ev->len);
    FrameNeedle(&f, 0, NEEDLE_TERMINATOR);
    snprintf(msg, sizeof msg, "buf[0] == '\\0'; %zu expression(s) on the stack", es->count - es->base);
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

/* ========================================================================= */
/* 7-8. pop first expression, convert, operate, feed the next pop            */
/* ========================================================================= */

static int IsOperatorToken(const char* t)
{
    return IsOperatorChar(t[0]) && t[1] == '\0';
}

static ExprError ParseNumber(const char* t, double* out)
{
    char* end;
    *out = strtod(t, &end);
    if (end == t || *end != '\0') return EXPR_ERR_MALFORMED_ENTRY;
    if (!isfinite(*out)) return EXPR_ERR_MATH_DOMAIN;  /* a literal too big for a double */
    return EXPR_OK;
}

static void DescribeOperand(char* out, size_t size, double value, int fromStack)
{
    snprintf(out, size, fromStack ? "%.10g (value stack)" : "%.10g", value);
}

static ExprError Phase7Evaluate(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    ExprStack* es = &ev->exprStack;
    ValueStack* vs = &ev->values;
    Frame f;

    while (es->base < es->count)
    {
        char tok[3][EXPR_STACK_ENTRY_SIZE];
        char shown[EXPR_STACK_ENTRY_SIZE];
        char ls[EXPR_NUMBER_TEXT_SIZE], rs[EXPR_NUMBER_TEXT_SIZE];
        size_t nt = 0, i = 0, k, idx = es->base;
        int opIdx = -1, lMissing, rMissing;
        double l = 0.0, r = 0.0, res = 0.0;
        char op;
        char* entry;

        VizPhase(v, PHASE_POP);
        f = FrameText(NULL, NULL, 0);
        f.exprMark = (long)idx;
        snprintf(msg, sizeof msg, "pop the first expression #%zu: \"%s\"", idx, es->items[idx]);
        f.message = msg;
        TRY(VizStep(v, &f));

        entry = ExprStackPopFirst(es);
        strcpy(shown, entry);   /* both are EXPR_STACK_ENTRY_SIZE */
        while (entry[i] != '\0' && nt <= 3)
        {
            size_t b;
            while (entry[i] == ' ') ++i;
            if (entry[i] == '\0') break;
            b = i;
            while (entry[i] != '\0' && entry[i] != ' ') ++i;
            if (nt == 3) { nt = 4; break; }
            memcpy(tok[nt], entry + b, i - b);
            tok[nt][i - b] = '\0';
            ++nt;
        }
        free(entry);    /* popped pointer is deleted right away */
        if (nt == 0 || nt > 3) return EXPR_ERR_MALFORMED_ENTRY;

        for (k = 0; k < nt; ++k)
        {
            if (IsOperatorToken(tok[k]))
            {
                if (opIdx >= 0) return EXPR_ERR_MALFORMED_ENTRY;
                opIdx = (int)k;
            }
        }

        VizPhase(v, PHASE_CONSUME);

        if (opIdx < 0)
        {
            if (nt != 1) return EXPR_ERR_MALFORMED_ENTRY;
            TRY(ParseNumber(tok[0], &res));
            TRY(ValuePush(vs, res));
            f = FrameText(NULL, NULL, 0);
            f.valueMark = (long)(vs->count - 1);
            snprintf(msg, sizeof msg, "\"%s\" is a lone operand -> push %.10g", shown, res);
            f.message = msg;
            TRY(VizStep(v, &f));
            continue;
        }

        op = tok[opIdx][0];

        if (IsUnaryOperator(op))
        {
            /* "$ 4", or "$" alone when its operand was an inner expression */
            int missing = nt == 1;

            if (opIdx != 0 || nt > 2) return EXPR_ERR_MALFORMED_ENTRY;
            if (missing) TRY(ValuePop(vs, &r));
            else TRY(ParseNumber(tok[1], &r));

            DescribeOperand(rs, sizeof rs, r, missing);
            f = FrameText(NULL, NULL, 0);
            snprintf(msg, sizeof msg, "convert \"%s\": op = '%c' (sqrt), operand = %s", shown, op, rs);
            f.message = msg;
            TRY(VizStep(v, &f));

            res = sqrt(r);
            if (!isfinite(res)) return EXPR_ERR_MATH_DOMAIN;
            TRY(ValuePush(vs, res));

            f = FrameText(NULL, NULL, 0);
            f.valueMark = (long)(vs->count - 1);
            snprintf(msg, sizeof msg, "sqrt(%.10g) = %.10g -> value stack (for the next pop)", r, res);
            f.message = msg;
            TRY(VizStep(v, &f));
            continue;
        }

        lMissing = opIdx == 0;
        rMissing = (size_t)opIdx == nt - 1;
        if ((size_t)opIdx > 1 || nt - (size_t)opIdx - 1 > 1) return EXPR_ERR_MALFORMED_ENTRY;

        if (!lMissing) TRY(ParseNumber(tok[0], &l));
        if (!rMissing) TRY(ParseNumber(tok[opIdx + 1], &r));
        /* the most recent result is the right operand when both are missing */
        if (rMissing) TRY(ValuePop(vs, &r));
        if (lMissing) TRY(ValuePop(vs, &l));

        DescribeOperand(ls, sizeof ls, l, lMissing);
        DescribeOperand(rs, sizeof rs, r, rMissing);
        f = FrameText(NULL, NULL, 0);
        snprintf(msg, sizeof msg, "convert \"%s\": left = %s, op = '%c', right = %s", shown, ls, op, rs);
        f.message = msg;
        TRY(VizStep(v, &f));

        switch (op)
        {
        case '+': res = l + r; break;
        case '-': res = l - r; break;
        case '*': res = l * r; break;
        case '/':
            if (r == 0.0) return EXPR_ERR_DIVISION_BY_ZERO;
            res = l / r;
            break;
        case '^': res = pow(l, r); break;
        default:
            return EXPR_ERR_MALFORMED_ENTRY;
        }
        if (!isfinite(res)) return EXPR_ERR_MATH_DOMAIN;
        TRY(ValuePush(vs, res));

        f = FrameText(NULL, NULL, 0);
        f.valueMark = (long)(vs->count - 1);
        snprintf(msg, sizeof msg, "%.10g %c %.10g = %.10g -> value stack (for the next pop)", l, op, r, res);
        f.message = msg;
        TRY(VizStep(v, &f));
    }

    if (vs->count != 1) return EXPR_ERR_MALFORMED_ENTRY;
    ev->result = vs->items[0];
    return EXPR_OK;
}

/* ========================================================================= */
/* Driver                                                                    */
/* ========================================================================= */

/* After phase 3 an expression without operators is exactly "[number]". */
static ExprError SingleOperand(Evaluator* ev, Viz* v)
{
    char msg[EXPR_MSG_SIZE];
    char tmp[EXPR_STACK_ENTRY_SIZE];
    size_t n;
    Frame f;

    if (ev->len < 3 || ev->buf[0] != '[' || ev->buf[ev->len - 1] != ']')
        return EXPR_ERR_MALFORMED_ENTRY;
    n = ev->len - 2;
    if (n >= sizeof tmp) return EXPR_ERR_STACK_ENTRY_TOO_LONG;
    strncpy(tmp, ev->buf + 1, n);
    tmp[n] = '\0';      /* strncpy does not terminate when it stops at n */
    TRY(ParseNumber(tmp, &ev->result));

    f = FrameText("work", ev->buf, ev->len);
    FrameHighlight(&f, 0, ev->len, C_BG_GROUP);
    snprintf(msg, sizeof msg, "no operators: the operand itself is the result (phases 4-8 skipped)");
    f.message = msg;
    return VizPhaseEnd(v, &f);
}

static void ShowError(Viz* v, const Evaluator* ev, ExprError err)
{
    char msg[EXPR_MSG_SIZE];
    Frame f = FrameText("error", ev->errText, ev->errText != NULL ? ev->errLen : 0);

    if (ev->errText != NULL && ev->hasErrPos)
        FrameNeedle(&f, ev->errPos, ev->errPos < ev->errLen ? ClassifyChar(ev->errText[ev->errPos]) : NEEDLE_TERMINATOR);
    if (ev->hasErrPos)
        snprintf(msg, sizeof msg, "error %d at %zu: %s  (any key)", (int)err, ev->errPos, ExprErrorString(err));
    else
        snprintf(msg, sizeof msg, "error %d: %s  (any key)", (int)err, ExprErrorString(err));
    f.isError = 1;
    f.message = msg;

    v->mode = STEP_MANUAL;
    ++v->step;
    if (v->trace)
    {
        TracePrint(v, &f);
        return;
    }
    Render(v, &f);
    (void)ReadKey();
}

static ExprError Evaluate(const char* expr, Viz* v, double* out)
{
    char msg[EXPR_MSG_SIZE];
    Evaluator* ev;
    ExprError err;
    Frame f;

    ev = (Evaluator*)calloc(1, sizeof *ev);    /* ~16 KB of stacks, keep it off the call stack */
    if (ev == NULL) return EXPR_ERR_OUT_OF_MEMORY;
    ev->input = expr;

    v->step = 0;
    v->mode = STEP_MANUAL;
    v->input = expr != NULL ? expr : "(null)";
    v->exprStack = &ev->exprStack;
    v->valueStack = &ev->values;

    err = Phase0Copy(ev, v);
    if (err == EXPR_OK) err = Phase1Normalize(ev, v);
    if (err == EXPR_OK) err = Phase2Operands(ev, v);
    if (err == EXPR_OK) err = Phase3Expressions(ev, v);
    if (err == EXPR_OK)
    {
        if (ev->opCount == 0)
        {
            err = SingleOperand(ev, v);
        }
        else
        {
            err = Phase4Depth(ev, v);
            if (err == EXPR_OK) err = Phase5Stack(ev, v);
            if (err == EXPR_OK) err = Phase7Evaluate(ev, v);
        }
    }

    if (err == EXPR_OK)
    {
        char value[EXPR_NUMBER_TEXT_SIZE];
        int n = snprintf(value, sizeof value, "%.15g", ev->result);

        *out = ev->result;
        VizPhase(v, PHASE_RESULT);
        f = FrameText("result", value, (size_t)n);
        FrameHighlight(&f, 0, (size_t)n, C_BG_GROUP);
        snprintf(msg, sizeof msg, "%s = %s  (any key)", expr, value);
        f.message = msg;
        v->mode = STEP_MANUAL;
        (void)VizStep(v, &f);
    }
    else if (err != EXPR_ERR_ABORTED)
    {
        ShowError(v, ev, err);
    }

    v->exprStack = NULL;
    v->valueStack = NULL;
    EvaluatorFree(ev);
    free(ev);
    return err;
}

static ExprError RunOne(Viz* v, const char* expr)
{
    double result = 0.0;
    ExprError err;

    if (!v->trace)
    {
        fputs(vtBuffMainA vtCurHideA, stdout);
        fflush(stdout);
    }

    err = Evaluate(expr, v, &result);

    if (!v->trace)
    {
        fputs(vtResetA vtRSTA vtCurShowA vtBuffAltA, stdout);
        fflush(stdout);
    }

    if (err == EXPR_OK)
    {
        printf("%s = %.15g\n", expr, result);
        fflush(stdout);
    }
    return err;
}

/* "exit", ignoring surrounding whitespace. */
static int IsExitCommand(const char* line)
{
    size_t b = 0, e = strlen(line);

    while (b < e && IsSpaceChar(line[b])) ++b;
    while (e > b && IsSpaceChar(line[e - 1])) --e;
    return e - b == 4 && strncmp(line + b, "exit", 4) == 0;
}

/* "--name=value" -> "value"; NULL when arg is not that option. */
static const char* OptionValue(const char* arg, const char* name)
{
    size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0 || arg[n] != '=') return NULL;
    return arg + n + 1;
}

static int ParseOnOff(const char* s, int* out)
{
    if (strcmp(s, "on") == 0) { *out = 1; return 1; }
    if (strcmp(s, "off") == 0) { *out = 0; return 1; }
    return 0;
}

/* "max" or 0 = full speed; otherwise EXPR_AUTO_DELAY_MIN_MS..MAX_MS. */
static int ParseDelay(const char* s, int* out)
{
    char* end;
    long ms;

    if (strcmp(s, "max") == 0) { *out = 0; return 1; }
    ms = strtol(s, &end, 10);
    if (end == s || *end != '\0') return 0;
    if (ms != 0 && (ms < EXPR_AUTO_DELAY_MIN_MS || ms > EXPR_AUTO_DELAY_MAX_MS)) return 0;
    *out = (int)ms;
    return 1;
}

static void PrintUsage(void)
{
    printf("ExprEvaluator [options] - reads one expression per line; \"exit\" quits\n"
           "\n"
           "  --trace                        plain step log: no VT, no key waits\n"
           "  --delay=<ms|max>               initial autoplay delay, %d..%d or max (default %d)\n"
           "  --depth-content=on|off         d=N rows show their chars, or only '^' (default %s)\n"
           "  --depth-hide-brackets=on|off   blank out [ ] in the d=N rows (default %s)\n"
           "  --help, -h                     this text\n",
           EXPR_AUTO_DELAY_MIN_MS, EXPR_AUTO_DELAY_MAX_MS, EXPR_AUTO_DELAY_MS,
           EXPR_DEPTH_ROWS_CONTENT ? "on" : "off",
           EXPR_DEPTH_ROWS_HIDE_BRACKETS ? "on" : "off");
}

int main(int argc, char** argv)
{
    char line[EXPR_LINE_SIZE];
    int a;
    Viz viz;

    memset(&viz, 0, sizeof viz);
    viz.autoDelayMs = EXPR_AUTO_DELAY_MS;
    viz.depthContent = EXPR_DEPTH_ROWS_CONTENT;
    viz.hideBrackets = EXPR_DEPTH_ROWS_HIDE_BRACKETS;

    for (a = 1; a < argc; ++a)
    {
        const char* arg = argv[a];
        const char* value;
        int ok = 1;

        if (strcmp(arg, "--trace") == 0) viz.trace = 1;
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) { PrintUsage(); return 0; }
        else if ((value = OptionValue(arg, "--delay")) != NULL) ok = ParseDelay(value, &viz.autoDelayMs);
        else if ((value = OptionValue(arg, "--depth-content")) != NULL) ok = ParseOnOff(value, &viz.depthContent);
        else if ((value = OptionValue(arg, "--depth-hide-brackets")) != NULL) ok = ParseOnOff(value, &viz.hideBrackets);
        else ok = 0;

        if (!ok)
        {
            printf("invalid argument: %s (see --help)\n", arg);
            printf("Failure. Exit code: %d\n", (int)EXPR_ERR_INVALID_ARGUMENT);
            return (int)EXPR_ERR_INVALID_ARGUMENT;
        }
    }

    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (!viz.trace) EnableVt();

    for (;;)
    {
        ExprError err;
        char* nl;

        fputs("expr> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) return 0;   /* end of input */

        nl = strchr(line, '\n');
        if (nl == NULL && !feof(stdin))
        {
            /* longer than the buffer: drain the rest of the line, never truncate */
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            err = EXPR_ERR_INPUT_TOO_LONG;
        }
        else
        {
            if (nl != NULL) *nl = '\0';
            nl = strchr(line, '\r');
            if (nl != NULL) *nl = '\0';
            if (IsExitCommand(line)) return 0;
            err = RunOne(&viz, line);
        }

        if (err != EXPR_OK)
        {
            printf("Failure. Exit code: %d\n", (int)err);
            fflush(stdout);
            return (int)err;
        }
    }
}
