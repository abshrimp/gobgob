/*
 * Quick sanity tests — no DB required.
 *
 * 1. Encode/decode round-trip on every one of the 1423 single-size
 *    configs.
 * 2. Spot-check generate_moves on the empty board: 6 placement squares
 *    per size for the corners/center, etc., 27 places-from-hand total
 *    for a never-touched board (3 sizes × 9 squares).
 * 3. Spot-check predecessor enumeration on a position reached by a
 *    single placement.
 */

#include "gobblet.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define ASSERT(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } \
} while (0)

static void test_config_roundtrip(void)
{
    int seen[CFG_PER_SIZE] = {0};
    int count = 0;
    for (int p = 0; p < POW3_9; p++) {
        int id = config_to_id(p);
        if (id < 0) continue;
        ASSERT(id_to_config((cfg_id_t)id) == p, "round-trip cfg id=%d", id);
        ASSERT(seen[id] == 0, "duplicate id=%d", id);
        seen[id] = 1;
        count++;
    }
    ASSERT(count == CFG_PER_SIZE, "count = %d, expected %d", count, CFG_PER_SIZE);
}

static void test_board_roundtrip(void)
{
    /* Empty board */
    board_t b = {{{0}}};
    pos_id_t id = board_to_id(&b);
    ASSERT(id == 0, "empty board id=%llu", (unsigned long long)id);
    board_t b2;
    id_to_board(id, &b2);
    ASSERT(memcmp(&b, &b2, sizeof b) == 0, "empty round-trip");

    /* Random-ish boards */
    for (int trial = 0; trial < 50; trial++) {
        board_t r; memset(&r, 0, sizeof r);
        int cp_count[3] = {0,0,0};
        int op_count[3] = {0,0,0};
        for (int s = 0; s < N_SIZES; s++) {
            for (int sq = 0; sq < N_SQUARES; sq++) {
                int v = rand() % 4;
                if (v == 1 && cp_count[s] < 2) { r.cells[s][sq] = CELL_CP; cp_count[s]++; }
                else if (v == 2 && op_count[s] < 2) { r.cells[s][sq] = CELL_OP; op_count[s]++; }
            }
        }
        pos_id_t pid = board_to_id(&r);
        board_t r2;
        id_to_board(pid, &r2);
        ASSERT(memcmp(&r, &r2, sizeof r) == 0, "random round-trip trial=%d", trial);
    }
}

static void test_initial_moves(void)
{
    board_t b; memset(&b, 0, sizeof b);
    move_t mv[MAX_LEGAL_MOVES];
    int wc;
    int n = generate_moves(&b, mv, &wc);
    /* 3 sizes × 9 squares = 27 placements from an empty board */
    ASSERT(n == 27, "empty board moves = %d (expected 27)", n);
    ASSERT(wc == 0, "winning_count = %d (expected 0)", wc);
}

static void test_three_in_a_row(void)
{
    board_t b; memset(&b, 0, sizeof b);
    /* Top row of small-CP pieces */
    b.cells[0][0] = CELL_CP;
    b.cells[0][1] = CELL_CP;
    b.cells[0][2] = CELL_CP;
    /* Wait: only 2 small CP pieces are allowed, and we just placed three.
       This is for the row-detection test only — board may be illegal. */
    ASSERT(has_three_in_a_row(&b, CELL_CP), "row detection");

    /* Now overlay an OP medium on square 1 — should hide the CP small. */
    b.cells[1][1] = CELL_OP;
    ASSERT(!has_three_in_a_row(&b, CELL_CP), "row hidden by larger OP");
}

static void test_predecessor_basic(void)
{
    /* Take an empty board, play "place small at sq 4" forward, then
     * confirm that the empty board appears as a predecessor of the
     * resulting (post-swap) position. */
    board_t a; memset(&a, 0, sizeof a);
    board_t after;
    move_t m = { .from = -1, .to = 4, .size = 0 };
    apply_move_next(&a, &m, &after);  /* applies + colour-swaps */

    board_t pre[256];
    int n = generate_predecessors(&after, pre, 256);
    int found_empty = 0;
    for (int i = 0; i < n; i++) {
        int empty = 1;
        for (int s = 0; s < N_SIZES && empty; s++)
            for (int sq = 0; sq < N_SQUARES && empty; sq++)
                if (pre[i].cells[s][sq] != CELL_EMPTY) empty = 0;
        if (empty) { found_empty = 1; break; }
    }
    ASSERT(found_empty, "empty board appears as predecessor (n=%d)", n);
}

int main(void)
{
    position_init();

    test_config_roundtrip();
    test_board_roundtrip();
    test_initial_moves();
    test_three_in_a_row();
    test_predecessor_basic();

    if (fails) {
        fprintf(stderr, "%d test(s) failed.\n", fails);
        return 1;
    }
    printf("All tests passed.\n");
    return 0;
}
