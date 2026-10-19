/*
 * Checkers engine. See checkers_engine.h.
 *
 * Search: iterative-deepening negamax with alpha-beta. When the depth runs
 * out but the side to move has a jump available, the search keeps going
 * (jumps are forced anyway), so it never stops in the middle of an exchange.
 */
#include <stdlib.h>
#include <string.h>

#include "checkers_engine.h"
#include "game_sys.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define CAPTURED  64      /* marks a jumped piece during a multi-jump */

static const int8_t DR[4] = { 1, 1, -1, -1 };
static const int8_t DC[4] = { -1, 1, -1, 1 };

void ck_new_game(ck_pos_t *p)
{
    memset(p, 0, sizeof(*p));
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if ((r + c) & 1) continue;
            if (r <= 2) p->sq[r * 8 + c] = 1;
            if (r >= 5) p->sq[r * 8 + c] = -1;
        }
    }
    p->side = CK_RED;
}

/* Depth-first search of all jump sequences for the piece `pc` now on `sq`.
 * Records each sequence that can't be continued. Returns the new count. */
static int jumps(int8_t *b, int sq, int8_t pc, ck_move_t *cur, ck_move_t *out,
                 int n, int max)
{
    int r = sq >> 3, c = sq & 7;
    for (int d = 0; d < 4; d++) {
        if (abs(pc) == 1 && DR[d] != (pc > 0 ? 1 : -1)) continue;   /* men: forward only */
        int lr = r + 2 * DR[d], lc = c + 2 * DC[d];
        if (lr < 0 || lr > 7 || lc < 0 || lc > 7) continue;
        int m = (r + DR[d]) * 8 + (c + DC[d]);
        int l = lr * 8 + lc;
        int8_t v = b[m];
        if (v == 0 || v == CAPTURED || (v > 0) == (pc > 0) || b[l] != 0) continue;
        if (cur->len >= CK_MAX_PATH) continue;

        b[m] = CAPTURED;
        b[sq] = 0;
        b[l] = pc;
        cur->path[cur->len++] = (uint8_t)l;
        cur->ncap++;

        bool crowned = abs(pc) == 1 && lr == (pc > 0 ? 7 : 0);
        int before = n;
        if (!crowned) {
            n = jumps(b, l, pc, cur, out, n, max);
        }
        if (n == before && n < max) {
            out[n++] = *cur;
        }

        cur->len--;
        cur->ncap--;
        b[l] = 0;
        b[sq] = pc;
        b[m] = v;
    }
    return n;
}

static int gen(const ck_pos_t *p, ck_move_t *out, int max)
{
    int8_t b[64];
    memcpy(b, p->sq, sizeof(b));
    int n = 0;
    ck_move_t cur;

    /* Jumps first; if there are any, they're the only legal moves. */
    for (int s = 0; s < 64; s++) {
        int8_t pc = b[s];
        if (pc == 0 || (pc > 0) != (p->side > 0)) continue;
        cur.len = 1;
        cur.ncap = 0;
        cur.path[0] = (uint8_t)s;
        n = jumps(b, s, pc, &cur, out, n, max);
    }
    if (n > 0) return n;

    for (int s = 0; s < 64; s++) {
        int8_t pc = b[s];
        if (pc == 0 || (pc > 0) != (p->side > 0)) continue;
        int r = s >> 3, c = s & 7;
        for (int d = 0; d < 4; d++) {
            if (abs(pc) == 1 && DR[d] != (pc > 0 ? 1 : -1)) continue;
            int tr = r + DR[d], tc = c + DC[d];
            if (tr < 0 || tr > 7 || tc < 0 || tc > 7) continue;
            int t = tr * 8 + tc;
            if (b[t] != 0 || n >= max) continue;
            out[n].len = 2;
            out[n].ncap = 0;
            out[n].path[0] = (uint8_t)s;
            out[n].path[1] = (uint8_t)t;
            n++;
        }
    }
    return n;
}

int ck_legal_moves(const ck_pos_t *p, ck_move_t *out)
{
    return gen(p, out, CK_MAX_MOVES);
}

void ck_make(ck_pos_t *p, const ck_move_t *m)
{
    int from = m->path[0], to = m->path[m->len - 1];
    int8_t pc = p->sq[from];
    bool man_moved = abs(pc) == 1;
    p->sq[from] = 0;
    if (m->ncap) {
        for (int i = 0; i + 1 < m->len; i++) {
            p->sq[(m->path[i] + m->path[i + 1]) / 2] = 0;   /* jumped square */
        }
    }
    if (abs(pc) == 1 && (to >> 3) == (pc > 0 ? 7 : 0)) {
        pc *= 2;                                            /* crowned */
    }
    p->sq[to] = pc;
    if (m->ncap || man_moved) {
        p->quiet = 0;
    } else if (p->quiet < 255) {
        p->quiet++;
    }
    p->side = (int8_t)-p->side;
}

ck_status_t ck_status(const ck_pos_t *p)
{
    static ck_move_t list[CK_MAX_MOVES];   /* UI task only */
    if (gen(p, list, CK_MAX_MOVES) == 0) return CK_LOSS;
    if (p->quiet >= 80) return CK_DRAW;
    return CK_PLAYING;
}

uint32_t ck_hash(const ck_pos_t *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++) {
        h = (h ^ (uint8_t)p->sq[i]) * 16777619u;
    }
    return (h ^ (uint8_t)p->side) * 16777619u;
}

/* ---------- evaluation ---------- */

