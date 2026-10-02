#include "selection.h"

#include <glib/gstdio.h>
#include <pango/pangocairo.h>
#include <tesseract/capi.h>

#include "application.h"
#include "ocr.h"
#include "paint.h"
#include "pixbuf.h"
#include "render.h"

struct fixture {
  struct swappy_state state;
  struct swappy_state_ui ui;
  struct swappy_config config;
  GtkBuilder *builder;
};

static void drain_events(void) {
  while (gtk_events_pending()) {
    gtk_main_iteration();
  }
}

static GtkWidget *widget(struct fixture *fixture, const char *id) {
  return GTK_WIDGET(gtk_builder_get_object(fixture->builder, id));
}

static void setup(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  state->ui = &fixture->ui;
  state->config = &fixture->config;
  state->settings = (struct swappy_state_settings){1, 0, 0, 1, 20, 20, 50};
  fixture->config.text_font = "Sans";
  state->original_image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 320, 200);
  gdk_pixbuf_fill(state->original_image, 0xffffffff);
  state->ui->im_context = gtk_im_multicontext_new();
  fixture->builder =
      gtk_builder_new_from_resource("/me/jtheoof/swappy/swappy.glade");
  state->ui->window = GTK_WINDOW(widget(fixture, "paint-window"));
  state->ui->area = widget(fixture, "painting-area");
  state->ui->ocr_overlay = widget(fixture, "ocr-overlay");
  state->ui->painting_box = GTK_BOX(widget(fixture, "painting-box"));
  state->ui->ocr = GTK_TOGGLE_BUTTON(widget(fixture, "ocr-button"));
  state->ui->selection = GTK_TOGGLE_BUTTON(widget(fixture, "selection-button"));
  state->ui->crop = GTK_BUTTON(widget(fixture, "crop-button"));
  state->ui->undo = GTK_BUTTON(widget(fixture, "undo-button"));
  state->ui->redo = GTK_BUTTON(widget(fixture, "redo-button"));
  state->ui->fill_shape =
      GTK_TOGGLE_BUTTON(widget(fixture, "fill-shape-toggle-button"));
  gtk_builder_connect_signals(fixture->builder, state);
  gtk_widget_set_size_request(state->ui->area, 320, 200);
  gtk_widget_show(GTK_WIDGET(state->ui->painting_box));
  gtk_widget_show(GTK_WIDGET(state->ui->window));
  gtk_test_widget_wait_for_draw(state->ui->area);
  drain_events();
  pixbuf_scale_surface_from_widget(state, state->ui->area);
  render_state(state);
}

static void teardown(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  gtk_widget_destroy(GTK_WIDGET(state->ui->window));
  paint_free_all(state);
  pixbuf_free(state);
  cairo_surface_destroy(state->original_image_surface);
  cairo_surface_destroy(state->rendering_surface);
  g_object_unref(state->ui->im_context);
  g_object_unref(fixture->builder);
  g_free(state->window);
  drain_events();
}

static void set_selection(struct fixture *fixture) {
  gtk_toggle_button_set_active(fixture->ui.selection, TRUE);
  fixture->state.selection.box = (struct swappy_box){80, 50, 120, 100};
}

static void assert_box(struct swappy_box box, int x, int y, int w, int h) {
  g_assert_cmpint(box.x, ==, x);
  g_assert_cmpint(box.y, ==, y);
  g_assert_cmpint(box.width, ==, w);
  g_assert_cmpint(box.height, ==, h);
}

static void drag(struct fixture *fixture, double from_x, double from_y,
                 double to_x, double to_y) {
  struct swappy_state *state = &fixture->state;
  GtkAllocation alloc;
  gtk_widget_get_allocation(state->ui->area, &alloc);
  double sx = (double)alloc.width / gdk_pixbuf_get_width(state->original_image);
  double sy =
      (double)alloc.height / gdk_pixbuf_get_height(state->original_image);
  GdkWindow *window = gtk_widget_get_window(state->ui->area);
  GdkEventButton press = {.button = 1, .x = from_x * sx, .y = from_y * sy};
  GdkEventMotion motion = {.window = window,
                           .state = GDK_BUTTON1_MASK,
                           .x = to_x * sx,
                           .y = to_y * sy};
  GdkEventButton release = {.button = 1, .x = to_x * sx, .y = to_y * sy};
  draw_area_button_press_handler(state->ui->area, &press, state);
  draw_area_motion_notify_handler(state->ui->area, &motion, state);
  draw_area_button_release_handler(state->ui->area, &release, state);
}

