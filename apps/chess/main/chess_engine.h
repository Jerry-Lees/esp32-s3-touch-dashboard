#pragma once
/*
 * Small chess engine: full rules (castling, en passant, promotion, check,
 * mate, stalemate, 50-move rule, insufficient material) and an alpha-beta
 * search with four difficulty levels.
 *
 * No LVGL here. chess_search() is meant to run in the game worker task;
 * everything else is cheap and can run in the UI.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CH_EMPTY = 0, CH_PAWN, CH_KNIGHT, CH_BISHOP, CH_ROOK, CH_QUEEN, CH_KING };

#define CH_WHITE   1
#define CH_BLACK  (-1)

/* Squares: index = rank * 8 + file; rank 0 is White's back rank (rank 1),
 * file 0 is the a-file. Pieces: +type for White, -type for Black. */
typedef struct {
    int8_t   sq[64];
    int8_t   side;        /* CH_WHITE or CH_BLACK to move */
    uint8_t  castle;      /* bit 0 = White O-O, 1 = White O-O-O, 2/3 = Black */
    int8_t   ep;          /* en-passant target square, or -1 */
    uint8_t  halfmove;    /* plies since the last capture or pawn move */
    uint16_t fullmove;
    uint8_t  king[2];     /* king squares: [0] White, [1] Black */
} chess_pos_t;

/* A move packed into 32 bits: from (6 bits), to (6), promotion piece (3),
 * flags (8), ordering score (8). 0 means "no move". */
typedef uint32_t chess_move_t;
#define CM_FROM(m)    ((int)((m) & 63))
#define CM_TO(m)      ((int)(((m) >> 6) & 63))
#define CM_PROMO(m)   ((int)(((m) >> 12) & 7))
#define CM_FLAGS(m)   ((int)(((m) >> 16) & 0xFF))
#define CM_SAME(a, b) ((((a) ^ (b)) & 0xFFFFFF) == 0)

#define CMF_CAPTURE   0x01
#define CMF_EP        0x02
#define CMF_CASTLE    0x04
#define CMF_DOUBLE    0x08

#define CH_MAX_MOVES  256

typedef enum {
    CH_PLAYING,
    CH_CHECKMATE,         /* side to move is mated */
    CH_STALEMATE,
    CH_DRAW_50,
    CH_DRAW_MATERIAL,
} chess_status_t;

void chess_new_game(chess_pos_t *p);
void chess_make(chess_pos_t *p, chess_move_t m);
bool chess_in_check(const chess_pos_t *p);
int  chess_legal_moves(const chess_pos_t *p, chess_move_t *out);
chess_status_t chess_status(const chess_pos_t *p);
/* For spotting repeated positions (board, side, castling, en passant). */
uint32_t chess_hash(const chess_pos_t *p);
/* Standard notation such as "Nf3", "exd5", "O-O", "e8=Q+". UI task only. */
void chess_move_to_san(const chess_pos_t *p, chess_move_t m, char *buf, size_t n);

/* Picks a move for the side to move. level 0..3 = Easy..Expert.
 * Returns 0 if there are no legal moves. Stops early if *abort becomes true.
 * Only one search may run at a time. */
chess_move_t chess_search(const chess_pos_t *p, int level, volatile bool *abort);