/* From the side to move's point of view. */
static int evaluate(const ck_pos_t *p)
{
    int score = 0, red = 0, white = 0;
    for (int s = 0; s < 64; s++) {
        int pc = p->sq[s];
        if (!pc) continue;
        int r = s >> 3, c = s & 7;
        int v;
        if (abs(pc) == 1) {
            int adv = pc > 0 ? r : 7 - r;        /* rows advanced */
            v = 100 + adv * 3;
            if (adv == 0) v += 8;                /* back row guards against kings */
        } else {
            v = 160;
        }
        if (r >= 2 && r <= 5 && c >= 2 && c <= 5) v += 4;   /* centre */
        if (pc > 0) { score += v; red += abs(pc) == 1 ? 100 : 160; }
        else        { score -= v; white += abs(pc) == 1 ? 100 : 160; }
    }
    /* When ahead, trading pieces makes the win easier. */
    int total = red + white;
    if (total > 0) {
        score += (red - white) * 300 / total;
    }
    return score * p->side;
}

/* ---------- search ---------- */

#define MAX_PLY 48
#define INF     32000
#define WIN     30000

typedef struct {
    int depth, noise, time_ms;
} level_t;

static const level_t LEVELS[4] = {
    { 2,  80, 1000 },     /* Easy */
    { 4,  25, 1500 },     /* Medium */
    { 8,   0, 2000 },     /* Hard */
    { 20,  0, 5000 },     /* Expert */
};

static struct {
    ck_move_t     *arena;            /* MAX_PLY move lists */
    int64_t        deadline;
    volatile bool *abort;
    bool           stop;
    uint32_t       nodes;
} S;

static void check_stop(void)
{
    game_worker_yield();
    if ((S.abort && *S.abort) || game_time_us() > S.deadline) {
        S.stop = true;
    }
}

static int negamax(const ck_pos_t *p, int depth, int alpha, int beta, int ply)
{
    if ((++S.nodes & 511) == 0) check_stop();
    if (S.stop) return 0;
    if (p->quiet >= 80) return 0;
    if (ply >= MAX_PLY - 1) return evaluate(p);

    ck_move_t *moves = S.arena + ply * CK_MAX_MOVES;
    int n = gen(p, moves, CK_MAX_MOVES);
    if (n == 0) return -WIN + ply;                   /* no moves = lost */
    if (depth <= 0 && moves[0].ncap == 0) return evaluate(p);   /* quiet: stop here */

    /* Bigger jumps first. */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && moves[j].ncap > moves[j - 1].ncap; j--) {
            ck_move_t t = moves[j]; moves[j] = moves[j - 1]; moves[j - 1] = t;
        }
    }

    int best = -INF;
    for (int i = 0; i < n; i++) {
        ck_pos_t c = *p;
        ck_make(&c, &moves[i]);
        int sc = -negamax(&c, depth - 1, -beta, -alpha, ply + 1);
        if (S.stop) return 0;
        if (sc > best) best = sc;
        if (sc > alpha) {
            alpha = sc;
            if (alpha >= beta) break;
        }
    }
    return best;
}

bool ck_search(const ck_pos_t *p, int level, volatile bool *abort, ck_move_t *out)
{
    static ck_move_t root[CK_MAX_MOVES];
    static int score[CK_MAX_MOVES];

    if (level < 0) level = 0;
    if (level > 3) level = 3;
    const level_t *L = &LEVELS[level];

    int n = gen(p, root, CK_MAX_MOVES);
    if (n == 0) return false;
    *out = root[0];
    if (n == 1) return true;

    if (!S.arena) {
        size_t sz = sizeof(ck_move_t) * MAX_PLY * CK_MAX_MOVES;
#ifdef ESP_PLATFORM
        S.arena = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#endif
        if (!S.arena) S.arena = malloc(sz);
        if (!S.arena) {
            *out = root[game_random() % n];
            return true;
        }
    }

    S.abort = abort;
    S.stop = false;
    S.nodes = 0;
    int64_t start = game_time_us();
    S.deadline = start + (int64_t)L->time_ms * 1000;

    for (int i = 0; i < n; i++) score[i] = 0;
    int done_depth = 0;

    for (int depth = 1; depth <= L->depth; depth++) {
        int alpha = -INF, best_i = 0;
        for (int i = 0; i < n; i++) {
            ck_pos_t c = *p;
            ck_make(&c, &root[i]);
            int sc = L->noise ? -negamax(&c, depth - 1, -INF, INF, 1)
                              : -negamax(&c, depth - 1, -INF, -alpha, 1);
            if (S.stop) break;
            score[i] = sc;
            if (sc > alpha) {
                alpha = sc;
                best_i = i;
            }
        }
        if (S.stop) break;

        ck_move_t bm = root[best_i];
        int bs = score[best_i];
        for (int j = best_i; j > 0; j--) {
            root[j] = root[j - 1];
            score[j] = score[j - 1];
        }
        root[0] = bm;
        score[0] = bs;
        *out = bm;
        done_depth = depth;

        if (alpha > WIN - 100 || alpha < -WIN + 100) break;
        if (game_time_us() - start > (int64_t)L->time_ms * 500) break;
    }

    if (L->noise && done_depth > 0) {
        int bi = 0, bv = -2 * INF;
        for (int i = 0; i < n; i++) {
            int v = score[i] + (int)(game_random() % (2 * L->noise + 1)) - L->noise;
            if (v > bv) {
                bv = v;
                bi = i;
            }
        }
        *out = root[bi];
    }
    return true;
}
