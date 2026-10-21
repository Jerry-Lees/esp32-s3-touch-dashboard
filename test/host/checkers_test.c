/*
 * PC-side tests for apps/checkers/main/checkers_engine.c (the Checkers SD card app).
 *
 *   gcc -O2 -Wall -I sdk/dashboard_sdk/include -I apps/checkers/main -o /tmp/checkers_test test/host/checkers_test.c apps/checkers/main/checkers_engine.c
 *   /tmp/checkers_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "checkers_engine.h"
#include "game_sys.h"

int64_t game_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
uint32_t game_random(void) { return (uint32_t)rand(); }
void game_worker_yield(void) {}

static int failures;
static ck_move_t lists[16][CK_MAX_MOVES];

static uint64_t perft(const ck_pos_t *p, int depth)
{
    int n = ck_legal_moves(p, lists[depth]);
    if (depth == 1) return (uint64_t)n;
    uint64_t t = 0;
    for (int i = 0; i < n; i++) {
        ck_pos_t c = *p;
        ck_make(&c, &lists[depth][i]);
        t += perft(&c, depth - 1);
    }
    return t;
}

static void expect(const char *what, long got, long want)
{
    printf("%-40s %8ld  %s\n", what, got, got == want ? "OK" : "MISMATCH");
    if (got != want) {
        printf("    expected %ld\n", want);
        failures++;
    }
}

/* Plays a game between two levels; returns +1 if `a` (Red) wins, -1 if
 * `b` wins, 0 for a draw. */
static int play(int a, int b)
{
    ck_pos_t p;
    ck_new_game(&p);
    volatile bool abort = false;
    for (int ply = 0; ply < 300; ply++) {
        ck_status_t st = ck_status(&p);
        if (st == CK_LOSS) return p.side == CK_RED ? -1 : 1;
        if (st == CK_DRAW) return 0;
        ck_move_t m;
        ck_search(&p, p.side == CK_RED ? a : b, &abort, &m);
        ck_make(&p, &m);
    }
    return 0;
}

int main(void)
{
    srand(2);
    ck_pos_t p;
    ck_new_game(&p);

    /* Reference: American checkers perft from the opening position. */
    const long want[] = { 0, 7, 49, 302, 1469, 7361, 36768, 179740, 845931, 3963680 };
    for (int d = 1; d <= 9; d++) {
        char what[32];
        snprintf(what, sizeof(what), "perft depth %d", d);
        expect(what, (long)perft(&p, d), want[d]);
    }

    /* Forced double jump: red man on 0 (a1), white men on 9 and 25. */
    memset(&p, 0, sizeof(p));
    p.side = CK_RED;
    p.sq[0] = 1; p.sq[9] = -1; p.sq[27] = -1; p.sq[6] = 1;
    ck_move_t mv[CK_MAX_MOVES];
    int n = ck_legal_moves(&p, mv);
    expect("double jump: number of moves", n, 1);
    expect("double jump: squares in path", mv[0].len, 3);
    expect("double jump: pieces captured", mv[0].ncap, 2);
    ck_make(&p, &mv[0]);
    expect("double jump: lands on 36", p.sq[36], 1);
    expect("double jump: jumped pieces removed", p.sq[9] == 0 && p.sq[27] == 0, 1);

    /* Crowning ends the move: red man on 42 jumps 51 and lands on 60 (the
     * far row). As a king it could go on to jump 53, but crowning stops it. */
    memset(&p, 0, sizeof(p));
    p.side = CK_RED;
    p.sq[42] = 1; p.sq[51] = -1; p.sq[53] = -1;
    n = ck_legal_moves(&p, mv);
    expect("crowning jump: moves", n, 1);
    expect("crowning jump: single hop", mv[0].len, 2);
    ck_make(&p, &mv[0]);
    expect("crowning jump: became a king", p.sq[60], 2);
    expect("crowning jump: 53 not taken", p.sq[53], -1);

    /* Strength: stronger levels should beat weaker ones (Expert takes up to
     * 5 s per move, so this part runs for several minutes). */
    int score = 0;
    for (int g = 0; g < 4; g++) score += play(2, 0) - play(0, 2);
    printf("Hard vs Easy (8 games, +1 per Hard win, -1 per loss): %+d\n", score);
    if (score <= 0) failures++;

    score = 0;
    for (int g = 0; g < 2; g++) score += play(3, 1) - play(1, 3);
    printf("Expert vs Medium (4 games): %+d\n", score);
    if (score <= 0) failures++;

    ck_new_game(&p);
    for (int lvl = 0; lvl < 4; lvl++) {
        volatile bool abort = false;
        ck_move_t m;
        int64_t t0 = game_time_us();
        ck_search(&p, lvl, &abort, &m);
        printf("opening move level %d: %d-%d  (%.0f ms)\n", lvl, m.path[0], m.path[1],
               (game_time_us() - t0) / 1000.0);
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nAll tests passed\n", failures);
    return failures ? 1 : 0;
}
