#include "gobblet.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* --- encoding tables ------------------------------------------------- */

static int   g_cfg_to_id[POW3_9];
static int   g_id_to_cfg[CFG_PER_SIZE];
static int   g_pow3[N_SQUARES + 1];

/*
 * mask_to_packed[m] = sum_{bit b set in m} 3^b   (for b in 0..8)
 *
 * Lets us convert (cp_mask, op_mask) -> packed config via
 *   packed = mask_to_packed[cp_mask] + 2 * mask_to_packed[op_mask]
 * (since CP contributes 1·3^b per bit, OP contributes 2·3^b per bit).
 */
static uint32_t g_mask_to_packed[512];

/*
 * cfg_to_masks[id] = (cp_mask, op_mask) decoded from packed config.
 * Used by id_to_board.
 */
typedef struct { uint16_t cp; uint16_t op; } masks16_t;
static masks16_t g_cfg_to_masks[CFG_PER_SIZE];

static int g_initialized = 0;

void position_init(void)
{
    if (g_initialized) return;

    g_pow3[0] = 1;
    for (int i = 1; i <= N_SQUARES; i++) g_pow3[i] = g_pow3[i - 1] * 3;

    /* Build mask_to_packed for 9-bit masks. */
    for (int m = 0; m < 512; m++) {
        uint32_t s = 0;
        for (int b = 0; b < 9; b++)
            if (m & (1 << b)) s += g_pow3[b];
        g_mask_to_packed[m] = s;
    }

    /* Build packed <-> id and (id -> masks) tables. */
    int next = 0;
    for (int p = 0; p < POW3_9; p++) {
        int cp = 0, op = 0, q = p;
        uint16_t cp_mask = 0, op_mask = 0;
        for (int i = 0; i < N_SQUARES; i++) {
            int v = q % 3; q /= 3;
            if (v == CELL_CP)      { cp++; cp_mask |= (uint16_t)(1u << i); }
            else if (v == CELL_OP) { op++; op_mask |= (uint16_t)(1u << i); }
        }
        if (cp <= 2 && op <= 2) {
            g_cfg_to_id[p]            = next;
            g_id_to_cfg[next]         = p;
            g_cfg_to_masks[next].cp   = cp_mask;
            g_cfg_to_masks[next].op   = op_mask;
            next++;
        } else {
            g_cfg_to_id[p] = -1;
        }
    }
    if (next != CFG_PER_SIZE) {
        fprintf(stderr, "FATAL: configs/size = %d, expected %d\n",
                next, CFG_PER_SIZE);
        exit(1);
    }
    g_initialized = 1;
}

int config_to_id(int packed) { return g_cfg_to_id[packed]; }
int id_to_config(cfg_id_t id) { return g_id_to_cfg[id]; }

/* --- core conversions ----------------------------------------------- */

pos_id_t board_to_id(const board_t *b)
{
    pos_id_t id = 0;
    for (int s = 0; s < N_SIZES; s++) {
        uint32_t packed = g_mask_to_packed[b->cp[s]] + 2 * g_mask_to_packed[b->op[s]];
        id = id * CFG_PER_SIZE + (cfg_id_t)g_cfg_to_id[packed];
    }
    return id;
}

void id_to_board(pos_id_t id, board_t *b)
{
    for (int s = N_SIZES - 1; s >= 0; s--) {
        cfg_id_t cid = (cfg_id_t)(id % CFG_PER_SIZE);
        id /= CFG_PER_SIZE;
        b->cp[s] = g_cfg_to_masks[cid].cp;
        b->op[s] = g_cfg_to_masks[cid].op;
    }
}

void board_swap_colors(board_t *b)
{
    for (int s = 0; s < N_SIZES; s++) {
        uint16_t t = b->cp[s];
        b->cp[s] = b->op[s];
        b->op[s] = t;
    }
}

int top_piece(const board_t *b, int sq, int *out_size)
{
    uint16_t bit = (uint16_t)(1u << sq);
    for (int s = N_SIZES - 1; s >= 0; s--) {
        if (b->cp[s] & bit) { if (out_size) *out_size = s; return CELL_CP; }
        if (b->op[s] & bit) { if (out_size) *out_size = s; return CELL_OP; }
    }
    if (out_size) *out_size = -1;
    return CELL_EMPTY;
}

