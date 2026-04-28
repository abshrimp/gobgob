/*
 * Query tool: given a board state and the side to move, look up the
 * position in the analysed DB and report the evaluation of every legal
 * move (best-first ordering).
 *
 * Usage:
 * query "<board>" <O|B> [--hand "oSoMoL/bSbMbL"] [--json] [db-path]
 *
 * <board> = 9 squares, separated by whitespace, '/' or '|'.  Each
 * square is THREE characters in this fixed order:
 * char 0 = small-piece  slot  ('-' empty, 's'=orange, 'S'=blue)
 * char 1 = medium-piece slot  ('-' empty, 'm'=orange, 'M'=blue)
 * char 2 = large-piece  slot  ('-' empty, 'l'=orange, 'L'=blue)
 *
 * <O|B>  = O if it is orange's turn, B if blue's turn.
 * db-path defaults to ./gobblet.db.
 *
 * Examples:
 * query "--- --- --- --- s-- --- --- --- ---" O
 * query "s-l --- --- --- --- --- --- --- ---" B  --json ./gobblet.db
 */

#include "gobblet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const uint8_t *db = NULL;

static int load_db(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return -1; }
    if ((unsigned long long)st.st_size < (unsigned long long)TOTAL_POSITIONS) {
        fprintf(stderr, "DB file too small: %lld bytes (expected %llu)\n",
                (long long)st.st_size, (unsigned long long)TOTAL_POSITIONS);
        close(fd); return -1;
    }
    void *p = mmap(NULL, TOTAL_POSITIONS, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return -1; }
    db = (const uint8_t *)p;
    return 0;
}

/* --- evaluation helpers ----------------------------------------- */

/* A score that ranks a CHILD position from the *current* player's POV.
 * The current player wants children that are LOSS (opponent loses).
 * child = LOSS-d  → CP wins in d+1, score very high (smaller d → higher).
 * child = DRAW    → 0.
 * child = WIN-d   → CP loses in d+1, score very low (larger d → higher).
 */
static long score_child(uint8_t cs)
{
    if (IS_LOSS(cs)) return  100000L - LOSS_DIST(cs);
    if (cs == ST_DRAW) return 0;
    if (IS_WIN(cs))  return -100000L + WIN_DIST(cs);
    if (cs == ST_IMPOSSIBLE) return -1000000L;  /* shouldn't appear */
    return -2000000L;  /* unknown */
}

static const char *eval_str(uint8_t cs, char *buf, size_t cap)
{
    if (IS_LOSS(cs))    snprintf(buf, cap, "WIN  in %2d", LOSS_DIST(cs) + 1);
    else if (IS_WIN(cs))snprintf(buf, cap, "LOSS in %2d", WIN_DIST(cs)  + 1);
    else if (cs == ST_DRAW)      snprintf(buf, cap, "DRAW       ");
    else if (cs == ST_IMPOSSIBLE)snprintf(buf, cap, "IMPOSSIBLE ");
    else                          snprintf(buf, cap, "UNKNOWN    ");
    return buf;
}

/* --- main -------------------------------------------------------- */

typedef struct {
    move_t  m;
    uint8_t child_status;
    long    score;
} ranked_move_t;

static int cmp_ranked(const void *a, const void *b)
{
    long sa = ((const ranked_move_t *)a)->score;
    long sb = ((const ranked_move_t *)b)->score;
    return (sa < sb) - (sa > sb);
}

