/*
 * Gobblet Gobblers complete-analysis library.
 *
 * Position representation
 * -----------------------
 * - 3x3 board, three sizes (small/medium/large).
 * - Each player owns 2 pieces of each size (6 total per player).
 * - Player-swap symmetric encoding: the side to move is always "CP"
 *   (current player), the other is "OP" (opponent). When a move is made
 *   the colours are swapped so the next position is again "CP to move".
 *
 * Position id
 * -----------
 * For a single size there are 1423 valid colour configurations on 9
 * squares with at most 2 of each colour.  A full position is
 *   id = small_id * 1423 * 1423 + medium_id * 1423 + large_id
 * giving 1423^3 = 2,881,473,967 ids in total.
 */

#ifndef GOBBLET_H
#define GOBBLET_H

#include <stdint.h>
#include <stdio.h>

#define N_SIZES   3
#define N_SQUARES 9
#define POW3_9    19683
#define CFG_PER_SIZE 1423

#define CELL_EMPTY 0
#define CELL_CP    1   /* current player */
#define CELL_OP    2   /* opponent       */

typedef uint64_t pos_id_t;
typedef uint16_t cfg_id_t;

#define TOTAL_POSITIONS \
    ((pos_id_t)CFG_PER_SIZE * (pos_id_t)CFG_PER_SIZE * (pos_id_t)CFG_PER_SIZE)

/* --- Status byte stored per position ---------------------------------- */
/* 0   = unknown / draw candidate                                        */
/* 1   = impossible (both players have a visible 3-in-a-row, etc.)       */
/* 2   = draw (set after analysis converges)                             */
/* 100 + d = WIN  with distance d to the terminal  (d in 0..127)         */
/* 200 + d = LOSS with distance d to the terminal  (d in 0..55)          */
#define ST_UNKNOWN     0
#define ST_IMPOSSIBLE  1
#define ST_DRAW        2
#define ST_WIN_BASE    100
#define ST_LOSS_BASE   200
#define ST_WIN(d)      (uint8_t)(ST_WIN_BASE  + (d))
#define ST_LOSS(d)     (uint8_t)(ST_LOSS_BASE + (d))

#define IS_WIN(s)   ((s) >= ST_WIN_BASE  && (s) < ST_WIN_BASE  + 100)
#define IS_LOSS(s)  ((s) >= ST_LOSS_BASE)
#define WIN_DIST(s)  ((s) - ST_WIN_BASE)
#define LOSS_DIST(s) ((s) - ST_LOSS_BASE)

#define MAX_LEGAL_MOVES 64

typedef struct {
    /* cells[size][square]: CELL_EMPTY / CELL_CP / CELL_OP */
    uint8_t cells[N_SIZES][N_SQUARES];
} board_t;

typedef struct {
    int8_t  from;   /* -1 = from hand           */
    int8_t  to;     /* -1 = pickup-only (rare)  */
    int8_t  size;   /* 0 small, 1 medium, 2 large */
} move_t;

/* --- position.c ------------------------------------------------------- */

void      position_init(void);
int       config_to_id(int packed);     /* packed in 0..3^9-1; -1 if invalid */
int       id_to_config(cfg_id_t id);

pos_id_t  board_to_id(const board_t *b);
void      id_to_board(pos_id_t id, board_t *b);

void      board_swap_colors(board_t *b);
int       top_piece(const board_t *b, int sq, int *out_size);
int       has_three_in_a_row(const board_t *b, int player);
int       hand_count(const board_t *b, int player, int size);
int       count_color(const board_t *b, int player);

/* --- moves.c ---------------------------------------------------------- */

/*
 * Generate all *legal* forward moves from `b` (assuming neither side
 * already has a visible three-in-a-row, i.e. `b` is non-terminal).
 *
 * A move is legal iff
 *   - it respects piece-stacking rules,
 *   - moving (picking up) the source piece does NOT expose either
 *     player's visible three-in-a-row before placing the piece.
 *
 * Moves that deliver a winning row (CP completes a row by placing) are
 * included.  The number of such winning moves is returned via
 * `winning_count` (may be NULL).
 *
 * Returns the number of moves stored in `moves` (at most MAX_LEGAL_MOVES).
 */
int generate_moves(const board_t *b, move_t *moves, int *winning_count);

/*
 * Apply `m` to `b` *without* swapping colours.  Sets *was_terminal=1 if
 * the resulting board is terminal (someone has a three-in-a-row).
 */
void apply_move(board_t *b, const move_t *m, int *was_terminal);

/*
 * Compute the *next* position id reached after CP plays `m`.  This
 * applies the move and swaps colours so the result is in "next side to
 * move" perspective.  Stores the next board in *out_next.
 *
 * Return value: ST_LOSS(0) if the move ends the game with CP winning
 * (the next position is therefore terminal LOSS for its CP),
 * ST_WIN(0)  if the move ends with CP losing (e.g. pickup-exposes-OP-row),
 * 0 (ST_UNKNOWN) otherwise.
 */
uint8_t apply_move_next(const board_t *b, const move_t *m, board_t *out_next);

/*
 * Backward (predecessor) move generation.
 *
 * Given a non-terminal position `b`, enumerate every (PRE, move) such
 * that PRE makes `move` to produce `b` (after colour swap).  PRE is
 * stored as a board in CP-to-move perspective.
 *
 * `out_pre`   array of board_t with capacity `cap`
 * Returns number of predecessors written.
 */
int generate_predecessors(const board_t *b, board_t *out_pre, int cap);

/* --- I/O helpers (used by query) ------------------------------------- */

void board_print(const board_t *b, FILE *fp);
int  parse_board(const char *board_str, const char *hand_str,
                 int turn_is_cp_orange, board_t *out);
int  format_move(const move_t *m, char *buf, size_t cap);

#endif /* GOBBLET_H */
