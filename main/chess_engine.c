/*
 * Chess engine. See chess_engine.h.
 *
 * Board: 64 squares plus a 10x12 "mailbox" table for move generation, so
 * stepping off the board is a single table lookup (-1 = off board).
 * Search: iterative-deepening negamax with alpha-beta, quiescence search
 * (captures only) at the leaves, MVV-LVA capture ordering, killer moves
 * and a check extension. Evaluation: material + piece-square tables
 * (Tomasz Michniewski's "Simplified Evaluation Function").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chess_engine.h"
#include "game_sys.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* ---------- board geometry ---------- */

#define X10   -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
#define R8(n) -1, n, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7, -1
static const int8_t MB120[120] = {
    X10, X10, R8(0), R8(8), R8(16), R8(24), R8(32), R8(40), R8(48), R8(56), X10, X10,
};
#define TO120(s) (21 + ((s) >> 3) * 10 + ((s) & 7))

static const int8_t N_OFF[8] = { -21, -19, -12, -8, 8, 12, 19, 21 };
static const int8_t B_OFF[4] = { -11, -9, 9, 11 };
static const int8_t R_OFF[4] = { -10, -1, 1, 10 };
static const int8_t K_OFF[8] = { -11, -10, -9, -1, 1, 9, 10, 11 };

#define KIDX(side) ((side) == CH_WHITE ? 0 : 1)

static const int VALUE[7] = { 0, 100, 320, 330, 500, 900, 0 };

/* Castling rights kept when a move touches a square (king or rook moved or
 * a rook was captured). */
static uint8_t castle_keep(int sq)
{
    switch (sq) {
    case 0:  return 15 & ~2;
    case 4:  return 15 & ~3;
    case 7:  return 15 & ~1;
    case 56: return 15 & ~8;
    case 60: return 15 & ~12;
    case 63: return 15 & ~4;
    default: return 15;
    }
}

/* ---------- basics ---------- */

void chess_new_game(chess_pos_t *p)
{
    static const int8_t back[8] = { CH_ROOK, CH_KNIGHT, CH_BISHOP, CH_QUEEN,
                                    CH_KING, CH_BISHOP, CH_KNIGHT, CH_ROOK };
    memset(p, 0, sizeof(*p));
    for (int f = 0; f < 8; f++) {
        p->sq[f] = back[f];
        p->sq[8 + f] = CH_PAWN;
        p->sq[48 + f] = -CH_PAWN;
        p->sq[56 + f] = (int8_t)-back[f];
    }
    p->side = CH_WHITE;
    p->castle = 15;
    p->ep = -1;
    p->fullmove = 1;
    p->king[0] = 4;
    p->king[1] = 60;
}

/* Is square `sq` attacked by side `by`? */
static bool attacked(const chess_pos_t *p, int sq, int by)
{
    int s120 = TO120(sq);
    int t;

    /* A white pawn attacks diagonally upward, so look one rank below. */
    int pd = (by == CH_WHITE) ? -10 : 10;
    t = MB120[s120 + pd - 1];
    if (t >= 0 && p->sq[t] == by * CH_PAWN) return true;
    t = MB120[s120 + pd + 1];
    if (t >= 0 && p->sq[t] == by * CH_PAWN) return true;

    for (int i = 0; i < 8; i++) {
        t = MB120[s120 + N_OFF[i]];
        if (t >= 0 && p->sq[t] == by * CH_KNIGHT) return true;
        t = MB120[s120 + K_OFF[i]];
        if (t >= 0 && p->sq[t] == by * CH_KING) return true;
    }
    for (int i = 0; i < 4; i++) {
        for (int s = s120 + B_OFF[i]; (t = MB120[s]) >= 0; s += B_OFF[i]) {
            int pc = p->sq[t];
            if (pc) {
                if (pc == by * CH_BISHOP || pc == by * CH_QUEEN) return true;
                break;
            }
        }
        for (int s = s120 + R_OFF[i]; (t = MB120[s]) >= 0; s += R_OFF[i]) {
            int pc = p->sq[t];
            if (pc) {
                if (pc == by * CH_ROOK || pc == by * CH_QUEEN) return true;
                break;
            }
        }
    }
    return false;
}

