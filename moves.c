#include "gobblet.h"
#include <string.h>

/*
 * Forward move generation.
 *
 * Returns the number of *legal* moves for the current player.  A move is
 * "legal" iff it can be made without immediately handing the game to the
 * opponent — concretely, picking up a piece on the board must not expose
 * either player's three-in-a-row (we exclude such pickup-triggers because
 * they are end-of-game branches that don't lead to a normal next turn).
 *
 * Both winning placements (which complete CP's row) and ordinary moves are
 * included; their count is returned via *winning_count.
 */
int generate_moves(const board_t *b, move_t *moves, int *winning_count)
{
    int n = 0, wc = 0;

    /* (1) place from hand */
    for (int s = 0; s < N_SIZES; s++) {
        if (hand_count(b, CELL_CP, s) == 0) continue;
        for (int sq = 0; sq < N_SQUARES; sq++) {
            int top_sz, top = top_piece(b, sq, &top_sz);
            if (top != CELL_EMPTY && top_sz >= s) continue;
            board_t nb = *b;
            nb.cells[s][sq] = CELL_CP;
            int wins = has_three_in_a_row(&nb, CELL_CP);
            /* OP cannot get a row from a CP placement (CP is on top here). */
            if (n >= MAX_LEGAL_MOVES) return n;
            moves[n].from = -1;
            moves[n].to   = (int8_t)sq;
            moves[n].size = (int8_t)s;
            n++;
            if (wins) wc++;
        }
    }

    /* (2) move a piece on the board */
    for (int from = 0; from < N_SQUARES; from++) {
        int top_sz, top = top_piece(b, from, &top_sz);
        if (top != CELL_CP) continue;

        board_t after_pick = *b;
        after_pick.cells[top_sz][from] = CELL_EMPTY;
        if (has_three_in_a_row(&after_pick, CELL_OP)) continue; /* losing pickup */
        if (has_three_in_a_row(&after_pick, CELL_CP)) continue; /* rare pickup-win, skip */

        for (int to = 0; to < N_SQUARES; to++) {
            if (to == from) continue;
            int dsz, dtop = top_piece(&after_pick, to, &dsz);
            if (dtop != CELL_EMPTY && dsz >= top_sz) continue;
            board_t nb = after_pick;
            nb.cells[top_sz][to] = CELL_CP;
            int wins = has_three_in_a_row(&nb, CELL_CP);
            if (n >= MAX_LEGAL_MOVES) return n;
            moves[n].from = (int8_t)from;
            moves[n].to   = (int8_t)to;
            moves[n].size = (int8_t)top_sz;
            n++;
            if (wins) wc++;
        }
    }

    if (winning_count) *winning_count = wc;
    return n;
}

void apply_move(board_t *b, const move_t *m, int *was_terminal)
{
    if (m->from < 0) {
        b->cells[m->size][m->to] = CELL_CP;
    } else {
        b->cells[m->size][m->from] = CELL_EMPTY;
        if (m->to >= 0)
            b->cells[m->size][m->to] = CELL_CP;
    }
    if (was_terminal)
        *was_terminal = has_three_in_a_row(b, CELL_CP) ||
                        has_three_in_a_row(b, CELL_OP);
}

uint8_t apply_move_next(const board_t *b, const move_t *m, board_t *out_next)
{
    *out_next = *b;
    if (m->from < 0) {
        out_next->cells[m->size][m->to] = CELL_CP;
    } else {
        out_next->cells[m->size][m->from] = CELL_EMPTY;
        if (m->to >= 0)
            out_next->cells[m->size][m->to] = CELL_CP;
    }
    int cp_won = has_three_in_a_row(out_next, CELL_CP);
    int op_won = has_three_in_a_row(out_next, CELL_OP);
    board_swap_colors(out_next);

    if (cp_won && op_won) return ST_IMPOSSIBLE;
    if (cp_won) return ST_LOSS(0);   /* next pos: OP-of-next has row */
    if (op_won) return ST_WIN(0);    /* next pos: CP-of-next has row */
    return 0;
}

/*
 * Backward (predecessor) move generation.
 *
 * Inverse of `generate_moves`.  Given a non-terminal position `b` (in
 * "CP-to-move" perspective), enumerate every PRE such that some legal
 * forward move from PRE produces `b` after the colour-swap.  Each
 * returned PRE is itself in CP-to-move perspective (its CP = the player
 * who just moved to reach `b`).
 *
 * Returns the number of predecessors written into `out_pre` (capped at
 * `cap`).  Validity rules mirror `generate_moves`:
 *   - PRE has no visible three-in-a-row for either side.
 *   - For un-move, the intermediate "piece-just-lifted" state has no
 *     visible row either.
 *
 * Note: this routine does NOT enumerate predecessors that would have
 * required a pickup-exposes-row move, since such moves are excluded
 * from `generate_moves` — keeping the two routines symmetric.
 */
int generate_predecessors(const board_t *b, board_t *out_pre, int cap)
{
    int n = 0;
    /* Q = colour-swap(P).  In Q, the previous mover is CP. */
    board_t Q = *b;
    board_swap_colors(&Q);

    /* (Optional fast reject — these can't be normal-move successors.) */
    if (has_three_in_a_row(&Q, CELL_OP)) return 0;

    for (int s = 0; s < N_SQUARES; s++) {
        int top_sz, top = top_piece(&Q, s, &top_sz);
        if (top != CELL_CP) continue;
        int sz = top_sz;

        /* (a) un-place : piece was placed from hand */
        {
            board_t pre = Q;
            pre.cells[sz][s] = CELL_EMPTY;
            if (!has_three_in_a_row(&pre, CELL_CP) &&
                !has_three_in_a_row(&pre, CELL_OP)) {
                if (n < cap) out_pre[n++] = pre;
            }
        }

        /* (b) un-move : piece moved from `f` to `s` */
        for (int f = 0; f < N_SQUARES; f++) {
            if (f == s) continue;
            if (Q.cells[sz][f] != CELL_EMPTY) continue;
            int ok = 1;
            for (int t = sz + 1; t < N_SIZES; t++) {
                if (Q.cells[t][f] != CELL_EMPTY) { ok = 0; break; }
                if (Q.cells[t][s] != CELL_EMPTY) { ok = 0; break; }
            }
            if (!ok) continue;

            board_t I = Q;
            I.cells[sz][s] = CELL_EMPTY;
            if (has_three_in_a_row(&I, CELL_CP)) continue;
            if (has_three_in_a_row(&I, CELL_OP)) continue;
            board_t pre = I;
            pre.cells[sz][f] = CELL_CP;
            if (has_three_in_a_row(&pre, CELL_CP)) continue;
            if (has_three_in_a_row(&pre, CELL_OP)) continue;

            if (n < cap) out_pre[n++] = pre;
            else return n;
        }
    }
    return n;
}
