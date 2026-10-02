#include "selection.h"

#include <math.h>

bool selection_get_box(const struct swappy_state *state,
                       struct swappy_box *box) {
  if (!state->selection.enabled || !state->original_image) {
    return false;
  }

  int width = gdk_pixbuf_get_width(state->original_image);
  int height = gdk_pixbuf_get_height(state->original_image);
  struct swappy_box selected = state->selection.box;
  int right = CLAMP(selected.x + selected.width, 0, width);
  int bottom = CLAMP(selected.y + selected.height, 0, height);
  selected.x = CLAMP(selected.x, 0, width);
  selected.y = CLAMP(selected.y, 0, height);
  selected.width = right - selected.x;
  selected.height = bottom - selected.y;
  if (selected.width <= 0 || selected.height <= 0) {
    return false;
  }

  *box = selected;
  return true;
}

bool selection_contains(const struct swappy_state *state, double x, double y) {
  struct swappy_box box;
  return selection_get_box(state, &box) && x >= box.x && y >= box.y &&
         x < box.x + box.width && y < box.y + box.height;
}

bool selection_begin_drag(struct swappy_state *state, double x, double y,
                          double tolerance_x, double tolerance_y) {
  if (!state->selection.enabled) {
    return false;
  }

  struct swappy_box box;
  struct swappy_point anchor = {x, y};
  bool at_corner = false;
  if (selection_get_box(state, &box)) {
    double right = box.x + box.width;
    double bottom = box.y + box.height;
    bool left_edge = fabs(x - box.x) <= tolerance_x;
    bool right_edge = fabs(x - right) <= tolerance_x;
    bool top_edge = fabs(y - box.y) <= tolerance_y;
    bool bottom_edge = fabs(y - bottom) <= tolerance_y;
    at_corner = (left_edge || right_edge) && (top_edge || bottom_edge);
    if (at_corner) {
      anchor.x = left_edge ? right : box.x;
      anchor.y = top_edge ? bottom : box.y;
    } else if (selection_contains(state, x, y)) {
      return false;
    }
  }

  state->selection.previous_box = state->selection.box;
  state->selection.anchor = anchor;
  state->selection.dragging = true;
  selection_update_drag(state, x, y);
  return true;
}

void selection_update_drag(struct swappy_state *state, double x, double y) {
  if (!state->selection.dragging) {
    return;
  }

  int width = gdk_pixbuf_get_width(state->original_image);
  int height = gdk_pixbuf_get_height(state->original_image);
  x = CLAMP(x, 0, width);
  y = CLAMP(y, 0, height);
  double anchor_x = state->selection.anchor.x;
  double anchor_y = state->selection.anchor.y;
  int left = (int)floor(MIN(anchor_x, x));
  int top = (int)floor(MIN(anchor_y, y));
  int right = (int)ceil(MAX(anchor_x, x));
  int bottom = (int)ceil(MAX(anchor_y, y));
  state->selection.box =
      (struct swappy_box){left, top, right - left, bottom - top};
}

void selection_finish_drag(struct swappy_state *state, double x, double y) {
  selection_update_drag(state, x, y);
  if (state->selection.box.width <= 0 || state->selection.box.height <= 0) {
    state->selection.box = state->selection.previous_box;
  }
  state->selection.dragging = false;
}

void selection_draw_overlay(struct swappy_state *state, cairo_t *cr,
                            double scale_x, double scale_y) {
  struct swappy_box box;
  if (!selection_get_box(state, &box)) {
    return;
  }

  /* Draw in widget coordinates so the border and handles stay the same size. */
  double x = box.x * scale_x;
  double y = box.y * scale_y;
  double width = box.width * scale_x;
  double height = box.height * scale_y;
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0,
                  gdk_pixbuf_get_width(state->original_image) * scale_x,
                  gdk_pixbuf_get_height(state->original_image) * scale_y);
  cairo_rectangle(cr, x, y, width, height);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_fill(cr);

  cairo_rectangle(cr, x, y, width, height);
  cairo_set_line_width(cr, 1);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_stroke_preserve(cr);
  const double dashes[] = {4, 4};
  cairo_set_dash(cr, dashes, G_N_ELEMENTS(dashes), 0);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0);

  for (int corner = 0; corner < 4; corner++) {
    double corner_x = x + ((corner & 1) ? width : 0);
    double corner_y = y + ((corner & 2) ? height : 0);
    cairo_rectangle(cr, corner_x - 3, corner_y - 3, 6, 6);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}