bool chess_in_check(const chess_pos_t *p)
{
    return attacked(p, p->king[KIDX(p->side)], -p->side);
}

static inline chess_move_t mk(int from, int to, int promo, int flags, int score)
{
    if (score > 255) score = 255;
    return (uint32_t)from | ((uint32_t)to << 6) | ((uint32_t)promo << 12) |
           ((uint32_t)flags << 16) | ((uint32_t)score << 24);
}

static int add_promos(chess_move_t *out, int n, int from, int to, int flags,
                      int base, bool queen_only)
{
    out[n++] = mk(from, to, CH_QUEEN, flags, base + 100);
    if (!queen_only) {
        out[n++] = mk(from, to, CH_KNIGHT, flags, base);
        out[n++] = mk(from, to, CH_ROOK, flags, base);
        out[n++] = mk(from, to, CH_BISHOP, flags, base);
    }
    return n;
}

/* Pseudo-legal moves (may leave the own king in check). Capture ordering
 * score = MVV-LVA: most valuable victim first, then least valuable attacker. */
static int gen(const chess_pos_t *p, chess_move_t *out, bool caps_only)
{
    int n = 0;
    const int side = p->side;

    for (int s = 0; s < 64; s++) {
        int pc = p->sq[s];
        if (pc == 0 || (pc > 0) != (side > 0)) continue;
        int type = pc * side;
        int s120 = TO120(s);

        if (type == CH_PAWN) {
            int fwd   = (side == CH_WHITE) ? 10 : -10;
            int rank  = s >> 3;
            int start = (side == CH_WHITE) ? 1 : 6;
            int last  = (side == CH_WHITE) ? 6 : 1;     /* rank before promotion */

            int t = MB120[s120 + fwd];
            if (t >= 0 && p->sq[t] == 0) {
                if (rank == last) {
                    n = add_promos(out, n, s, t, 0, 50, caps_only);
                } else if (!caps_only) {
                    out[n++] = mk(s, t, 0, 0, 0);
                    if (rank == start) {
                        int t2 = MB120[s120 + 2 * fwd];
                        if (p->sq[t2] == 0) out[n++] = mk(s, t2, 0, CMF_DOUBLE, 0);
                    }
                }
            }
            for (int d = -1; d <= 1; d += 2) {
                t = MB120[s120 + fwd + d];
                if (t < 0) continue;
                int v = p->sq[t];
                if (v != 0 && (v > 0) != (side > 0)) {
                    int score = 64 + abs(v) * 8 - CH_PAWN;
                    if (rank == last) {
                        n = add_promos(out, n, s, t, CMF_CAPTURE, score, caps_only);
                    } else {
                        out[n++] = mk(s, t, 0, CMF_CAPTURE, score);
                    }
                } else if (v == 0 && t == p->ep) {
                    out[n++] = mk(s, t, 0, CMF_CAPTURE | CMF_EP, 64 + 8 - CH_PAWN);
                }
            }
            continue;
        }

        const int8_t *off;
        int noff;
        bool slide;
        switch (type) {
        case CH_KNIGHT: off = N_OFF; noff = 8; slide = false; break;
        case CH_BISHOP: off = B_OFF; noff = 4; slide = true;  break;
        case CH_ROOK:   off = R_OFF; noff = 4; slide = true;  break;
        case CH_QUEEN:  off = K_OFF; noff = 8; slide = true;  break;
        default:        off = K_OFF; noff = 8; slide = false; break;   /* king */
        }
        for (int i = 0; i < noff; i++) {
            for (int t120 = s120 + off[i];; t120 += off[i]) {
                int t = MB120[t120];
                if (t < 0) break;
                int v = p->sq[t];
                if (v == 0) {
                    if (!caps_only) out[n++] = mk(s, t, 0, 0, 0);
                } else {
                    if ((v > 0) != (side > 0)) {
                        out[n++] = mk(s, t, 0, CMF_CAPTURE, 64 + abs(v) * 8 - type);
                    }
                    break;
                }
                if (!slide) break;
            }
        }

        if (type == CH_KING && !caps_only) {
            int e = (side == CH_WHITE) ? 4 : 60;
            int ks = (side == CH_WHITE) ? 1 : 4, qs = (side == CH_WHITE) ? 2 : 8;
            if (s == e && (p->castle & (ks | qs)) && !attacked(p, e, -side)) {
                if ((p->castle & ks) && !p->sq[e + 1] && !p->sq[e + 2] &&
                    p->sq[e + 3] == side * CH_ROOK &&
                    !attacked(p, e + 1, -side) && !attacked(p, e + 2, -side)) {
                    out[n++] = mk(e, e + 2, 0, CMF_CASTLE, 0);
                }
                if ((p->castle & qs) && !p->sq[e - 1] && !p->sq[e - 2] && !p->sq[e - 3] &&
                    p->sq[e - 4] == side * CH_ROOK &&
                    !attacked(p, e - 1, -side) && !attacked(p, e - 2, -side)) {
                    out[n++] = mk(e, e - 2, 0, CMF_CASTLE, 0);
                }
            }
        }
    }
    return n;
}

