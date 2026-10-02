#pragma once

#include "swappy.h"

bool selection_get_box(const struct swappy_state *state,
                       struct swappy_box *box);
bool selection_contains(const struct swappy_state *state, double x, double y);
bool selection_begin_drag(struct swappy_state *state, double x, double y,
                          double tolerance_x, double tolerance_y);
void selection_update_drag(struct swappy_state *state, double x, double y);
void selection_finish_drag(struct swappy_state *state, double x, double y);
void selection_draw_overlay(struct swappy_state *state, cairo_t *cr,
                            double scale_x, double scale_y);