static void assert_pixel(struct swappy_state *state, int x, int y, int red,
                         int green, int blue) {
  GdkPixbuf *pixbuf =
      gdk_pixbuf_get_from_surface(state->rendering_surface, x, y, 1, 1);
  const guchar *pixel = gdk_pixbuf_get_pixels(pixbuf);
  g_assert_cmpint(pixel[0], ==, red);
  g_assert_cmpint(pixel[1], ==, green);
  g_assert_cmpint(pixel[2], ==, blue);
  g_object_unref(pixbuf);
}

static void test_drag_and_toggle(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  gtk_toggle_button_set_active(fixture->ui.selection, TRUE);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(fixture->ui.crop)));
  /* Use the actual allocation rather than the initial monitor scale. */
  state->scaling_factor = 0.5;
  drag(fixture, 200, 150, 80, 50);
  assert_box(state->selection.box, 80, 50, 120, 100);
  g_assert_null(state->paints);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(fixture->ui.crop)));

  GtkAllocation select_alloc, ocr_alloc, crop_alloc, fill_alloc;
  gtk_widget_get_allocation(GTK_WIDGET(fixture->ui.selection), &select_alloc);
  gtk_widget_get_allocation(GTK_WIDGET(fixture->ui.ocr), &ocr_alloc);
  gtk_widget_get_allocation(GTK_WIDGET(fixture->ui.crop), &crop_alloc);
  gtk_widget_get_allocation(GTK_WIDGET(fixture->ui.fill_shape), &fill_alloc);
  g_assert_cmpint(select_alloc.x + select_alloc.width, <=, ocr_alloc.x);
  g_assert_cmpint(crop_alloc.y, >=, fill_alloc.y + fill_alloc.height);

  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  struct swappy_box box;
  g_assert_false(selection_get_box(state, &box));
  gtk_toggle_button_set_active(fixture->ui.selection, TRUE);
  assert_box(state->selection.box, 80, 50, 120, 100);
  drag(fixture, 200, 150, 220, 170);
  assert_box(state->selection.box, 80, 50, 140, 120);
  drag(fixture, 20, 20, 20, 20);
  assert_box(state->selection.box, 80, 50, 140, 120);
  drag(fixture, 300, 190, -20, -20);
  assert_box(state->selection.box, 0, 0, 300, 190);
}

static void test_clipped_edits(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  set_selection(fixture);
  state->mode = SWAPPY_PAINT_MODE_BRUSH;
  drag(fixture, 100, 100, 260, 100);
  assert_pixel(state, 150, 100, 255, 0, 0);
  assert_pixel(state, 205, 100, 255, 255, 255);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  assert_pixel(state, 205, 100, 255, 255, 255);
  undo_clicked_handler(NULL, state);
  assert_pixel(state, 150, 100, 255, 255, 255);
  redo_clicked_handler(NULL, state);
  assert_pixel(state, 150, 100, 255, 0, 0);
  state->selection.box = (struct swappy_box){20, 20, 30, 30};
  gtk_toggle_button_set_active(fixture->ui.selection, TRUE);
  assert_pixel(state, 150, 100, 255, 0, 0);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  drag(fixture, 220, 100, 260, 100);
  assert_pixel(state, 240, 100, 255, 0, 0);
}

static void test_shapes_and_text(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  set_selection(fixture);
  fixture->config.fill_shape = TRUE;
  paint_add_temporary(state, 10, 10, SWAPPY_PAINT_MODE_RECTANGLE);
  paint_update_temporary_shape(state, 250, 180, FALSE);
  paint_commit_temporary(state);
  render_state(state);
  assert_pixel(state, 100, 60, 255, 0, 0);
  assert_pixel(state, 40, 40, 255, 255, 255);
  assert_pixel(state, 210, 100, 255, 255, 255);

  paint_free_all(state);
  paint_add_temporary(state, 190, 80, SWAPPY_PAINT_MODE_TEXT);
  paint_update_temporary_text_clip(state, 300, 140);
  paint_update_temporary_str(state, "MMMMMMMM");
  paint_commit_temporary(state);
  render_state(state);
  for (int y = 75; y < 150; y++) {
    for (int x = 200; x < 310; x++) {
      assert_pixel(state, x, y, 255, 255, 255);
    }
  }
  g_assert_true(((struct swappy_paint *)state->paints->data)->has_clip);
}

