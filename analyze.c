/*
 * Retrograde analysis main (parallel).
 *
 * Threads
 *   - forward sweep : N threads each own a disjoint id range.  cur_bits
 *                     writes use atomic OR (since bits 0..7 of one byte
 *                     can fall to different threads); per-position writes
 *                     to result[]/child_count[] never collide.
 *   - bfs pass     : N threads each scan a disjoint byte range of
 *                     cur_bits (read-only).  Predecessor updates use
 *                     atomic compare-exchange on result[] and atomic
 *                     fetch-sub on child_count[] so multiple threads can
 *                     safely fight over the same PRE id.
 *
 * Memory: same as serial — about 6.5 GB total.
 *
 * Invariants relied on by the parallel BFS
 *   - child_count[id] == 0 ↔ result[id] != ST_UNKNOWN
 *     (so the early "result != UNKNOWN" check skips dead decrements).
 *   - Within a single pass every WIN child carries the same `dist`, so
 *     the LOSS distance written when child_count reaches 0 is correct
 *     regardless of the thread that wins the CAS race.
 */

#include "gobblet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define BITMAP_BYTES ((TOTAL_POSITIONS + 7) / 8)

static uint8_t *result;
static uint8_t *child_count;
static uint8_t *cur_bits;
static uint8_t *nxt_bits;
static int      g_nthreads = 1;

/* --- atomic helpers -------------------------------------------------- */

static inline void atomic_set_bit(uint8_t *bits, pos_id_t id)
{
    __atomic_fetch_or(&bits[id >> 3],
                      (uint8_t)(1u << (id & 7)),
                      __ATOMIC_RELAXED);
}

/* --- per-thread stats ------------------------------------------------ */

typedef struct {
    long long term_loss;
    long long term_win;
    long long imp;
    long long win1;
    long long undet;
    long long stalemate;
    long long marks_win;
    long long marks_loss;
    long long processed;
} stats_t;

typedef struct {
    int     tid;
    pos_id_t fstart, fend;   /* forward sweep range  */
    size_t   bstart, bend;   /* bfs pass byte range  */
    stats_t  st;
} worker_t;

/* --- forward sweep --------------------------------------------------- */

static void *forward_worker(void *arg)
{
    worker_t *w = (worker_t *)arg;
    move_t mv[MAX_LEGAL_MOVES];

    for (pos_id_t id = w->fstart; id < w->fend; id++) {
        board_t b;
        id_to_board(id, &b);

        uint16_t top_cp, top_op;
        board_tops(&b, &top_cp, &top_op, NULL, NULL);
        int cp_row = line_complete(top_cp);
        int op_row = line_complete(top_op);

        if (cp_row && op_row) {
            result[id] = ST_IMPOSSIBLE;
            w->st.imp++;
            continue;
        }
        if (cp_row) {
            result[id] = ST_WIN(0);
            atomic_set_bit(cur_bits, id);
            w->st.term_win++;
            continue;
        }
        if (op_row) {
            result[id] = ST_LOSS(0);
            atomic_set_bit(cur_bits, id);
            w->st.term_loss++;
            continue;
        }

        int wc;
        int nm = generate_moves(&b, mv, &wc);
        if (wc > 0) {
            result[id] = ST_WIN(1);
            atomic_set_bit(cur_bits, id);
            w->st.win1++;
        } else if (nm == 0) {
            result[id] = ST_LOSS(0);
            atomic_set_bit(cur_bits, id);
            w->st.stalemate++;
        } else {
            child_count[id] = (nm > 255) ? 255 : (uint8_t)nm;
            w->st.undet++;
        }
    }
    return NULL;
}