void chess_make(chess_pos_t *p, chess_move_t m)
{
    const int from = CM_FROM(m), to = CM_TO(m), fl = CM_FLAGS(m), promo = CM_PROMO(m);
    const int side = p->side;
    const int pc = p->sq[from];
    bool captured = p->sq[to] != 0;

    p->sq[to] = (int8_t)(promo ? side * promo : pc);
    p->sq[from] = 0;
    if (fl & CMF_EP) {
        p->sq[to - 8 * side] = 0;
        captured = true;
    }
    if (fl & CMF_CASTLE) {
        int rf = (to > from) ? to + 1 : to - 2;   /* rook from */
        int rt = (to > from) ? to - 1 : to + 1;   /* rook to */
        p->sq[rt] = p->sq[rf];
        p->sq[rf] = 0;
    }
    if (pc * side == CH_KING) {
        p->king[KIDX(side)] = (uint8_t)to;
    }
    p->castle &= castle_keep(from) & castle_keep(to);
    p->ep = (int8_t)((fl & CMF_DOUBLE) ? (from + to) / 2 : -1);
    if (pc * side == CH_PAWN || captured) {
        p->halfmove = 0;
    } else if (p->halfmove < 255) {
        p->halfmove++;
    }
    if (side == CH_BLACK) {
        p->fullmove++;
    }
    p->side = (int8_t)-side;
}

int chess_legal_moves(const chess_pos_t *p, chess_move_t *out)
{
    int n = gen(p, out, false), k = 0;
    for (int i = 0; i < n; i++) {
        chess_pos_t c = *p;
        chess_make(&c, out[i]);
        if (!attacked(&c, c.king[KIDX(p->side)], c.side)) {
            out[k++] = out[i] & 0xFFFFFF;
        }
    }
    return k;
}

static bool insufficient_material(const chess_pos_t *p)
{
    int minors = 0;
    for (int s = 0; s < 64; s++) {
        int t = abs(p->sq[s]);
        if (t == CH_PAWN || t == CH_ROOK || t == CH_QUEEN) return false;
        if (t == CH_KNIGHT || t == CH_BISHOP) minors++;
    }
    return minors <= 1;
}

