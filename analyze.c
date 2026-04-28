/*
 * Retrograde-analysis main.
 *
 * Memory layout (about 6.5 GB total):
 *   result[N]      — 1 byte per position, 2.88 GB
 *   child_count[N] — 1 byte per position, 2.88 GB
 *   cur/nxt bitmap — 360 MB each
 *
 * The analyzer
 *   1. forward sweep: classify every position as WIN-0 / LOSS-0 / WIN-1 /
 *      IMPOSSIBLE, or undetermined-with-child_count.
 *   2. wave-by-wave BFS: each pass scans the "current" bitmap, calls
 *      generate_predecessors, and marks newly-determined positions in the
 *      "next" bitmap.  Swap.  Stop when no new positions appear.
 *   3. mark anything still UNKNOWN as DRAW.
 *   4. dump the byte-array to disk.
 */

#include "gobblet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BITMAP_BYTES ((TOTAL_POSITIONS + 7) / 8)

static inline void set_bit(uint8_t *bits, pos_id_t id) {
    bits[id >> 3] |= (uint8_t)(1u << (id & 7));
}

static uint8_t *result;
static uint8_t *child_count;
static uint8_t *cur_bits;
static uint8_t *nxt_bits;

/* Stats */
static long long g_terminal_loss = 0;
static long long g_terminal_win  = 0;
static long long g_impossible    = 0;
static long long g_win1          = 0;
static long long g_undet         = 0;
static long long g_stalemate     = 0;
static long long g_dist_count[256];

/* ---------------------------------------------------------------- */

static void forward_sweep(void)
{
    fprintf(stderr, "[forward] starting...\n");
    time_t t0 = time(NULL);

    pos_id_t step = TOTAL_POSITIONS / 50;
    pos_id_t prog = 0;

    move_t mv[MAX_LEGAL_MOVES];
    for (pos_id_t id = 0; id < TOTAL_POSITIONS; id++) {
        if (++prog >= step) {
            prog = 0;
            fprintf(stderr, "  %.1f%%\n",
                    100.0 * id / (double)TOTAL_POSITIONS);
        }

        board_t b;
        id_to_board(id, &b);

        int cp_row = has_three_in_a_row(&b, CELL_CP);
        int op_row = has_three_in_a_row(&b, CELL_OP);

        if (cp_row && op_row) {
            result[id] = ST_IMPOSSIBLE;
            g_impossible++;
            continue;
        }
        if (cp_row) {
            result[id] = ST_WIN(0);
            set_bit(cur_bits, id);
            g_terminal_win++;
            continue;
        }
        if (op_row) {
            result[id] = ST_LOSS(0);
            set_bit(cur_bits, id);
            g_terminal_loss++;
            continue;
        }

        int wc;
        int nm = generate_moves(&b, mv, &wc);

        if (wc > 0) {
            result[id] = ST_WIN(1);
            set_bit(cur_bits, id);
            g_win1++;
        } else if (nm == 0) {
            result[id] = ST_LOSS(0);
            set_bit(cur_bits, id);
            g_stalemate++;
        } else {
            child_count[id] = (nm > 255) ? 255 : (uint8_t)nm;
            g_undet++;
        }
    }

    time_t t1 = time(NULL);
    fprintf(stderr, "[forward] done in %ld s\n", (long)(t1 - t0));
    fprintf(stderr, "  WIN-0       : %lld\n", g_terminal_win);
    fprintf(stderr, "  LOSS-0(row) : %lld\n", g_terminal_loss);
    fprintf(stderr, "  LOSS-0(stm) : %lld\n", g_stalemate);
    fprintf(stderr, "  IMPOSSIBLE  : %lld\n", g_impossible);
    fprintf(stderr, "  WIN-1       : %lld\n", g_win1);
    fprintf(stderr, "  undetermined: %lld\n", g_undet);
}

/* ---------------------------------------------------------------- */

static long long g_pass_marks_win  = 0;
static long long g_pass_marks_loss = 0;

static void process_position(pos_id_t id)
{
    uint8_t s = result[id];
    if (s == ST_UNKNOWN || s == ST_IMPOSSIBLE) return;

    int dist;
    if (IS_WIN(s))      dist = WIN_DIST(s);
    else if (IS_LOSS(s)) dist = LOSS_DIST(s);
    else return;

    if (dist >= 99) return; /* avoid overflow */

    board_t b;
    id_to_board(id, &b);

    static board_t pre_buf[256];
    int n = generate_predecessors(&b, pre_buf, 256);
    if (n == 0) return;

    if (IS_LOSS(s)) {
        for (int i = 0; i < n; i++) {
            pos_id_t pid = board_to_id(&pre_buf[i]);
            if (result[pid] != ST_UNKNOWN) continue;
            result[pid] = ST_WIN(dist + 1);
            set_bit(nxt_bits, pid);
            g_pass_marks_win++;
        }
    } else { /* WIN */
        for (int i = 0; i < n; i++) {
            pos_id_t pid = board_to_id(&pre_buf[i]);
            if (result[pid] != ST_UNKNOWN) continue;
            uint8_t cc = child_count[pid];
            if (cc == 0) continue;
            cc--;
            child_count[pid] = cc;
            if (cc == 0) {
                result[pid] = ST_LOSS(dist + 1);
                set_bit(nxt_bits, pid);
                g_pass_marks_loss++;
            }
        }
    }
}