static void run_forward(void)
{
    fprintf(stderr, "[forward] starting (%d threads)...\n", g_nthreads);
    time_t t0 = time(NULL);

    pthread_t *th = malloc((size_t)g_nthreads * sizeof *th);
    worker_t  *w  = calloc((size_t)g_nthreads, sizeof *w);

    pos_id_t chunk = TOTAL_POSITIONS / (pos_id_t)g_nthreads;
    for (int i = 0; i < g_nthreads; i++) {
        w[i].tid    = i;
        w[i].fstart = chunk * (pos_id_t)i;
        w[i].fend   = (i == g_nthreads - 1)
                          ? TOTAL_POSITIONS
                          : chunk * (pos_id_t)(i + 1);
        pthread_create(&th[i], NULL, forward_worker, &w[i]);
    }

    stats_t T = {0};
    for (int i = 0; i < g_nthreads; i++) {
        pthread_join(th[i], NULL);
        T.term_loss += w[i].st.term_loss;
        T.term_win  += w[i].st.term_win;
        T.imp       += w[i].st.imp;
        T.win1      += w[i].st.win1;
        T.undet     += w[i].st.undet;
        T.stalemate += w[i].st.stalemate;
    }
    free(th); free(w);

    time_t t1 = time(NULL);
    fprintf(stderr, "[forward] done in %ld s\n", (long)(t1 - t0));
    fprintf(stderr, "  WIN-0       : %lld\n", T.term_win);
    fprintf(stderr, "  LOSS-0(row) : %lld\n", T.term_loss);
    fprintf(stderr, "  LOSS-0(stm) : %lld\n", T.stalemate);
    fprintf(stderr, "  IMPOSSIBLE  : %lld\n", T.imp);
    fprintf(stderr, "  WIN-1       : %lld\n", T.win1);
    fprintf(stderr, "  undetermined: %lld\n", T.undet);
}

/* --- BFS pass -------------------------------------------------------- */