chess_status_t chess_status(const chess_pos_t *p)
{
    static chess_move_t list[CH_MAX_MOVES];   /* UI task only */
    if (chess_legal_moves(p, list) == 0) {
        return chess_in_check(p) ? CH_CHECKMATE : CH_STALEMATE;
    }
    if (p->halfmove >= 100) return CH_DRAW_50;
    if (insufficient_material(p)) return CH_DRAW_MATERIAL;
    return CH_PLAYING;
}

uint32_t chess_hash(const chess_pos_t *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++) {
        h = (h ^ (uint8_t)p->sq[i]) * 16777619u;
    }
    h = (h ^ (uint8_t)p->side) * 16777619u;
    h = (h ^ p->castle) * 16777619u;
    h = (h ^ (uint8_t)p->ep) * 16777619u;
    return h;
}

void chess_move_to_san(const chess_pos_t *p, chess_move_t m, char *buf, size_t n)
{
    static chess_move_t list[CH_MAX_MOVES];   /* UI task only */
    static const char LETTER[] = " PNBRQK";
    const int from = CM_FROM(m), to = CM_TO(m), fl = CM_FLAGS(m);
    const int type = abs(p->sq[from]);
    char s[16];
    int k = 0;

    if (fl & CMF_CASTLE) {
        k = snprintf(s, sizeof(s), "%s", (to & 7) == 6 ? "O-O" : "O-O-O");
    } else {
        if (type != CH_PAWN) {
            s[k++] = LETTER[type];
            bool amb = false, same_file = false, same_rank = false;
            int cnt = chess_legal_moves(p, list);
            for (int i = 0; i < cnt; i++) {
                int of = CM_FROM(list[i]);
                if (CM_TO(list[i]) == to && of != from && abs(p->sq[of]) == type) {
                    amb = true;
                    if ((of & 7) == (from & 7)) same_file = true;
                    if ((of >> 3) == (from >> 3)) same_rank = true;
                }
            }
            if (amb) {
                if (!same_file) {
                    s[k++] = (char)('a' + (from & 7));
                } else if (!same_rank) {
                    s[k++] = (char)('1' + (from >> 3));
                } else {
                    s[k++] = (char)('a' + (from & 7));
                    s[k++] = (char)('1' + (from >> 3));
                }
            }
        } else if (fl & CMF_CAPTURE) {
            s[k++] = (char)('a' + (from & 7));
        }
        if (fl & CMF_CAPTURE) s[k++] = 'x';
        s[k++] = (char)('a' + (to & 7));
        s[k++] = (char)('1' + (to >> 3));
        if (CM_PROMO(m)) {
            s[k++] = '=';
            s[k++] = LETTER[CM_PROMO(m)];
        }
    }

    chess_pos_t c = *p;
    chess_make(&c, m);
    if (chess_in_check(&c)) {
        s[k++] = chess_legal_moves(&c, list) == 0 ? '#' : '+';
    }
    s[k] = 0;
    snprintf(buf, n, "%s", s);
}

/* ---------- evaluation ---------- */

/* Piece-square tables from White's side, rank 8 first (as usually printed). */
static const int8_t PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0,
};
static const int8_t PST_KNIGHT[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50,
};
static const int8_t PST_BISHOP[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20,
};
static const int8_t PST_ROOK[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0,
};
static const int8_t PST_QUEEN[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20,
};
static const int8_t PST_KING_MID[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20,
};
static const int8_t PST_KING_END[64] = {
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50,
};

