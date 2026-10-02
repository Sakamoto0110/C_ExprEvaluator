#ifndef EXPR_EVALUATOR_H
#define EXPR_EVALUATOR_H

#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* VT sequences                                                              */
/* ------------------------------------------------------------------------- */

#define vtPositionA         "\x1b[%i;%iH"
#define vtBackgroundColorA  "\x1b[48;2;%i;%i;%im"
#define vtForegroundColorA  "\x1b[38;2;%i;%i;%im"
#define vtCurMOVEA          "\x1b[%i;%iH"
#define vtCurUPA            "\x1b[%iA"
#define vtCurDOWNA          "\x1b[%iB"
#define vtCurRIGHTA         "\x1b[%iC"
#define vtCurLEFTA          "\x1b[%iD"
#define vtCurLDOWNA         "\x1b[%iE"
#define vtCurLUPA           "\x1b[%iF"
#define vtCurHABSA          "\x1b[%iG"
#define vtCurVABSA          "\x1b[%id"
#define vtCurSVA            "\x1b""7"
#define vtCurLDA            "\x1b""8"

#define vtBuffMainA         "\x1b[?1049h"
#define vtBuffAltA          "\x1b[?1049l"

#define vtRSTA              "\x1b[!p"

/* Extras not covered by the set above. */
#define vtClearA            "\x1b[2J"
#define vtResetA            "\x1b[0m"
#define vtBgDefaultA        "\x1b[49m"
#define vtCurHideA          "\x1b[?25l"
#define vtCurShowA          "\x1b[?25h"

/* ------------------------------------------------------------------------- */
/* Limits - every fixed-size buffer in the program is sized from here        */
/* ------------------------------------------------------------------------- */

/* Heap */
#define EXPR_MAX_INPUT          1024    /* chars, excluding '\0'; longer input is an error */
#define EXPR_STACK_ENTRY_SIZE   512     /* each pushed expression's malloc, including '\0' */
#define EXPR_STACK_MAX          2048    /* pushed expressions (one per operator) */
#define EXPR_VALUE_STACK_MAX    2048

/* Call stack (char arrays) */
#define EXPR_LINE_SIZE          (EXPR_MAX_INPUT + 128) /* stdin line; needs >= EXPR_MAX_INPUT + 2 */
#define EXPR_MSG_SIZE           1024    /* one step's status message */
#define EXPR_NUMBER_TEXT_SIZE   128     /* one formatted double ("%.15g" needs ~24) */

/* ------------------------------------------------------------------------- */
/* Visualization                                                             */
/* ------------------------------------------------------------------------- */

/* The three settings below are defaults; command-line options override them
   at startup (run with --help). */

/* Autoplay: [+] divides the delay by 1.5, [-] multiplies it by 1.5.
   [+] past the minimum drops the delay to 0 (full speed, shown as "max");
   [-] from full speed resumes at the minimum.
   --delay=<ms|max> */
#define EXPR_AUTO_DELAY_MS      150
#define EXPR_AUTO_DELAY_MIN_MS  10
#define EXPR_AUTO_DELAY_MAX_MS  2000

/* 1: each d=N row shows the chars sitting at depth N, in their columns, so a
      row reads as the expressions that will be pushed from that depth.
   0: each d=N row only shows a '^' under every '(' and ')' of depth N.
   --depth-content=on|off */
#define EXPR_DEPTH_ROWS_CONTENT 0

/* With depth content on: 1 draws operand brackets as blanks in the d=N rows
   ("( 1 + 2 )", the token view); the work row always keeps them.
   --depth-hide-brackets=on|off */
#define EXPR_DEPTH_ROWS_HIDE_BRACKETS 0

/* ------------------------------------------------------------------------- */
/* Enums                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum ExprError
{
    EXPR_OK = 0,
    EXPR_ERR_NULL_INPUT,
    EXPR_ERR_INPUT_TOO_LONG,
    EXPR_ERR_EMPTY,
    EXPR_ERR_OUT_OF_MEMORY,
    EXPR_ERR_INVALID_CHAR,
    EXPR_ERR_INVALID_NUMBER,
    EXPR_ERR_UNEXPECTED_TOKEN,
    EXPR_ERR_UNBALANCED_PARENS,
    EXPR_ERR_UNSUPPORTED_UNARY,
    EXPR_ERR_BUFFER_OVERFLOW,
    EXPR_ERR_STACK_OVERFLOW,
    EXPR_ERR_STACK_ENTRY_TOO_LONG,
    EXPR_ERR_STACK_UNDERFLOW,
    EXPR_ERR_DIVISION_BY_ZERO,
    EXPR_ERR_MALFORMED_ENTRY,
    EXPR_ERR_ABORTED,
    EXPR_ERR_MATH_DOMAIN,   /* appended so existing exit codes keep their values */
    EXPR_ERR_INVALID_ARGUMENT
} ExprError;

