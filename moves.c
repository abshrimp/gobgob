#include "gobblet.h"
#include <string.h>

/*
 * Forward move generation (bitboard).
 *
 * Returns the number of legal moves; out-parameter `winning_count` is
 * set to the number of placements/movements that complete a CP three.
 *
 * A move is excluded if executing the pickup phase exposes EITHER
 * player's three-in-a-row.  This keeps the move set perfectly symmetric
 * with `generate_predecessors`.
 */
int generate_moves(const board_t *b, move_t *moves, int *winning_count)
{
    int n = 0, wc = 0;

    uint16_t top_cp, top_op, mn_l, mn_lm;
    board_tops(b, &top_cp, &top_op, &mn_l, &mn_lm);
    uint16_t mn_lms = (uint16_t)(mn_lm & ~(b->cp[0] | b->op[0]));  /* fully empty */

    /* Squares CP currently has its own piece on top of, by size */
    uint16_t top_cp_by_sz[3];
    top_cp_by_sz[2] = b->cp[2];
    top_cp_by_sz[1] = (uint16_t)(b->cp[1] & mn_l);
    top_cp_by_sz[0] = (uint16_t)(b->cp[0] & mn_lm);

    /* Where can a piece of size sz be placed?  (top of dest must be smaller.) */
    uint16_t can_place[3];
    can_place[0] = mn_lms;   /* small  : square must be entirely empty  */
    can_place[1] = mn_lm;    /* medium : at most a small piece may sit  */
    can_place[2] = mn_l;     /* large  : at most medium underneath      */

    /* (1) place from hand ---------------------------------------- */
    for (int s = 0; s < N_SIZES; s++) {
        if (__builtin_popcount(b->cp[s]) >= 2) continue;   /* hand empty */
        for (uint16_t m = can_place[s]; m; m &= (uint16_t)(m - 1)) {
            int sq = __builtin_ctz(m);
            board_t nb = *b;
            nb.cp[s] |= (uint16_t)(1u << sq);
            int wins = has_three_in_a_row(&nb, CELL_CP);
            if (n >= MAX_LEGAL_MOVES) goto done;
            moves[n].from = -1;
            moves[n].to   = (int8_t)sq;
            moves[n].size = (int8_t)s;
            n++;
            if (wins) wc++;
        }
    }

    /* (2) move a piece on the board ------------------------------ */
    for (int sz = 0; sz < N_SIZES; sz++) {
        for (uint16_t srcs = top_cp_by_sz[sz]; srcs; srcs &= (uint16_t)(srcs - 1)) {
            int from = __builtin_ctz(srcs);
            uint16_t bit_from = (uint16_t)(1u << from);

            board_t after = *b;
            after.cp[sz] &= (uint16_t)~bit_from;

            uint16_t a_top_cp, a_top_op, a_mn_l, a_mn_lm;
            board_tops(&after, &a_top_cp, &a_top_op, &a_mn_l, &a_mn_lm);
            if (line_complete(a_top_cp)) continue;   /* rare pickup-win — skip */
            if (line_complete(a_top_op)) continue;   /* losing pickup */

            uint16_t a_mn_lms = (uint16_t)(a_mn_lm & ~(after.cp[0] | after.op[0]));
            uint16_t dst_mask =
                (sz == 0) ? a_mn_lms :
                (sz == 1) ? a_mn_lm  : a_mn_l;
            dst_mask &= (uint16_t)~bit_from;   /* cannot land on the same square */

            for (uint16_t m = dst_mask; m; m &= (uint16_t)(m - 1)) {
                int to = __builtin_ctz(m);
                board_t nb = after;
                nb.cp[sz] |= (uint16_t)(1u << to);
                int wins = has_three_in_a_row(&nb, CELL_CP);
                if (n >= MAX_LEGAL_MOVES) goto done;
                moves[n].from = (int8_t)from;
                moves[n].to   = (int8_t)to;
                moves[n].size = (int8_t)sz;
                n++;
                if (wins) wc++;
            }
        }
    }

done:
    if (winning_count) *winning_count = wc;
    return n;
}

void apply_move(board_t *b, const move_t *m, int *was_terminal)
{
    if (m->from < 0) {
        b->cp[m->size] |= (uint16_t)(1u << m->to);
    } else {
        b->cp[m->size] &= (uint16_t)~(1u << m->from);
        if (m->to >= 0)
            b->cp[m->size] |= (uint16_t)(1u << m->to);
    }
    if (was_terminal)
        *was_terminal = has_three_in_a_row(b, CELL_CP) ||
                        has_three_in_a_row(b, CELL_OP);
}

