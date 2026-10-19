/*
 * PC-side tests for main/chess_engine.c (not part of the firmware).
 *
 *   gcc -O2 -Wall -I main -o /tmp/chess_test test/host/chess_test.c main/chess_engine.c
 *   /tmp/chess_test
 *
 * Perft counts every legal move sequence to a fixed depth and compares the
 * totals with well-known reference numbers (chessprogramming.org "Perft
 * Results"); any move-generation bug shows up as a mismatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chess_engine.h"
#include "game_sys.h"

/* ---- stubs for the device services ---- */
int64_t game_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
uint32_t game_random(void) { return (uint32_t)rand(); }
void game_worker_yield(void) {}

static void from_fen(chess_pos_t *p, const char *fen)
{
    memset(p, 0, sizeof(*p));
    int rank = 7, file = 0;
    const char *c = fen;
    for (; *c && *c != ' '; c++) {
        if (*c == '/') { rank--; file = 0; continue; }
        if (*c >= '1' && *c <= '8') { file += *c - '0'; continue; }
        int t = 0;
        switch (*c | 32) {
        case 'p': t = CH_PAWN; break;   case 'n': t = CH_KNIGHT; break;
        case 'b': t = CH_BISHOP; break; case 'r': t = CH_ROOK; break;
        case 'q': t = CH_QUEEN; break;  case 'k': t = CH_KING; break;
        }
        int sq = rank * 8 + file++;
        int white = (*c >= 'A' && *c <= 'Z');
        p->sq[sq] = (int8_t)(white ? t : -t);
        if (t == CH_KING) p->king[white ? 0 : 1] = (uint8_t)sq;
    }
    c++;
    p->side = (*c == 'w') ? CH_WHITE : CH_BLACK;
    c += 2;
    for (; *c && *c != ' '; c++) {
        if (*c == 'K') p->castle |= 1;
        if (*c == 'Q') p->castle |= 2;
        if (*c == 'k') p->castle |= 4;
        if (*c == 'q') p->castle |= 8;
    }
    c++;
    p->ep = -1;
    if (*c != '-') p->ep = (int8_t)((c[1] - '1') * 8 + (c[0] - 'a'));
    p->fullmove = 1;
}

static chess_move_t lists[16][CH_MAX_MOVES];

static uint64_t perft(const chess_pos_t *p, int depth)
{
    int n = chess_legal_moves(p, lists[depth]);
    if (depth == 1) return (uint64_t)n;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        chess_pos_t c = *p;
        chess_make(&c, lists[depth][i]);
        total += perft(&c, depth - 1);
    }
    return total;
}

static int failures;

static void check_perft(const char *name, const char *fen, int depth, uint64_t want)
{
    chess_pos_t p;
    from_fen(&p, fen);
    int64_t t0 = game_time_us();
    uint64_t got = perft(&p, depth);
    double ms = (game_time_us() - t0) / 1000.0;
    printf("%-10s depth %d: %10llu  %s  (%.0f ms)\n", name, depth,
           (unsigned long long)got, got == want ? "OK" : "MISMATCH", ms);
    if (got != want) {
        printf("           expected %llu\n", (unsigned long long)want);
        failures++;
    }
}

static void move_str(chess_move_t m, char *b)
{
    sprintf(b, "%c%d%c%d", 'a' + (CM_FROM(m) & 7), 1 + (CM_FROM(m) >> 3),
            'a' + (CM_TO(m) & 7), 1 + (CM_TO(m) >> 3));
}

static void check_search(const char *name, const char *fen, int level, const char *want)
{
    chess_pos_t p;
    from_fen(&p, fen);
    volatile bool abort = false;
    int64_t t0 = game_time_us();
    chess_move_t m = chess_search(&p, level, &abort);
    double ms = (game_time_us() - t0) / 1000.0;
    char b[8], san[16];
    move_str(m, b);
    chess_move_to_san(&p, m, san, sizeof(san));
    int ok = !want || strcmp(b, want) == 0;
    printf("%-22s level %d: %-6s %-8s %s (%.0f ms)\n", name, level, b, san,
           want ? (ok ? "OK" : "WRONG") : "", ms);
    if (!ok) failures++;
}

int main(void)
{
    srand(1);
    const char *start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    check_perft("start", start, 4, 197281);
    check_perft("start", start, 5, 4865609);
    check_perft("kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603);
    check_perft("pos3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624);
    check_perft("pos4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333);
    check_perft("pos5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379);

    /* Search sanity: find mates and win material. */
    const char *mate1 = "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1";             /* Rd8# */
    const char *mate2 = "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 0 1"; /* Qxf7# */
    const char *hang  = "rnb1kbnr/pppp1ppp/8/4p1q1/3P4/2N5/PPP1PPPP/R1BQKBNR w KQkq - 0 1"; /* Bxg5 */
    for (int lvl = 1; lvl < 4; lvl++) {
        check_search("mate in 1 (Rd8#)", mate1, lvl, "d1d8");
        check_search("scholar's mate (Qxf7#)", mate2, lvl, "h5f7");
        check_search("free queen (Bxg5)", hang, lvl, "c1g5");
    }
    for (int lvl = 0; lvl < 4; lvl++) {
        check_search("opening move", start, lvl, NULL);
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nAll tests passed\n", failures);
    return failures ? 1 : 0;
}