/* One per numbered operation of the spec. */
typedef enum ExprPhase
{
    PHASE_COPY = 0,     /* 0: const char* -> mutable char[len + 1]         */
    PHASE_NORMALIZE,    /* 1: strip whitespace                             */
    PHASE_OPERANDS,     /* 2: 5 -> [5], -5 -> [-5]                          */
    PHASE_EXPRESSIONS,  /* 3: implicit -> explicit (operand op operand)    */
    PHASE_DEPTH,        /* 4: needle walk counting depth                   */
    PHASE_STACK,        /* 5: push innermost expression, replace with ""   */
    PHASE_DRAINED,      /* 6: text reached '\0'                            */
    PHASE_POP,          /* 7: pop first expression                         */
    PHASE_CONSUME,      /* 8: convert, operate, feed the next pop          */
    PHASE_RESULT,       /* 9: show the result                              */
    PHASE_COUNT
} ExprPhase;

typedef enum NeedleKind
{
    NEEDLE_NONE = 0,
    NEEDLE_OPERAND,
    NEEDLE_UNARY,
    NEEDLE_OPERATOR,
    NEEDLE_EXPR_START,
    NEEDLE_EXPR_END,
    NEEDLE_WHITESPACE,
    NEEDLE_TERMINATOR
} NeedleKind;

typedef enum StepMode
{
    STEP_MANUAL = 0,    /* wait for a key on every step          */
    STEP_AUTO,          /* advance on a timer until a key        */
    STEP_SKIP_PHASE     /* run silently to the end of the phase  */
} StepMode;

/* ------------------------------------------------------------------------- */
/* Structs                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct Rgb
{
    int r, g, b;
} Rgb;

typedef enum PenBgState
{
    PEN_BG_UNKNOWN = 0,
    PEN_BG_DEFAULT,
    PEN_BG_COLOR
} PenBgState;

/* Colors last sent to the terminal, so a row only emits a VT color
   sequence when the color actually changes. Zeroed = nothing known. */
typedef struct Pen
{
    Rgb fg;
    int hasFg;
    Rgb bg;
    PenBgState bgState;
} Pen;

typedef struct Needle
{
    size_t pos;
    NeedleKind kind;
} Needle;

/* A pair of needles delimiting one expression's scope: '(' ... ')'. */
typedef struct NeedleExpression
{
    Needle start;
    Needle end;
    int depth;
} NeedleExpression;

typedef struct Highlight
{
    size_t begin;   /* inclusive */
    size_t end;     /* exclusive */
    Rgb color;
} Highlight;

/* Each entry is its own heap block of EXPR_STACK_ENTRY_SIZE chars.
   Evaluation pops from `base` (the first expression pushed); popped
   entries are freed right away. */
typedef struct ExprStack
{
    char* items[EXPR_STACK_MAX];
    size_t base;
    size_t count;
} ExprStack;

typedef struct ValueStack
{
    double items[EXPR_VALUE_STACK_MAX];
    size_t count;
} ValueStack;

typedef struct Evaluator
{
    const char* input;

    char* buf;          /* working text, always '\0'-terminated */
    size_t len;
    size_t cap;
    char* scratch;      /* destination buffer while a phase rewrites buf */

    size_t opCount;

    NeedleExpression* groups;
    size_t groupCount;
    int maxDepth;

    ExprStack exprStack;
    ValueStack values;
    double result;

    const char* errText;
    size_t errLen;
    size_t errPos;
    int hasErrPos;
} Evaluator;

/* Everything one visual step shows. */
typedef struct Frame
{
    const char* label;
    const char* text;
    size_t textLen;

    const char* label2;
    const char* text2;
    size_t text2Len;

    Needle needle;
    int hasNeedle;
    Needle needle2;
    int hasNeedle2;

    Highlight hl[3];
    int hlCount;

    int depthRows;      /* draw the NeedleExpression rows under the text */
    size_t depthUpto;   /* only parens before this index are drawn */

    long exprMark;      /* expr stack index to highlight, -1 for none */
    long valueMark;     /* value stack index to highlight, -1 for none */

    int isError;
    const char* message;
} Frame;

typedef struct Viz
{
    StepMode mode;
    int trace;          /* --trace: plain log, no VT, no waiting */
    int autoDelayMs;    /* 0 = full speed */
    int depthContent;   /* EXPR_DEPTH_ROWS_CONTENT, or --depth-content */
    int hideBrackets;   /* EXPR_DEPTH_ROWS_HIDE_BRACKETS, or --depth-hide-brackets */
    unsigned long step;
    ExprPhase phase;
    const char* input;
    const ExprStack* exprStack;
    const ValueStack* valueStack;
} Viz;

#endif /* EXPR_EVALUATOR_H */