static long long bfs_pass(void)
{
    long long processed = 0;
    g_pass_marks_win = g_pass_marks_loss = 0;
    memset(nxt_bits, 0, BITMAP_BYTES);

    pos_id_t total_bytes = BITMAP_BYTES;
    for (pos_id_t byte = 0; byte < total_bytes; byte++) {
        uint8_t bs = cur_bits[byte];
        if (bs == 0) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (!((bs >> bit) & 1)) continue;
            pos_id_t id = byte * 8 + bit;
            if (id >= TOTAL_POSITIONS) break;
            process_position(id);
            processed++;
        }
    }

    uint8_t *t = cur_bits; cur_bits = nxt_bits; nxt_bits = t;
    return processed;
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *out_path = (argc > 1) ? argv[1] : "gobblet.db";

    position_init();

    fprintf(stderr, "TOTAL_POSITIONS = %llu\n",
            (unsigned long long)TOTAL_POSITIONS);
    fprintf(stderr, "approx memory   = %.2f GB\n",
            (2.0 * TOTAL_POSITIONS + 2.0 * BITMAP_BYTES) / (1024.0 * 1024.0 * 1024.0));

    result      = calloc(TOTAL_POSITIONS, 1);
    child_count = calloc(TOTAL_POSITIONS, 1);
    cur_bits    = calloc(BITMAP_BYTES, 1);
    nxt_bits    = calloc(BITMAP_BYTES, 1);
    if (!result || !child_count || !cur_bits || !nxt_bits) {
        fprintf(stderr, "FATAL: out of memory.\n");
        return 1;
    }

    forward_sweep();

    fprintf(stderr, "[bfs] starting...\n");
    time_t t0 = time(NULL);
    int level = 1;
    for (;;) {
        long long n = bfs_pass();
        time_t t1 = time(NULL);
        fprintf(stderr, "  pass %2d: processed %12lld  newWIN %10lld  newLOSS %10lld  (%lds)\n",
                level, n, g_pass_marks_win, g_pass_marks_loss, (long)(t1 - t0));
        if (g_pass_marks_win == 0 && g_pass_marks_loss == 0) break;
        level++;
        if (level > 200) {
            fprintf(stderr, "  too many passes — aborting.\n");
            break;
        }
    }

    fprintf(stderr, "[finalize] tagging UNKNOWN -> DRAW...\n");
    long long draws = 0;
    for (pos_id_t id = 0; id < TOTAL_POSITIONS; id++) {
        if (result[id] == ST_UNKNOWN) {
            result[id] = ST_DRAW;
            draws++;
        }
        g_dist_count[result[id]]++;
    }
    fprintf(stderr, "  DRAW = %lld\n", draws);

    /* Compact distance histogram printout */
    {
        long long wins = 0, losses = 0;
        for (int d = 0; d < 100; d++) {
            wins   += g_dist_count[ST_WIN_BASE + d];
            losses += g_dist_count[ST_LOSS_BASE + d];
        }
        fprintf(stderr, "  total WIN  = %lld\n", wins);
        fprintf(stderr, "  total LOSS = %lld\n", losses);
        fprintf(stderr, "  IMPOSSIBLE = %lld\n", g_dist_count[ST_IMPOSSIBLE]);
        fprintf(stderr, "  DRAW       = %lld\n", g_dist_count[ST_DRAW]);
    }

    fprintf(stderr, "[write] %s\n", out_path);
    FILE *fp = fopen(out_path, "wb");
    if (!fp) { perror(out_path); return 1; }
    pos_id_t written = 0;
    while (written < TOTAL_POSITIONS) {
        size_t chunk = (size_t)((TOTAL_POSITIONS - written) > (1ULL << 28)
                                ? (1ULL << 28) : (TOTAL_POSITIONS - written));
        size_t w = fwrite(result + written, 1, chunk, fp);
        if (w != chunk) { perror("fwrite"); return 1; }
        written += w;
    }
    fclose(fp);

    fprintf(stderr, "Done.\n");
    free(result); free(child_count); free(cur_bits); free(nxt_bits);
    return 0;
}
