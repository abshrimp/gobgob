#include "gobblet.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static int  g_cfg_to_id[POW3_9];
static int  g_id_to_cfg[CFG_PER_SIZE];
static int  g_pow3[N_SQUARES + 1];
static int  g_initialized = 0;

void position_init(void)
{
    if (g_initialized) return;

    g_pow3[0] = 1;
    for (int i = 1; i <= N_SQUARES; i++) g_pow3[i] = g_pow3[i - 1] * 3;

    int next = 0;
    for (int p = 0; p < POW3_9; p++) {
        int cp = 0, op = 0, q = p;
        for (int i = 0; i < N_SQUARES; i++) {
            int v = q % 3; q /= 3;
            if (v == CELL_CP) cp++;
            else if (v == CELL_OP) op++;
        }
        if (cp <= 2 && op <= 2) {
            g_cfg_to_id[p]   = next;
            g_id_to_cfg[next] = p;
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

pos_id_t board_to_id(const board_t *b)
{
    pos_id_t id = 0;
    for (int s = 0; s < N_SIZES; s++) {
        int packed = 0;
        for (int sq = N_SQUARES - 1; sq >= 0; sq--)
            packed = packed * 3 + b->cells[s][sq];
        id = id * CFG_PER_SIZE + (cfg_id_t)g_cfg_to_id[packed];
    }
    return id;
}

void id_to_board(pos_id_t id, board_t *b)
{
    for (int s = N_SIZES - 1; s >= 0; s--) {
        cfg_id_t cid = (cfg_id_t)(id % CFG_PER_SIZE);
        id /= CFG_PER_SIZE;
        int packed = g_id_to_cfg[cid];
        for (int sq = 0; sq < N_SQUARES; sq++) {
            b->cells[s][sq] = (uint8_t)(packed % 3);
            packed /= 3;
        }
    }
}

void board_swap_colors(board_t *b)
{
    for (int s = 0; s < N_SIZES; s++)
        for (int sq = 0; sq < N_SQUARES; sq++) {
            uint8_t v = b->cells[s][sq];
            if (v == CELL_CP) b->cells[s][sq] = CELL_OP;
            else if (v == CELL_OP) b->cells[s][sq] = CELL_CP;
        }
}

int top_piece(const board_t *b, int sq, int *out_size)
{
    for (int s = N_SIZES - 1; s >= 0; s--) {
        if (b->cells[s][sq] != CELL_EMPTY) {
            if (out_size) *out_size = s;
            return b->cells[s][sq];
        }
    }
    if (out_size) *out_size = -1;
    return CELL_EMPTY;
}

static const int LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},
    {0,3,6},{1,4,7},{2,5,8},
    {0,4,8},{2,4,6}
};

int has_three_in_a_row(const board_t *b, int player)
{
    for (int l = 0; l < 8; l++) {
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            int sz; if (top_piece(b, LINES[l][k], &sz) != player) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

int hand_count(const board_t *b, int player, int size)
{
    int c = 0;
    for (int sq = 0; sq < N_SQUARES; sq++)
        if (b->cells[size][sq] == player) c++;
    return 2 - c;
}

int count_color(const board_t *b, int player)
{
    int c = 0;
    for (int s = 0; s < N_SIZES; s++)
        for (int sq = 0; sq < N_SQUARES; sq++)
            if (b->cells[s][sq] == player) c++;
    return c;
}

/* ---------------- I/O helpers --------------------------------------- */

void board_print(const board_t *b, FILE *fp)
{
    /* Print 3 rows; each square shows three glyphs (small/med/large)
     * with '-' for empty, lower for CP, upper for OP. */
    static const char glyph_cp[3] = { 's', 'm', 'l' };
    static const char glyph_op[3] = { 'S', 'M', 'L' };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int sq = row * 3 + col;
            for (int s = 0; s < N_SIZES; s++) {
                int v = b->cells[s][sq];
                fputc(v == CELL_CP ? glyph_cp[s]
                       : v == CELL_OP ? glyph_op[s] : '-', fp);
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

/*
 * Parse a board string of 9 squares.  Each square is three characters,
 * one for small, medium, large piece in that order.  Squares are
 * separated by spaces and rows by '/' or whitespace.  '-' means empty.
 *
 * Glyphs:  's' 'm' 'l' = orange piece; 'S' 'M' 'L' = blue piece.
 *
 * `hand_str` is optional (may be NULL) — when given, it is six
 * decimals "o_s o_m o_l b_s b_m b_l" of pieces left in hand.  If NULL,
 * hand counts are derived from the board (assumes both players started
 * with 2 of each size).
 *
 * `turn_is_cp_orange` = 1 if it is orange's turn, 0 if blue's turn.
 *
 * The returned board is in CP-to-move encoding (CP=orange if
 * turn_is_cp_orange=1, else CP=blue with colours swapped).
 */
int parse_board(const char *board_str, const char *hand_str,
                int turn_is_cp_orange, board_t *out)
{
    (void)hand_str;  /* hand is derived from the board */
    memset(out, 0, sizeof *out);

    int sq = 0;
    const char *p = board_str;
    while (*p && sq < N_SQUARES) {
        if (isspace((unsigned char)*p) || *p == '/' || *p == '|') { p++; continue; }
        /* read up to three glyphs for this square */
        int triple_n = 0;
        char tri[3] = {'-', '-', '-'};
        while (*p && triple_n < 3 &&
               !isspace((unsigned char)*p) && *p != '/' && *p != '|') {
            tri[triple_n++] = *p++;
        }
        for (int s = 0; s < 3; s++) {
            char c = tri[s];
            switch (c) {
            case '-': case '.': case '0': out->cells[s][sq] = CELL_EMPTY; break;
            case 's': case 'm': case 'l':
                out->cells[s][sq] =
                    (turn_is_cp_orange ? CELL_CP : CELL_OP);
                if ((c == 's' && s != 0) || (c == 'm' && s != 1) || (c == 'l' && s != 2)) {
                    fprintf(stderr, "parse: glyph %c at slot %d\n", c, s);
                    return -1;
                }
                break;
            case 'S': case 'M': case 'L':
                out->cells[s][sq] =
                    (turn_is_cp_orange ? CELL_OP : CELL_CP);
                if ((c == 'S' && s != 0) || (c == 'M' && s != 1) || (c == 'L' && s != 2)) {
                    fprintf(stderr, "parse: glyph %c at slot %d\n", c, s);
                    return -1;
                }
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
    /* Sanity: at most 2 of each colour/size on board. */
    for (int s = 0; s < N_SIZES; s++) {
        int cp_n = 0, op_n = 0;
        for (int q = 0; q < N_SQUARES; q++) {
            if (out->cells[s][q] == CELL_CP) cp_n++;
            else if (out->cells[s][q] == CELL_OP) op_n++;
        }
        if (cp_n > 2 || op_n > 2) {
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
