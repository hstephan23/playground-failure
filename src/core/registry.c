#include "playground/scenario.h"

#include <string.h>

#define PG_REG_MAX 256

static const pg_scenario_t *g_reg[PG_REG_MAX];
static size_t               g_reg_n = 0;

void pg_registry_add(const pg_scenario_t *s) {
    if (s && g_reg_n < PG_REG_MAX) {
        g_reg[g_reg_n++] = s;
    }
}

const pg_scenario_t **pg_registry_list(size_t *out_n) {
    if (out_n) *out_n = g_reg_n;
    return g_reg;
}

const pg_scenario_t *pg_registry_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < g_reg_n; ++i) {
        if (g_reg[i] && strcmp(g_reg[i]->id, id) == 0) {
            return g_reg[i];
        }
    }
    return NULL;
}
