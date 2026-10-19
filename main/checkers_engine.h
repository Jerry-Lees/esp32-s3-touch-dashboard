#pragma once
/*
 * Checkers (American rules / English draughts) engine: 8x8 board, men move
 * diagonally forward, captures are mandatory, multi-jumps must be finished,
 * a man that reaches the far row becomes a king (which ends the move),
 * kings move and jump in all four diagonal directions.
 *
 * No LVGL here. ck_search() is meant to run in the game worker task.
 */
#include <stdbool.h>
#include <stdint.h>

#define CK_RED     1          /* moves first, starts on rows 0-2, moves "up" */
#define CK_WHITE  (-1)

/* sq[row * 8 + col]: 0 empty, +1 red man, +2 red king, -1 white man,
 * -2 white king. Only dark squares ((row + col) even) are used. */
typedef struct {
    int8_t  sq[64];
    int8_t  side;
    uint8_t quiet;        /* plies since the last capture or man move */
} ck_pos_t;

#define CK_MAX_PATH  13       /* start + up to 12 landing squares */
#define CK_MAX_MOVES 64

typedef struct {
    uint8_t len;              /* squares in path (>= 2) */
    uint8_t ncap;             /* pieces captured (0 = simple move) */
    uint8_t path[CK_MAX_PATH];
} ck_move_t;

typedef enum {
    CK_PLAYING,
    CK_LOSS,                  /* side to move has no moves: it lost */
    CK_DRAW,                  /* 40 moves each without a capture or man move */
} ck_status_t;

void ck_new_game(ck_pos_t *p);
int  ck_legal_moves(const ck_pos_t *p, ck_move_t *out);   /* out: CK_MAX_MOVES */
void ck_make(ck_pos_t *p, const ck_move_t *m);
ck_status_t ck_status(const ck_pos_t *p);
uint32_t ck_hash(const ck_pos_t *p);

/* Picks a move for the side to move; level 0..3 = Easy..Expert.
 * Returns false if there is no legal move. Only one search at a time. */
bool ck_search(const ck_pos_t *p, int level, volatile bool *abort, ck_move_t *out);