int has_three_in_a_row(const board_t *b, int player)
{
    uint16_t top_cp, top_op;
    board_tops(b, &top_cp, &top_op, NULL, NULL);
    return line_complete(player == CELL_CP ? top_cp : top_op);
}

int hand_count(const board_t *b, int player, int size)
{
    uint16_t m = (player == CELL_CP) ? b->cp[size] : b->op[size];
    return 2 - __builtin_popcount(m);
}

int count_color(const board_t *b, int player)
{
    int c = 0;
    for (int s = 0; s < N_SIZES; s++)
        c += __builtin_popcount(player == CELL_CP ? b->cp[s] : b->op[s]);
    return c;
}

/* ---------------- I/O helpers --------------------------------------- */

void board_print(const board_t *b, FILE *fp)
{
    static const char glyph_cp[3] = { 's', 'm', 'l' };
    static const char glyph_op[3] = { 'S', 'M', 'L' };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int sq = row * 3 + col;
            uint16_t bit = (uint16_t)(1u << sq);
            for (int s = 0; s < N_SIZES; s++) {
                char c = '-';
                if (b->cp[s] & bit) c = glyph_cp[s];
                else if (b->op[s] & bit) c = glyph_op[s];
                fputc(c, fp);
            }
            fputc(' ', fp);
        }
        fputc('\n', fp);
    }
    fprintf(fp, "  CP hand: s=%d m=%d l=%d\n",
            hand_count(b, CELL_CP, 0),
            hand_count(b, CELL_CP, 1),
            hand_count(b, CELL_CP, 2));
    fprintf(fp, "  OP hand: s=%d m=%d l=%d\n",
            hand_count(b, CELL_OP, 0),
            hand_count(b, CELL_OP, 1),
            hand_count(b, CELL_OP, 2));
}

int parse_board(const char *board_str, const char *hand_str,
                int turn_is_cp_orange, board_t *out)
{
    (void)hand_str;
    memset(out, 0, sizeof *out);

    int sq = 0;
    const char *p = board_str;
    while (*p && sq < N_SQUARES) {
        if (isspace((unsigned char)*p) || *p == '/' || *p == '|') { p++; continue; }
        int triple_n = 0;
        char tri[3] = {'-', '-', '-'};
        while (*p && triple_n < 3 &&
               !isspace((unsigned char)*p) && *p != '/' && *p != '|') {
            tri[triple_n++] = *p++;
        }
        uint16_t bit = (uint16_t)(1u << sq);
        for (int s = 0; s < 3; s++) {
            char c = tri[s];
            int expected_size =
                (c == 's' || c == 'S') ? 0 :
                (c == 'm' || c == 'M') ? 1 :
                (c == 'l' || c == 'L') ? 2 : -1;
            switch (c) {
            case '-': case '.': case '0': break;
            case 's': case 'm': case 'l':
                if (expected_size != s) {
                    fprintf(stderr, "parse: glyph %c at slot %d\n", c, s);
                    return -1;
                }
                if (turn_is_cp_orange) out->cp[s] |= bit;
                else                   out->op[s] |= bit;
                break;
            case 'S': case 'M': case 'L':
                if (expected_size != s) {
                    fprintf(stderr, "parse: glyph %c at slot %d\n", c, s);
                    return -1;
                }
                if (turn_is_cp_orange) out->op[s] |= bit;
                else                   out->cp[s] |= bit;
                break;
            default:
                fprintf(stderr, "parse: bad glyph '%c'\n", c);
                return -1;
            }
        }
        sq++;
    }
    if (sq != N_SQUARES) {
        fprintf(stderr, "parse: only %d squares supplied\n", sq);
        return -1;
    }
    for (int s = 0; s < N_SIZES; s++) {
        if (__builtin_popcount(out->cp[s]) > 2 ||
            __builtin_popcount(out->op[s]) > 2) {
            fprintf(stderr, "parse: too many size-%d pieces\n", s);
            return -1;
        }
    }
    return 0;
}

int format_move(const move_t *m, char *buf, size_t cap)
{
    static const char *sz_name = "sml";
    if (m->from < 0 && m->to >= 0)
        return snprintf(buf, cap, "place %c -> sq%d",
                        sz_name[m->size], m->to);
    if (m->to < 0 && m->from >= 0)
        return snprintf(buf, cap, "lift %c from sq%d (game-ending)",
                        sz_name[m->size], m->from);
    return snprintf(buf, cap, "move %c sq%d -> sq%d",
                    sz_name[m->size], m->from, m->to);
}
