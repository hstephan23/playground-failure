#pragma once

#include <stdint.h>

typedef struct { uint64_t s[4]; } pg_xosh_t;

void     pg_xosh_seed(pg_xosh_t *r, uint64_t seed);
uint64_t pg_xosh_next(pg_xosh_t *r);