/* Score from the side to move's point of view, in centipawns. */
static int evaluate(const chess_pos_t *p)
{
    int phase = 0;
    for (int s = 0; s < 64; s++) {
        int t = abs(p->sq[s]);
        if (t >= CH_KNIGHT && t <= CH_QUEEN) phase += VALUE[t];
    }
    const int8_t *king_pst = (phase <= 2600) ? PST_KING_END : PST_KING_MID;

    int score = 0, wb = 0, bb = 0;
    for (int s = 0; s < 64; s++) {
        int pc = p->sq[s];
        if (!pc) continue;
        int t = abs(pc);
        /* Tables are rank 8 first: flip the rank for White. */
        int idx = (pc > 0) ? (7 - (s >> 3)) * 8 + (s & 7) : s;
        int v = VALUE[t];
        switch (t) {
        case CH_PAWN:   v += PST_PAWN[idx]; break;
        case CH_KNIGHT: v += PST_KNIGHT[idx]; break;
        case CH_BISHOP: v += PST_BISHOP[idx]; if (pc > 0) wb++; else bb++; break;
        case CH_ROOK:   v += PST_ROOK[idx]; break;
        case CH_QUEEN:  v += PST_QUEEN[idx]; break;
        default:        v += king_pst[idx]; break;
        }
        score += (pc > 0) ? v : -v;
    }
    if (wb >= 2) score += 30;
    if (bb >= 2) score -= 30;
    return score * p->side;
}

/* ---------- search ---------- */

#define MAX_PLY 48
#define INF     32000
#define MATE    30000

typedef struct {
    int  depth;       /* maximum search depth */
    bool qs;          /* quiescence search at the leaves */
    int  noise;       /* +/- random centipawns added to each root move */
    int  time_ms;     /* thinking-time limit */
} level_t;

/* Easy: looks one move ahead and doesn't see recaptures, plus a lot of
 * randomness. Medium: two moves, some randomness. Hard/Expert: best move
 * within the time limit. */
static const level_t LEVELS[4] = {
    { 1, false, 120, 1000 },
    { 2, true,   30, 2000 },
    { 5, true,    0, 2500 },
    { 7, true,    0, 6000 },
};

static struct {
    chess_move_t  *arena;            /* MAX_PLY move lists */
    chess_move_t   killers[MAX_PLY][2];
    int64_t        deadline;
    volatile bool *abort;
    bool           stop;
    bool           use_qs;
    uint32_t       nodes;
} S;

static void check_stop(void)
{
    game_worker_yield();
    if ((S.abort && *S.abort) || game_time_us() > S.deadline) {
        S.stop = true;
    }
}

/* Moves the best-scored remaining move to index i (selection sort step). */
static inline void pick(chess_move_t *m, int i, int n)
{
    int best = i;
    for (int j = i + 1; j < n; j++) {
        if ((m[j] >> 24) > (m[best] >> 24)) best = j;
    }
    chess_move_t t = m[i];
    m[i] = m[best];
    m[best] = t;
}

static int qsearch(const chess_pos_t *p, int alpha, int beta, int ply)
{
    if ((++S.nodes & 1023) == 0) check_stop();
    if (S.stop) return 0;

    int stand = evaluate(p);
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    if (ply >= MAX_PLY - 1) return stand;

    chess_move_t *moves = S.arena + ply * CH_MAX_MOVES;
    int n = gen(p, moves, true);
    for (int i = 0; i < n; i++) {
        pick(moves, i, n);
        chess_pos_t c = *p;
        chess_make(&c, moves[i]);
        if (attacked(&c, c.king[KIDX(p->side)], c.side)) continue;
        int sc = -qsearch(&c, -beta, -alpha, ply + 1);
        if (S.stop) return 0;
        if (sc > alpha) {
            alpha = sc;
            if (alpha >= beta) break;
        }
    }
    return alpha;
}