static void test_export_and_crop(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  set_selection(fixture);
  drag(fixture, 100, 100, 260, 100);
  GdkPixbuf *selected = pixbuf_get_from_state(state);
  g_assert_cmpint(gdk_pixbuf_get_width(selected), ==, 120);
  g_assert_cmpint(gdk_pixbuf_get_height(selected), ==, 100);
  g_object_unref(selected);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  GdkPixbuf *full = pixbuf_get_from_state(state);
  g_assert_cmpint(gdk_pixbuf_get_width(full), ==, 320);
  g_object_unref(full);
  gtk_toggle_button_set_active(fixture->ui.selection, TRUE);
  gtk_button_clicked(fixture->ui.crop);
  gtk_test_widget_wait_for_draw(state->ui->area);
  drain_events();
  g_assert_cmpint(gdk_pixbuf_get_width(state->original_image), ==, 120);
  g_assert_cmpint(gdk_pixbuf_get_height(state->original_image), ==, 100);
  assert_box(state->selection.box, 0, 0, 120, 100);
  g_assert_null(state->paints);
  g_assert_null(state->redo_paints);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(fixture->ui.undo)));
  assert_pixel(state, 70, 50, 255, 0, 0);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  state->mode = SWAPPY_PAINT_MODE_BRUSH;
  drag(fixture, 50, 80, 100, 80);
  assert_pixel(state, 70, 80, 255, 0, 0);
}

static void test_clipped_blur(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  cairo_t *cr = cairo_create(state->original_image_surface);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_rectangle(cr, 190, 70, 2, 70);
  cairo_rectangle(cr, 210, 70, 2, 70);
  cairo_fill(cr);
  cairo_destroy(cr);
  set_selection(fixture);
  state->mode = SWAPPY_PAINT_MODE_BLUR;
  drag(fixture, 100, 80, 230, 130);
  assert_pixel(state, 210, 100, 0, 0, 0);
  GdkPixbuf *inside =
      gdk_pixbuf_get_from_surface(state->rendering_surface, 190, 100, 1, 1);
  g_assert_cmpint(gdk_pixbuf_get_pixels(inside)[0], >, 0);
  g_assert_cmpint(gdk_pixbuf_get_pixels(inside)[0], <, 255);
  g_object_unref(inside);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  undo_clicked_handler(NULL, state);
  assert_pixel(state, 190, 100, 0, 0, 0);
  redo_clicked_handler(NULL, state);
  assert_pixel(state, 210, 100, 0, 0, 0);
}

static void test_selected_clear(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  drag(fixture, 30, 100, 260, 100);
  set_selection(fixture);
  clear_clicked_handler(NULL, state);
  assert_pixel(state, 150, 100, 255, 255, 255);
  assert_pixel(state, 240, 100, 255, 0, 0);
  undo_clicked_handler(NULL, state);
  assert_pixel(state, 150, 100, 255, 0, 0);
  redo_clicked_handler(NULL, state);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  assert_pixel(state, 150, 100, 255, 255, 255);
  assert_pixel(state, 240, 100, 255, 0, 0);
}

