#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define RING_BUFFER_SIZE 16

typedef struct {
    int16_t *data;
    size_t actual_sample_count;
    bool is_full;
} iq_samps_t;

typedef struct {
    iq_samps_t ring[RING_BUFFER_SIZE];
    size_t element_size;
    uint32_t write_idx;
    uint32_t read_idx;
} ring_buffer_t;

ring_buffer_t* ring_buffer_create(size_t element_size);
void ring_buffer_destroy(ring_buffer_t *rb);