uint8_t apply_move_next(const board_t *b, const move_t *m, board_t *out_next)
{
    *out_next = *b;
    if (m->from < 0) {
        out_next->cp[m->size] |= (uint16_t)(1u << m->to);
    } else {
        out_next->cp[m->size] &= (uint16_t)~(1u << m->from);
        if (m->to >= 0)
            out_next->cp[m->size] |= (uint16_t)(1u << m->to);
    }
    int cp_won = has_three_in_a_row(out_next, CELL_CP);
    int op_won = has_three_in_a_row(out_next, CELL_OP);
    board_swap_colors(out_next);

    if (cp_won && op_won) return ST_IMPOSSIBLE;
    if (cp_won) return ST_LOSS(0);
    if (op_won) return ST_WIN(0);
    return 0;
}

/*
 * Backward (predecessor) move generation (bitboard).
 *
 * Mirrors `generate_moves`: we work in Q = colour-swap(b), where the
 * previous mover (PRE_CP) is now CP.  For every square `s` whose top
 * piece is a CP-of-Q piece, two predecessor classes are considered:
 *
 *   (a) un-place : that piece returns to PRE_CP's hand.
 *   (b) un-move  : that piece originally sat on some other square `f`.
 *
 * Each PRE candidate is rejected unless it is non-terminal AND the
 * intermediate "piece-just-lifted" state is also non-terminal (mirroring
 * the pickup constraints in `generate_moves`).
 */
int generate_predecessors(const board_t *b, board_t *out_pre, int cap)
{
    int n = 0;

    /* Q = colour-swap of P. */
    board_t Q;
    for (int s = 0; s < N_SIZES; s++) { Q.cp[s] = b->op[s]; Q.op[s] = b->cp[s]; }

    /* Fast reject: OP-of-Q line means the move that produced P would
     * have been one of the excluded "pickup-exposes-row" forward moves. */
    {
        uint16_t top_cp_q, top_op_q;
        board_tops(&Q, &top_cp_q, &top_op_q, NULL, NULL);
        if (line_complete(top_op_q)) return 0;
    }

    /* Top-of-Q masks for CP-of-Q (PRE_mover) per size */
    uint16_t mn_l, mn_lm;
    board_tops(&Q, NULL, NULL, &mn_l, &mn_lm);
    uint16_t top_cp_by_sz[3];
    top_cp_by_sz[2] = Q.cp[2];
    top_cp_by_sz[1] = (uint16_t)(Q.cp[1] & mn_l);
    top_cp_by_sz[0] = (uint16_t)(Q.cp[0] & mn_lm);

    for (int sz = 0; sz < N_SIZES; sz++) {
        /* Mask of squares that already contain a piece >=sz in either colour
         * (a piece of size sz can't be on top there if anything larger sits). */
        uint16_t blocked_above = 0;
        for (int t = sz + 1; t < N_SIZES; t++)
            blocked_above |= (uint16_t)(Q.cp[t] | Q.op[t]);

        for (uint16_t mm = top_cp_by_sz[sz]; mm; mm &= (uint16_t)(mm - 1)) {
            int s = __builtin_ctz(mm);
            uint16_t bit_s = (uint16_t)(1u << s);

            /* (a) un-place ---------------------------------------- */
            {
                board_t pre = Q;
                pre.cp[sz] &= (uint16_t)~bit_s;
                uint16_t tcp, top;
                board_tops(&pre, &tcp, &top, NULL, NULL);
                if (!line_complete(tcp) && !line_complete(top)) {
                    if (n < cap) out_pre[n++] = pre;
                }
            }

            /* (b) un-move : piece originated from square `f` ------- */
            /* I = Q with piece-at-s removed (intermediate "piece in air") */
            board_t I = Q;
            I.cp[sz] &= (uint16_t)~bit_s;
            uint16_t i_top_cp, i_top_op;
            board_tops(&I, &i_top_cp, &i_top_op, NULL, NULL);
            int intermediate_bad = line_complete(i_top_cp) || line_complete(i_top_op);

            if (intermediate_bad) continue;

            /* Where could `f` have been?  An empty (size-sz) square with no
             * larger piece sitting on top, and f != s. */
            uint16_t can_f = (uint16_t)(~(blocked_above | Q.cp[sz] | Q.op[sz]) & 0x1FF);
            can_f &= (uint16_t)~bit_s;

            for (uint16_t fm = can_f; fm; fm &= (uint16_t)(fm - 1)) {
                int f = __builtin_ctz(fm);
                board_t pre = I;
                pre.cp[sz] |= (uint16_t)(1u << f);
                uint16_t p_top_cp, p_top_op;
                board_tops(&pre, &p_top_cp, &p_top_op, NULL, NULL);
                if (line_complete(p_top_cp) || line_complete(p_top_op)) continue;
                if (n < cap) out_pre[n++] = pre;
                else return n;
            }
        }
    }
    return n;
}