/* Parse "<oS><oM><oL>/<bS><bM><bL>" e.g. "212/220" — 6 single digits. */
static int parse_hand_spec(const char *s, int orange[3], int blue[3])
{
    int n = sscanf(s, "%1d%1d%1d/%1d%1d%1d",
                   &orange[0], &orange[1], &orange[2],
                   &blue[0],   &blue[1],   &blue[2]);
    if (n != 6) return -1;
    for (int i = 0; i < 3; i++) {
        if (orange[i] < 0 || orange[i] > 2) return -1;
        if (blue[i]   < 0 || blue[i]   > 2) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s \"<board>\" <O|B> [--hand \"oSoMoL/bSbMbL\"] [--json] [db-path]\n"
            "  <board>: 9 squares of 3 glyphs each. - = empty.\n"
            "           s/m/l = orange small/medium/large.\n"
            "           S/M/L = blue   small/medium/large.\n"
            "  <O|B>  : O = orange to move, B = blue to move.\n"
            "  --hand : OPTIONAL.  6 digits like \"212/220\" for sanity-check.\n"
            "           If omitted, hand counts are derived from the board\n"
            "           (each player owns exactly 2 of each size).\n"
            "  --json : OPTIONAL.  Output result in JSON format.\n",
            argv[0]);
        return 2;
    }
    const char *board_str = argv[1];
    int turn_orange       = (argv[2][0] == 'O' || argv[2][0] == 'o');
    const char *hand_spec = NULL;
    const char *db_path   = "gobblet.db";
    int json_output       = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--hand") == 0 && i + 1 < argc) {
            hand_spec = argv[++i];
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
        } else {
            db_path = argv[i];
        }
    }

    position_init();

    board_t b;
    if (parse_board(board_str, NULL, turn_orange, &b) < 0) {
        fprintf(stderr, "Failed to parse board.\n");
        return 1;
    }

    if (hand_spec) {
        int want_o[3], want_b[3];
        if (parse_hand_spec(hand_spec, want_o, want_b) < 0) {
            fprintf(stderr,
                "Bad --hand spec: \"%s\".  Expected 6 digits like \"212/220\".\n",
                hand_spec);
            return 1;
        }
        int got_o[3], got_b[3];
        for (int sz = 0; sz < N_SIZES; sz++) {
            int cp = hand_count(&b, CELL_CP, sz);
            int op = hand_count(&b, CELL_OP, sz);
            got_o[sz] = turn_orange ? cp : op;
            got_b[sz] = turn_orange ? op : cp;
        }
        int ok = 1;
        for (int sz = 0; sz < N_SIZES; sz++) {
            if (want_o[sz] != got_o[sz] || want_b[sz] != got_b[sz]) ok = 0;
        }
        if (!ok) {
            fprintf(stderr,
                "Hand mismatch.\n"
                "  declared : orange %d%d%d / blue %d%d%d\n"
                "  on-board : orange %d%d%d / blue %d%d%d\n",
                want_o[0], want_o[1], want_o[2], want_b[0], want_b[1], want_b[2],
                got_o[0],  got_o[1],  got_o[2],  got_b[0],  got_b[1],  got_b[2]);
            return 1;
        }
    }

    if (load_db(db_path) < 0) return 1;

    pos_id_t id = board_to_id(&b);
    uint8_t  s  = db[id];

    // Evaluate current state
    int is_terminal = (s == ST_IMPOSSIBLE) || 
                      (IS_WIN(s) && WIN_DIST(s) == 0) || 
                      (IS_LOSS(s) && LOSS_DIST(s) == 0);

    // Generate and rank moves if not terminal
    int n = 0;
    int wc = 0;
    ranked_move_t rms[MAX_LEGAL_MOVES];
    int r = 0;

    if (!is_terminal) {
        move_t mv[MAX_LEGAL_MOVES];
        n = generate_moves(&b, mv, &wc);

        for (int i = 0; i < n; i++) {
            board_t nb;
            uint8_t code = apply_move_next(&b, &mv[i], &nb);
            uint8_t cs;
            if (code != 0) {
                cs = code;
            } else {
                pos_id_t cid = board_to_id(&nb);
                cs = db[cid];
            }
            rms[r].m            = mv[i];
            rms[r].child_status = cs;
            rms[r].score        = score_child(cs);
            r++;
        }
        qsort(rms, r, sizeof(ranked_move_t), cmp_ranked);
    }

    // --- Output Phase ---
    if (json_output) {
        char eval_base_buf[64] = "UNKNOWN";
        if (IS_WIN(s) || IS_LOSS(s) || s == ST_DRAW || s == ST_IMPOSSIBLE) {
            if (IS_WIN(s))  snprintf(eval_base_buf, sizeof eval_base_buf, "WIN in %d", WIN_DIST(s));
            else if (IS_LOSS(s)) snprintf(eval_base_buf, sizeof eval_base_buf, "LOSS in %d", LOSS_DIST(s));
            else if (s == ST_DRAW) snprintf(eval_base_buf, sizeof eval_base_buf, "DRAW");
            else snprintf(eval_base_buf, sizeof eval_base_buf, "IMPOSSIBLE");
        }

        printf("{\n");
        printf("  \"turn\": \"%s\",\n", turn_orange ? "ORANGE" : "BLUE");
        printf("  \"position_id\": %llu,\n", (unsigned long long)id);
        printf("  \"evaluation\": \"%s\",\n", eval_base_buf);
        printf("  \"is_terminal\": %s,\n", is_terminal ? "true" : "false");
        printf("  \"moves\": [\n");
        
        for (int i = 0; i < r; i++) {
            char mbuf[64], ebuf[32];
            format_move(&rms[i].m, mbuf, sizeof mbuf);
            eval_str(rms[i].child_status, ebuf, sizeof ebuf);
            
            // Remove trailing newlines or extra spaces if format_move/eval_str produces them
            // In JSON, we want clean strings.
            printf("    {\"move\": \"%s\", \"eval\": \"%s\"}%s\n", 
                   mbuf, ebuf, (i == r - 1) ? "" : ",");
        }
        
        printf("  ]\n");
        printf("}\n");

    } else {
        // Original text output
        printf("=== Position ===\n");
        board_print(&b, stdout);
        printf("turn          : %s\n", turn_orange ? "ORANGE" : "BLUE");
        printf("position id   : %llu\n", (unsigned long long)id);

        if (IS_WIN(s) || IS_LOSS(s) || s == ST_DRAW || s == ST_IMPOSSIBLE) {
            char buf[32];
            if (IS_WIN(s))  snprintf(buf, sizeof buf, "WIN  in %d (CP wins)",  WIN_DIST(s));
            else if (IS_LOSS(s)) snprintf(buf, sizeof buf, "LOSS in %d (CP lost)", LOSS_DIST(s));
            else if (s == ST_DRAW) snprintf(buf, sizeof buf, "DRAW");
            else snprintf(buf, sizeof buf, "IMPOSSIBLE");
            printf("evaluation    : %s\n", buf);
        } else {
            printf("evaluation    : UNKNOWN (id=%u)\n", s);
        }

        if (s == ST_IMPOSSIBLE) return 0;
        if (IS_WIN(s)  && WIN_DIST(s)  == 0) { printf("(terminal: CP just won)\n");  return 0; }
        if (IS_LOSS(s) && LOSS_DIST(s) == 0) { printf("(terminal: CP just lost)\n"); return 0; }

        printf("\n=== Legal moves : %d (winning placements: %d) ===\n", n, wc);
        for (int i = 0; i < r; i++) {
            char mbuf[64];
            char ebuf[32];
            format_move(&rms[i].m, mbuf, sizeof mbuf);
            eval_str(rms[i].child_status, ebuf, sizeof ebuf);
            printf("  %-32s %s\n", mbuf, ebuf);
        }
    }

    return 0;
}