static int negamax(const chess_pos_t *p, int depth, int alpha, int beta, int ply)
{
    if ((++S.nodes & 1023) == 0) check_stop();
    if (S.stop) return 0;
    if (p->halfmove >= 100) return 0;

    bool in_check = attacked(p, p->king[KIDX(p->side)], -p->side);
    if (in_check && ply < MAX_PLY - 8) depth++;      /* don't stop while in check */

    if (depth <= 0) return S.use_qs ? qsearch(p, alpha, beta, ply) : evaluate(p);
    if (ply >= MAX_PLY - 1) return evaluate(p);

    chess_move_t *moves = S.arena + ply * CH_MAX_MOVES;
    int n = gen(p, moves, false);

    /* Quiet moves that caused a cutoff at this ply before go first. */
    for (int i = 0; i < n; i++) {
        if ((moves[i] >> 24) == 0) {
            if (CM_SAME(moves[i], S.killers[ply][0])) moves[i] |= 90u << 24;
            else if (CM_SAME(moves[i], S.killers[ply][1])) moves[i] |= 80u << 24;
        }
    }

    int legal = 0, best = -INF;
    for (int i = 0; i < n; i++) {
        pick(moves, i, n);
        chess_move_t m = moves[i];
        chess_pos_t c = *p;
        chess_make(&c, m);
        if (attacked(&c, c.king[KIDX(p->side)], c.side)) continue;
        legal++;

        int sc = -negamax(&c, depth - 1, -beta, -alpha, ply + 1);
        if (S.stop) return 0;
        if (sc > best) best = sc;
        if (sc > alpha) {
            alpha = sc;
            if (alpha >= beta) {
                if (!(CM_FLAGS(m) & CMF_CAPTURE) && !CM_PROMO(m) &&
                    !CM_SAME(m, S.killers[ply][0])) {
                    S.killers[ply][1] = S.killers[ply][0];
                    S.killers[ply][0] = m & 0xFFFFFF;
                }
                break;
            }
        }
    }
    if (legal == 0) {
        return in_check ? -MATE + ply : 0;      /* mated (sooner is worse) or stalemate */
    }
    return best;
}

chess_move_t chess_search(const chess_pos_t *p, int level, volatile bool *abort)
{
    static chess_move_t root[CH_MAX_MOVES];
    static int score[CH_MAX_MOVES];

    if (level < 0) level = 0;
    if (level > 3) level = 3;
    const level_t *L = &LEVELS[level];

    int n = chess_legal_moves(p, root);
    if (n == 0) return 0;
    if (n == 1) return root[0];

    if (!S.arena) {
        size_t sz = sizeof(chess_move_t) * MAX_PLY * CH_MAX_MOVES;
#ifdef ESP_PLATFORM
        S.arena = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#endif
        if (!S.arena) S.arena = malloc(sz);
        if (!S.arena) return root[game_random() % n];
    }

    memset(S.killers, 0, sizeof(S.killers));
    S.abort = abort;
    S.stop = false;
    S.use_qs = L->qs;
    S.nodes = 0;
    int64_t start = game_time_us();
    S.deadline = start + (int64_t)L->time_ms * 1000;

    for (int i = 0; i < n; i++) score[i] = 0;
    chess_move_t best = root[0];
    int done_depth = 0;

    for (int depth = 1; depth <= L->depth; depth++) {
        int alpha = -INF, best_i = 0;
        for (int i = 0; i < n; i++) {
            chess_pos_t c = *p;
            chess_make(&c, root[i]);
            /* With noise every move needs an exact score (full window);
             * otherwise a null-ish window after the first move is enough. */
            int sc = L->noise ? -negamax(&c, depth - 1, -INF, INF, 1)
                              : -negamax(&c, depth - 1, -INF, -alpha, 1);
            if (S.stop) break;
            score[i] = sc;
            if (sc > alpha) {
                alpha = sc;
                best_i = i;
            }
        }
        if (S.stop) break;      /* keep the last finished depth's answer */

        /* Best move first for the next, deeper iteration. */
        chess_move_t bm = root[best_i];
        int bs = score[best_i];
        for (int j = best_i; j > 0; j--) {
            root[j] = root[j - 1];
            score[j] = score[j - 1];
        }
        root[0] = bm;
        score[0] = bs;
        best = bm;
        done_depth = depth;

        if (alpha > MATE - 100 || alpha < -MATE + 100) break;     /* mate found */
        if (game_time_us() - start > (int64_t)L->time_ms * 500) break; /* next won't finish */
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
        best = root[bi];
    }
    return best;
}
