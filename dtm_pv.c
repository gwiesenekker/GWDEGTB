#include "dtm_pv.h"
#include "material.h"
#include "endgame_index.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct Database {
    char name[80];
    EgIndexer indexer;
    Egtb *db;
    struct Database *next;
} Database;
typedef struct {
    const char *directory;
    char *error;
    size_t error_size;
    Database *files;
} Context;
static bool failure(Context *c, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    if (c->error && c->error_size) vsnprintf(c->error, c->error_size, fmt, args);
    va_end(args); return false;
}
static void spaces(const char **s)
{
    while (isspace((unsigned char)**s)) ++*s;
}
static bool parse(const char *s, DraughtsPosition *p, EgtbSide *side)
{
    unsigned seen = 0, count = 0;
    uint64_t occupied = 0;
    memset(p, 0, sizeof *p);
    spaces(&s);
    if (*s != 'W' && *s != 'B') return false;
    *side = *s++ == 'W' ? EGTB_WHITE_TO_MOVE : EGTB_BLACK_TO_MOVE;
    for (unsigned section = 0; section < 2; ++section) {
        spaces(&s); if (*s++ != ':') return false;
        spaces(&s); char colour = *s;
        if (colour != 'W' && colour != 'B') return false;
        ++s;
        unsigned flag = colour == 'W' ? 1 : 2;
        if (seen & flag) return false;
        seen |= flag; spaces(&s);
        while (*s == 'K' || isdigit((unsigned char)*s)) {
            bool king = *s == 'K'; if (king) ++s;
            if (!isdigit((unsigned char)*s)) return false;
            unsigned square = 0;
            do {
                square = square * 10 + (unsigned)(*s++ - '0');
                if (square > 50) return false;
            } while (isdigit((unsigned char)*s));
            if (!square || ++count > EGTB_MAX_PIECES) return false;
            uint64_t bit = UINT64_C(1) << (square - 1);
            if (occupied & bit) return false;
            occupied |= bit;
            uint64_t *board = colour == 'W'
                ? (king ? &p->white_kings : &p->white_men)
                : (king ? &p->black_kings : &p->black_men);
            *board |= bit; spaces(&s);
            if (*s != ',') break;
            ++s; spaces(&s);
            if (*s != 'K' && !isdigit((unsigned char)*s)) return false;
        }
    }
    spaces(&s); if (*s == '.') { ++s; spaces(&s); }
    return !*s && count && draughts_position_is_valid(p);
}
static bool probe(Context *c, const DraughtsPosition *p, EgtbSide side, int16_t *v)
{
    uint64_t own = side == EGTB_WHITE_TO_MOVE ? p->white_men | p->white_kings
                                             : p->black_men | p->black_kings;
    if (!own) { *v = 0; return true; }
    EgtbMaterial m = egtb_position_material(p), canonical;
    EgtbMaterialKind kind = egtb_material_resolve(&m, &canonical);
    if (kind == EGTB_MATERIAL_INVALID || kind == EGTB_MATERIAL_TERMINAL)
        return failure(c, "unsupported material (opponent has no pieces)");
    char name[80];
    if (!egtb_material_filename(name, sizeof name, canonical.white_kings,
        canonical.white_men, canonical.black_kings, canonical.black_men, "dtm"))
        return failure(c, "cannot format material name");
    Database *d;
    for (d = c->files; d && strcmp(d->name, name); d = d->next) {}
    if (!d) {
        d = calloc(1, sizeof *d);
        if (!d) return failure(c, "out of memory");
        strcpy(d->name, name); d->next = c->files; c->files = d;
        if (!eg_indexer_init(&d->indexer, canonical.white_men, canonical.black_men,
                            canonical.white_kings, canonical.black_kings))
            return failure(c, "cannot create indexer for %s", name);
        size_t length = strlen(c->directory) + strlen(name) + 2;
        char *path = malloc(length);
        if (!path) return failure(c, "out of memory");
        snprintf(path, length, "%s/%s", c->directory, name);
        bool ok = egtb_open_readonly(&d->db, path, 1);
        free(path);
        if (!ok) return failure(c, "%s: %s", name, egtb_last_error());
        if (egtb_maximum_index(d->db) != eg_max_index(&d->indexer))
            return failure(c, "%s: unexpected index range", name);
    }
    DraughtsPosition q = *p;
    if (kind == EGTB_MATERIAL_MIRROR) {
        egtb_mirror_position(p, &q); side = egtb_mirror_side(side);
    }
    EgPosition ep = {q.white_men, q.black_men, q.white_kings, q.black_kings};
    uint64_t index;
    if (!eg_position_to_index(&d->indexer, &ep, &index) ||
        !egtb_get_uncached(d->db, index, side, v))
        return failure(c, "%s: lookup failed: %s", name, egtb_last_error());
    return true;
}
typedef struct {
    DraughtsPosition position;
    DraughtsMove move;
} Alternative;
typedef struct {
    Context *context;
    DraughtsPosition position;
    EgtbSide side;
    DraughtsMove best;
    int win, longest;
    bool all_won, ok;
    Alternative *ties;
    size_t tie_count, tie_capacity;
} Choices;
static bool child(const DraughtsMove *move, void *opaque)
{
    Choices *s = opaque;
    DraughtsPosition p = s->position;
    int16_t value;
    if (!draughts_do_move(&p, s->side, move, NULL) ||
        !probe(s->context, &p, egtb_mirror_side(s->side), &value)) {
        s->ok = false; return false;
    }
    bool better = false, equal = false;
    if (value <= 0 && value != EGTB_DRAW) {
        int win = 1 - value;
        better = win < s->win;
        equal = win == s->win;
        if (better) { s->win = win; s->best = *move; }
    }
    if (value <= 0) s->all_won = false;
    if (value > s->longest) {
        s->longest = value;
        if (s->win == 32768) { s->best = *move; better = true; }
    }
    else if (value > 0 && value == s->longest && s->win == 32768) equal = true;
    if (better) s->tie_count = 0;
    if (better || equal) {
        for (size_t i = 0; i < s->tie_count; ++i)
            if (s->ties[i].position.white_men == p.white_men && s->ties[i].position.black_men == p.black_men &&
                s->ties[i].position.white_kings == p.white_kings && s->ties[i].position.black_kings == p.black_kings)
                return true;
        if (s->tie_count == s->tie_capacity) {
            size_t capacity = s->tie_capacity ? 2 * s->tie_capacity : 16;
            void *data = realloc(s->ties, capacity * sizeof *s->ties);
            if (!data) { s->ok = false; return failure(s->context, "out of memory"); }
            s->ties = data; s->tie_capacity = capacity;
        }
        s->ties[s->tie_count++] = (Alternative){p, *move};
    }
    return true;
}
/* Wrap between tokens, including alternatives inside long PDN comments. */
static void print_token(FILE *out, size_t *column, const char *token)
{
    size_t length = strlen(token);
    if (*column) {
        if (*column + 1 + length > 80) {
            fputc('\n', out); *column = 0;
        } else {
            fputc(' ', out); ++*column;
        }
    }
    fputs(token, out); *column += length;
}
bool gwdegtb_dtm_pv(const char *directory, const char *fen, FILE *output,
                    char *error, size_t error_size)
{
    Context c = {directory, error, error_size, NULL};
    DraughtsPosition p;
    EgtbSide side;
    bool ok = false;
    if (error && error_size) *error = 0;
    if (!directory || !fen || !output || !parse(fen, &p, &side))
        return failure(&c, "invalid FEN (use W:W27,K32:B12,K8; at most eight pieces)");
    int expected = 32768;
    unsigned move_number = 1;
    size_t column = 0;
    for (unsigned ply = 0; ply <= EGTB_MAX_LOSS_DTM; ++ply) {
        int16_t value;
        if (!probe(&c, &p, side, &value)) goto done;
        if (expected != 32768 && value != expected) {
            failure(&c, "DTM progression mismatch at ply %u: stored=%d expected=%d", ply, value, expected);
            goto done;
        }
        if (value == EGTB_DRAW) {
            print_token(output, &column, "1-1"); ok = true; goto done;
        }
        Choices choices = {.context=&c, .position=p, .side=side,
                           .win=32768, .all_won=true, .ok=true};
        size_t count;
        if (!draughts_generate_moves_padded(&p, side, child, &choices, &count)) {
            free(choices.ties);
            if (choices.ok) failure(&c, "move generation failed: %s", draughts_movegen_last_error());
            goto done;
        }
        int calculated = !count ? 0 : choices.win != 32768 ? choices.win
            : choices.all_won ? -(choices.longest + 1) : EGTB_DRAW;
        if (calculated != value) {
            free(choices.ties);
            failure(&c, "inconsistent DTM at ply %u: stored=%d expected=%d", ply, value, calculated);
            goto done;
        }
        if (!count) {
            free(choices.ties);
            print_token(output, &column, side == EGTB_WHITE_TO_MOVE ? "0-2" : "2-0");
            ok = true; goto done;
        }
        DraughtsMove *m = &choices.best;
        char token[128];
        if (side == EGTB_WHITE_TO_MOVE || ply == 0)
            snprintf(token, sizeof token, "%u%s %u%c%u {%d%s", move_number,
                     side == EGTB_WHITE_TO_MOVE ? "." : "...",
                     m->from + 1, m->captured ? 'x' : '-', m->to + 1, value,
                     choices.tie_count > 1 ? "," : "}");
        else
            snprintf(token, sizeof token, "%u%c%u {%d%s",
                     m->from + 1, m->captured ? 'x' : '-', m->to + 1, value,
                     choices.tie_count > 1 ? "," : "}");
        print_token(output, &column, token);
        /* The first distinct successor is the selected move. */
        for (size_t i = 1; i < choices.tie_count; ++i) {
            const DraughtsMove *alternative = &choices.ties[i].move;
            snprintf(token, sizeof token, "%u%c%u%s", alternative->from + 1,
                     alternative->captured ? 'x' : '-', alternative->to + 1,
                     i + 1 == choices.tie_count ? "}" : " or");
            print_token(output, &column, token);
        }
        free(choices.ties);
        if (!draughts_do_move(&p, side, m, NULL)) { failure(&c, "cannot apply move"); goto done; }
        expected = value > 0 ? 1 - value : -value - 1;
        if (side == EGTB_BLACK_TO_MOVE) ++move_number;
        side = egtb_mirror_side(side);
    }
    failure(&c, "PV exceeds maximum DTM length");
done:
    if (column) fputc('\n', output);
    while (c.files) {
        Database *d = c.files; c.files = d->next;
        if (d->db) egtb_close(d->db);
        eg_indexer_destroy(&d->indexer); free(d);
    }
    if (ok && ferror(output)) return failure(&c, "output write failed");
    return ok;
}