static void process_one(pos_id_t id, stats_t *st)
{
    uint8_t s = result[id];
    if (s == ST_UNKNOWN || s == ST_IMPOSSIBLE) return;

    int dist;
    if (IS_WIN(s))      dist = WIN_DIST(s);
    else if (IS_LOSS(s)) dist = LOSS_DIST(s);
    else return;
    if (dist >= 99) return;

    board_t b;
    id_to_board(id, &b);

    static __thread board_t pre_buf[256];
    int n = generate_predecessors(&b, pre_buf, 256);
    if (n == 0) return;

    if (IS_LOSS(s)) {
        const uint8_t newv = ST_WIN(dist + 1);
        for (int i = 0; i < n; i++) {
            pos_id_t pid = board_to_id(&pre_buf[i]);
            uint8_t expected = ST_UNKNOWN;
            if (__atomic_compare_exchange_n(&result[pid], &expected, newv,
                                            0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                atomic_set_bit(nxt_bits, pid);
                st->marks_win++;
            }
        }
    } else { /* WIN */
        const uint8_t lossv = ST_LOSS(dist + 1);
        for (int i = 0; i < n; i++) {
            pos_id_t pid = board_to_id(&pre_buf[i]);
            /* Quick non-atomic short-circuit: if PRE already determined,
             * skip without touching child_count.  child_count for any
             * determined PRE is 0, so a stray decrement here would
             * underflow. */
            if (__atomic_load_n(&result[pid], __ATOMIC_RELAXED) != ST_UNKNOWN)
                continue;
            uint8_t prev = __atomic_fetch_sub(&child_count[pid], 1, __ATOMIC_RELAXED);
            if (prev == 0) {
                /* defensive: race lost — restore */
                __atomic_fetch_add(&child_count[pid], 1, __ATOMIC_RELAXED);
                continue;
            }
            if (prev == 1) {
                uint8_t expected = ST_UNKNOWN;
                if (__atomic_compare_exchange_n(&result[pid], &expected, lossv,
                                                0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                    atomic_set_bit(nxt_bits, pid);
                    st->marks_loss++;
                }
            }
        }
    }
    st->processed++;
}

static void *bfs_worker(void *arg)
{
    worker_t *w = (worker_t *)arg;
    for (size_t byte = w->bstart; byte < w->bend; byte++) {
        uint8_t bs = cur_bits[byte];
        if (bs == 0) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (!((bs >> bit) & 1)) continue;
            pos_id_t id = (pos_id_t)byte * 8 + (pos_id_t)bit;
            if (id >= TOTAL_POSITIONS) break;
            process_one(id, &w->st);
        }
    }
    return NULL;
}

static long long run_bfs_pass(int level)
{
    pthread_t *th = malloc((size_t)g_nthreads * sizeof *th);
    worker_t  *w  = calloc((size_t)g_nthreads, sizeof *w);

    size_t chunk = BITMAP_BYTES / (size_t)g_nthreads;
    for (int i = 0; i < g_nthreads; i++) {
        w[i].tid    = i;
        w[i].bstart = chunk * (size_t)i;
        w[i].bend   = (i == g_nthreads - 1)
                          ? BITMAP_BYTES
                          : chunk * (size_t)(i + 1);
        pthread_create(&th[i], NULL, bfs_worker, &w[i]);
    }

    long long pr = 0, mw = 0, ml = 0;
    for (int i = 0; i < g_nthreads; i++) {
        pthread_join(th[i], NULL);
        pr += w[i].st.processed;
        mw += w[i].st.marks_win;
        ml += w[i].st.marks_loss;
    }
    free(th); free(w);

    /* swap cur and nxt */
    uint8_t *t = cur_bits; cur_bits = nxt_bits; nxt_bits = t;

    fprintf(stderr,
        "  pass %2d: processed %12lld  newWIN %10lld  newLOSS %10lld\n",
        level, pr, mw, ml);
    return mw + ml;
}

/* --- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *out_path = (argc > 1) ? argv[1] : "gobblet.db";

    g_nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_nthreads < 1) g_nthreads = 1;
    if (argc > 2) g_nthreads = atoi(argv[2]);
    if (g_nthreads < 1) g_nthreads = 1;

    position_init();

    fprintf(stderr, "TOTAL_POSITIONS = %llu\n",
            (unsigned long long)TOTAL_POSITIONS);
    fprintf(stderr, "approx memory   = %.2f GB\n",
            (2.0 * TOTAL_POSITIONS + 2.0 * BITMAP_BYTES) / (1024.0 * 1024.0 * 1024.0));
    fprintf(stderr, "threads         = %d\n", g_nthreads);

    result      = calloc(TOTAL_POSITIONS, 1);
    child_count = calloc(TOTAL_POSITIONS, 1);
    cur_bits    = calloc(BITMAP_BYTES, 1);
    nxt_bits    = calloc(BITMAP_BYTES, 1);
    if (!result || !child_count || !cur_bits || !nxt_bits) {
        fprintf(stderr, "FATAL: out of memory.\n");
        return 1;
    }

    run_forward();

    fprintf(stderr, "[bfs] starting...\n");
    time_t t0 = time(NULL);
    int level = 1;
    for (;;) {
        memset(nxt_bits, 0, BITMAP_BYTES);
        long long n = run_bfs_pass(level);
        time_t t1 = time(NULL);
        fprintf(stderr, "    elapsed %lds\n", (long)(t1 - t0));
        if (n == 0) break;
        level++;
        if (level > 200) {
            fprintf(stderr, "  too many passes — aborting.\n");
            break;
        }
    }

    fprintf(stderr, "[finalize] tagging UNKNOWN -> DRAW...\n");
    long long draws = 0;
    long long dist_count[256] = {0};
    for (pos_id_t id = 0; id < TOTAL_POSITIONS; id++) {
        if (result[id] == ST_UNKNOWN) {
            result[id] = ST_DRAW;
            draws++;
        }
        dist_count[result[id]]++;
    }
    long long wins = 0, losses = 0;
    for (int d = 0; d < 100; d++) {
        wins   += dist_count[ST_WIN_BASE + d];
        losses += dist_count[ST_LOSS_BASE + d];
    }
    fprintf(stderr, "  total WIN  = %lld\n", wins);
    fprintf(stderr, "  total LOSS = %lld\n", losses);
    fprintf(stderr, "  DRAW       = %lld\n", draws);
    fprintf(stderr, "  IMPOSSIBLE = %lld\n", dist_count[ST_IMPOSSIBLE]);

    fprintf(stderr, "[write] %s\n", out_path);
    FILE *fp = fopen(out_path, "wb");
    if (!fp) { perror(out_path); return 1; }
    pos_id_t written = 0;
    while (written < TOTAL_POSITIONS) {
        size_t cap = (size_t)((TOTAL_POSITIONS - written) > (1ULL << 28)
                              ? (1ULL << 28) : (TOTAL_POSITIONS - written));
        size_t wbytes = fwrite(result + written, 1, cap, fp);
        if (wbytes != cap) { perror("fwrite"); return 1; }
        written += wbytes;
    }
    fclose(fp);

    fprintf(stderr, "Done.\n");
    free(result); free(child_count); free(cur_bits); free(nxt_bits);
    return 0;
}