static void test_save_and_copy(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  gchar *parent =
      g_build_filename(g_get_user_state_dir(), "swappy-tests", NULL);
  g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
  gchar *directory = g_build_filename(parent, "selection-XXXXXX", NULL);
  g_assert_nonnull(g_mkdtemp(directory));
  fixture->config.save_dir = directory;
  fixture->config.save_filename_format = "selected.png";
  gchar *path = g_build_filename(directory, "selected.png", NULL);
  set_selection(fixture);
  drag(fixture, 100, 100, 180, 100);
  save_clicked_handler(NULL, state);
  GError *error = NULL;
  GdkPixbuf *saved = gdk_pixbuf_new_from_file(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(saved);
  g_assert_cmpint(gdk_pixbuf_get_width(saved), ==, 120);
  g_assert_cmpint(gdk_pixbuf_get_height(saved), ==, 100);
  const guchar *pixel = gdk_pixbuf_get_pixels(saved) +
                        50 * gdk_pixbuf_get_rowstride(saved) +
                        70 * gdk_pixbuf_get_n_channels(saved);
  g_assert_cmpint(pixel[1], ==, 0);
  g_object_unref(saved);

  copy_clicked_handler(NULL, state);
  GdkPixbuf *copied =
      gtk_clipboard_wait_for_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
  g_assert_nonnull(copied);
  g_assert_cmpint(gdk_pixbuf_get_width(copied), ==, 120);
  g_assert_cmpint(gdk_pixbuf_get_height(copied), ==, 100);
  g_object_unref(copied);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  save_clicked_handler(NULL, state);
  saved = gdk_pixbuf_new_from_file(path, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gdk_pixbuf_get_width(saved), ==, 320);
  g_assert_cmpint(gdk_pixbuf_get_height(saved), ==, 200);
  g_object_unref(saved);
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(path);
  g_free(directory);
  g_free(parent);
}

static void test_ocr_offset(struct fixture *fixture, gconstpointer data) {
  struct swappy_state *state = &fixture->state;
  TessBaseAPI *api = TessBaseAPICreate();
  int initialized = TessBaseAPIInit3(api, NULL, "eng");
  TessBaseAPIDelete(api);
  if (initialized != 0) {
    g_test_skip("Tesseract English language data is not installed.");
    return;
  }
  cairo_t *cr = cairo_create(state->original_image_surface);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font = pango_font_description_from_string("Sans 20");
  pango_layout_set_font_description(layout, font);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_move_to(cr, 15, 10);
  pango_layout_set_text(layout, "OUTSIDE", -1);
  pango_cairo_show_layout(cr, layout);
  cairo_move_to(cr, 90, 80);
  pango_layout_set_text(layout, "INSIDE", -1);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
  cairo_destroy(cr);
  set_selection(fixture);
  render_state(state);
  gtk_toggle_button_set_active(fixture->ui.ocr, TRUE);
  g_assert_true(state->ui->ocr_active);
  GList *children =
      gtk_container_get_children(GTK_CONTAINER(fixture->ui.ocr_overlay));
  g_assert_cmpuint(g_list_length(children), ==, 1);
  GtkWidget *view = children->data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(text, ==, "INSIDE");
  g_free(text);
  int x, y;
  gtk_container_child_get(GTK_CONTAINER(fixture->ui.ocr_overlay), view, "x", &x,
                          "y", &y, NULL);
  g_assert_cmpint(x, >=, 90);
  g_assert_cmpint(y, >=, 80);
  g_list_free(children);
  gtk_toggle_button_set_active(fixture->ui.selection, FALSE);
  g_assert_false(state->ui->ocr_active);
  g_assert_false(gtk_widget_get_visible(fixture->ui.ocr_overlay));
}

int main(int argc, char **argv) {
  /* Copy falls back to the isolated Xvfb clipboard, never the user's Wayland
   * session. */
  g_setenv("WAYLAND_DISPLAY", "swappy-tests-no-display", TRUE);
  g_test_init(&argc, &argv, NULL);
  gtk_init(&argc, &argv);
  g_test_add("/selection/drag-and-toggle", struct fixture, NULL, setup,
             test_drag_and_toggle, teardown);
  g_test_add("/selection/clipped-edits", struct fixture, NULL, setup,
             test_clipped_edits, teardown);
  g_test_add("/selection/shapes-and-text", struct fixture, NULL, setup,
             test_shapes_and_text, teardown);
  g_test_add("/selection/export-and-crop", struct fixture, NULL, setup,
             test_export_and_crop, teardown);
  g_test_add("/selection/blur", struct fixture, NULL, setup, test_clipped_blur,
             teardown);
  g_test_add("/selection/clear", struct fixture, NULL, setup,
             test_selected_clear, teardown);
  g_test_add("/selection/save-and-copy", struct fixture, NULL, setup,
             test_save_and_copy, teardown);
  g_test_add("/selection/ocr-offset", struct fixture, NULL, setup,
             test_ocr_offset, teardown);
  return g_test_run();
}
