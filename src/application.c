#include "application.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>

#include "clipboard.h"
#include "config.h"
#include "file.h"
#include "ocr.h"
#include "paint.h"
#include "pixbuf.h"
#include "render.h"
#include "selection.h"

#define PANEL_WIDTH 950
#define PANEL_HEIGHT 164
#define PANEL_MARGIN 16

static void update_layout(struct swappy_state *s, int width, int height);
static void update_controls(struct swappy_state *s);

static int detect_dark_mode(void) {
  /* XDG desktop portal is desktop-neutral and works on KDE and GNOME.
   * The result of Read is a variant containing a uint32:
   * 0=no preference, 1=dark, 2=light. */
  GError *error = NULL;
  GDBusProxy *portal = g_dbus_proxy_new_for_bus_sync(
    G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
    NULL, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
    "org.freedesktop.portal.Settings", NULL, &error);
  if (portal) {
    GVariant *response = g_dbus_proxy_call_sync(portal, "Read",
      g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
      G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
    if (response) {
      GVariant *inner = NULL;
      g_variant_get(response, "(v)", &inner);
      while (g_variant_is_of_type(inner, G_VARIANT_TYPE_VARIANT)) {
        GVariant *unwrapped = g_variant_get_variant(inner);
        g_variant_unref(inner);
        inner = unwrapped;
      }
      if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32)) {
        guint scheme = g_variant_get_uint32(inner);
        g_variant_unref(inner);
        g_variant_unref(response);
        g_object_unref(portal);
        g_clear_error(&error);
        if (scheme == 1) return 1;
        if (scheme == 2) return 0;
      } else {
        g_variant_unref(inner);
        g_variant_unref(response);
      }
    }
    g_object_unref(portal);
  }
  g_clear_error(&error);

  /* Only used when the portal has no explicit preference. Don't depend
   * on gsettings CLI or GNOME schemas existing on other desktops. */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(
      source, "org.gnome.desktop.interface", TRUE);
    if (schema) {
      if (g_settings_schema_has_key(schema, "color-scheme")) {
        GSettings *settings = g_settings_new_full(schema, NULL, NULL);
        char *choice = g_settings_get_string(settings, "color-scheme");
        int dark = g_strcmp0(choice, "prefer-dark") == 0;
        g_free(choice);
        g_object_unref(settings);
        g_settings_schema_unref(schema);
        return dark;
      }
      g_settings_schema_unref(schema);
    }
  }
  return 0;
}

static void sync_paint(struct swappy_state *s) {
  render_state(s);
  update_controls(s);
}

static void commit_drawing(struct swappy_state *s) {
  if (!s->temp_paint) return;
  paint_commit_temporary(s);
  paint_free_list(&s->redo_paints);
  sync_paint(s);
}

static void undo(struct swappy_state *s) {
  if (!s->paints) return;
  GList *link = s->paints;
  struct swappy_paint *item = link->data;
  s->paints = g_list_delete_link(s->paints, link);
  s->redo_paints = g_list_prepend(s->redo_paints, item);
  sync_paint(s);
}

static void redo(struct swappy_state *s) {
  if (!s->redo_paints) return;
  GList *link = s->redo_paints;
  struct swappy_paint *item = link->data;
  s->redo_paints = g_list_delete_link(s->redo_paints, link);
  s->paints = g_list_prepend(s->paints, item);
  sync_paint(s);
}

static void save_to_path(struct swappy_state *s, const char *path) {
  if (!path) return;
  commit_drawing(s);
  GdkPixbuf *image = pixbuf_get_from_state(s);
  if (!image) return;
  pixbuf_save_to_file(image, (char *)path);
  g_object_unref(image);
  if (s->config->early_exit) Rquit(0);
}

static void save_default(struct swappy_state *s) {
  commit_drawing(s);
  GdkPixbuf *image = pixbuf_get_from_state(s);
  if (!image) return;
  pixbuf_save_state_to_folder(image, s->config->save_dir, s->config->save_filename_format);
  g_object_unref(image);
  if (s->config->early_exit) Rquit(0);
}

static void save_as(struct swappy_state *s) {
  RFileDialogDesc desc = {0};
  desc.parent = s->ui->window;
  desc.title = "Save screenshot as PNG";
  desc.directory = s->config->save_dir;
  desc.file_name = "swappy.png";
  desc.default_extension = "png";
  RFileFilter filter = {"PNG image", "*.png"};
  desc.filters = &filter;
  desc.filter_count = 1;
  const char *path = Rfile_dialog_save(&desc);
  if (path) save_to_path(s, path);
}

static void run_ocr(struct swappy_state *s) {
  commit_drawing(s);
  char *text = ocr_extract_text(s);
  if (text && *text) {
    Rclipboard_set_text(text);
    Rmessage_box(s->ui->window, "OCR", "Recognized text copied to clipboard.", R_MESSAGE_OK);
  } else {
    Rmessage_box(s->ui->window, "OCR", "No text found, or Tesseract data unavailable.", R_MESSAGE_OK);
  }
  g_free(text);
}

static void mode_clicked(Rid widget, void *ctx) {
  struct swappy_state *s = ctx;
  commit_drawing(s);
  s->mode = (enum swappy_paint_type)(uintptr_t)Rwidget_get_user_handle(widget);
  update_controls(s);
  Rwidget_focus(s->ui->canvas);
}

static void action_clicked(Rid widget, void *ctx) {
  struct swappy_state *s = ctx;
  const char *action = (const char *)Rwidget_get_user_handle(widget);
  if (!action) return;
  if (!strcmp(action, "undo")) undo(s);
  else if (!strcmp(action, "redo")) redo(s);
  else if (!strcmp(action, "copy")) {
    commit_drawing(s);
    if (!clipboard_copy_drawing_area_to_selection(s))
      Rmessage_box(s->ui->window, "Clipboard", "Install wl-clipboard or xclip.", R_MESSAGE_OK);
  }
  else if (!strcmp(action, "save")) save_default(s);
  else if (!strcmp(action, "saveas")) save_as(s);
  else if (!strcmp(action, "ocr")) run_ocr(s);
  else if (!strcmp(action, "crop")) {
    if (pixbuf_crop_to_selection(s)) {
      s->selection.enabled = false;
      s->selection.box = (struct swappy_box){0};
      int w, h;
      Rwindow_get_size(s->ui->window, &w, &h);
      update_layout(s, w, h);
      sync_paint(s);
    }
  }
  else if (!strcmp(action, "clear")) {
    struct swappy_box box;
    if (selection_get_box(s, &box)) {
      commit_drawing(s);
      struct swappy_paint *item = g_new0(struct swappy_paint, 1);
      item->type = SWAPPY_PAINT_MODE_CLEAR;
      item->can_draw = true;
      item->is_committed = true;
      item->has_clip = true;
      item->clip = box;
      s->paints = g_list_prepend(s->paints, item);
    } else {
      paint_free_all(s);
    }
    sync_paint(s);
  }
  else if (!strcmp(action, "color")) {
    Rid picker = Rcolor_picker_create();
    Rcolor_picker_set_color(picker, s->settings.r, s->settings.g, s->settings.b, 1);
    /* The modal widget's color is read after it closes. */
    if (Rdialog_run(s->ui->window, picker)) {
      /* Raven destroys dialog contents on close: colors are therefore
       * captured by a changed callback, not read from the dead handle. */
    }
  }
  Rwidget_focus(s->ui->canvas);
}

static void color_changed(Rid picker, float r, float g, float b, float a, void *ctx) {
  (void)picker;
  struct swappy_state *s = ctx;
  s->settings.r = r; s->settings.g = g; s->settings.b = b;
  s->settings.a = a;
}

static void color_button(Rid button, void *ctx) {
  struct swappy_state *s = ctx;
  Rid picker = Rcolor_picker_create();
  Rcolor_picker_set_color(picker, s->settings.r, s->settings.g, s->settings.b, 1.0f);
  Rcolor_picker_set_changed_callback(picker, color_changed, s);
  (void)Rdialog_run(s->ui->window, picker);
  Rwidget_focus(s->ui->canvas);
}

static void selection_changed(Rid widget, int checked, void *ctx) {
  struct swappy_state *s = ctx;
  commit_drawing(s);
  s->selection.enabled = checked;
  s->selection.dragging = false;
  if (checked) s->selection.box = (struct swappy_box){0};
  Rwidget_focus(s->ui->canvas);
  update_controls(s);
  Rwidget_invalidate(s->ui->canvas);
}

static void fill_changed(Rid widget, int checked, void *ctx) {
  (void)widget;
  struct swappy_state *s = ctx;
  s->config->fill_shape = checked;
  Rwidget_focus(s->ui->canvas);
}

static void alpha_changed(Rid widget, int checked, void *ctx) {
  (void)widget;
  struct swappy_state *s = ctx;
  s->config->transparent = checked;
  Rwidget_focus(s->ui->canvas);
}

static void size_changed(Rid widget, double value, void *ctx) {
  struct swappy_state *s = ctx;
  if (widget == s->ui->size_input) s->settings.w = value;
  if (widget == s->ui->text_input) s->settings.t = value;
  if (widget == s->ui->opacity_input) s->settings.tr = (int)value;
}

static Rid button(Rid row, const char *name, const char *action, struct swappy_state *s) {
  Rid item = Rwidget_add_child(row, Rbutton_create(name));
  Rbutton_set_style(item, R_BUTTON_FLAT);
  Rwidget_set_user_handle(item, (void *)action);
  Rbutton_set_clicked_callback(item, action_clicked, s);
  return item;
}

static Rid mode_button(Rid row, const char *name,
                       enum swappy_paint_type mode, struct swappy_state *s) {
  Rid item = Rwidget_add_child(row, Rbutton_create(name));
  Rbutton_set_style(item, R_BUTTON_FLAT);
  Rwidget_set_user_handle(item, (void *)(uintptr_t)mode);
  Rbutton_set_clicked_callback(item, mode_clicked, s);
  s->ui->mode_buttons[(int)mode] = item;
  return item;
}

static Rid make_row(Rid parent, float spacing) {
  Rid row = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_HBOX));
  Rpanel_set_spacing(row, spacing);
  return row;
}

static void update_controls(struct swappy_state *s) {
  if (!s->ui) return;
  for (int i = 0; i < 6; ++i) {
    Rid button_id = s->ui->mode_buttons[i];
    if (!Rwidget_is_valid(button_id)) continue;
    Rbutton_set_style(button_id, s->mode == i ? R_BUTTON_PRIMARY : R_BUTTON_FLAT);
  }
  if (Rwidget_is_valid(s->ui->undo_button))
    Rwidget_set_enabled(s->ui->undo_button, s->paints != NULL);
  if (Rwidget_is_valid(s->ui->redo_button))
    Rwidget_set_enabled(s->ui->redo_button, s->redo_paints != NULL);
  struct swappy_box box;
  if (Rwidget_is_valid(s->ui->crop_button))
    Rwidget_set_enabled(s->ui->crop_button, selection_get_box(s, &box));
}

static void build_panel(struct swappy_state *s, Rid root) {
  Rid panel = Rwidget_add_child(root, Rwidget_create(R_WIDGET_BORDER));
  s->ui->toolbar = panel;
  Rborder_set_material(panel, Rtheme_is_dark() ? R_MATERIAL_TINTED_GLASS : R_MATERIAL_FROSTED_GLASS);
  Rborder_set_corner_radius(panel, 25, 25, 25, 25);
  Rborder_set_padding(panel, 13, 10, 13, 10);
  Rborder_set_shadow(panel, 18, 0, 6);
  Rslot_set_size(panel, PANEL_WIDTH, PANEL_HEIGHT);

  Rid stack = Rwidget_add_child(panel, Rwidget_create(R_WIDGET_VBOX));
  Rpanel_set_spacing(stack, 4);
  Rid first = make_row(stack, 5);
  mode_button(first, "Brush", SWAPPY_PAINT_MODE_BRUSH, s);
  mode_button(first, "Text", SWAPPY_PAINT_MODE_TEXT, s);
  mode_button(first, "Rect", SWAPPY_PAINT_MODE_RECTANGLE, s);
  mode_button(first, "Ellipse", SWAPPY_PAINT_MODE_ELLIPSE, s);
  mode_button(first, "Arrow", SWAPPY_PAINT_MODE_ARROW, s);
  mode_button(first, "Blur", SWAPPY_PAINT_MODE_BLUR, s);
  button(first, "Clear", "clear", s);
  s->ui->undo_button = button(first, "Undo", "undo", s);
  s->ui->redo_button = button(first, "Redo", "redo", s);

  Rid second = make_row(stack, 7);
  Rid color = Rwidget_add_child(second, Rbutton_create("Color"));
  Rbutton_set_clicked_callback(color, color_button, s);
  Rwidget_add_child(second, Rlabel_create("Stroke"));
  s->ui->size_input = Rwidget_add_child(second, Rwidget_create(R_WIDGET_SPIN_BOX));
  Rspin_box_set_range(s->ui->size_input, SWAPPY_LINE_SIZE_MIN, SWAPPY_LINE_SIZE_MAX);
  Rspin_box_set_value(s->ui->size_input, s->settings.w);
  Rspin_box_set_changed_callback(s->ui->size_input, size_changed, s);
  Rwidget_add_child(second, Rlabel_create("Text"));
  s->ui->text_input = Rwidget_add_child(second, Rwidget_create(R_WIDGET_SPIN_BOX));
  Rspin_box_set_range(s->ui->text_input, SWAPPY_TEXT_SIZE_MIN, SWAPPY_TEXT_SIZE_MAX);
  Rspin_box_set_value(s->ui->text_input, s->settings.t);
  Rspin_box_set_changed_callback(s->ui->text_input, size_changed, s);
  s->ui->fill_button = Rwidget_add_child(second, Rcheckbox_create("Fill"));
  Rcheckbox_set_checked(s->ui->fill_button, s->config->fill_shape);
  Rcheckbox_set_changed_callback(s->ui->fill_button, fill_changed, s);
  s->ui->transparent_button = Rwidget_add_child(second, Rcheckbox_create("Alpha"));
  Rcheckbox_set_checked(s->ui->transparent_button, s->config->transparent);
  Rcheckbox_set_changed_callback(s->ui->transparent_button, alpha_changed, s);
  s->ui->opacity_input = Rwidget_add_child(second, Rwidget_create(R_WIDGET_SPIN_BOX));
  Rspin_box_set_range(s->ui->opacity_input, SWAPPY_TRANSPARENCY_MIN, SWAPPY_TRANSPARENCY_MAX);
  Rspin_box_set_value(s->ui->opacity_input, s->settings.tr);
  Rspin_box_set_changed_callback(s->ui->opacity_input, size_changed, s);

  Rid third = make_row(stack, 6);
  s->ui->selection_button = Rwidget_add_child(third, Rcheckbox_create("Select"));
  Rcheckbox_set_checked(s->ui->selection_button, s->selection.enabled);
  Rcheckbox_set_changed_callback(s->ui->selection_button, selection_changed, s);
  s->ui->crop_button = button(third, "Crop", "crop", s);
  button(third, "Copy", "copy", s);
  button(third, "Save", "save", s);
  button(third, "Save as…", "saveas", s);
  button(third, "OCR → Clipboard", "ocr", s);
}

static void update_layout(struct swappy_state *s, int width, int height) {
  struct swappy_state_ui *ui = s->ui;
  if (!ui) return;
  ui->window_width = width;
  ui->window_height = height;
  int image_w = gdk_pixbuf_get_width(s->original_image);
  int image_h = gdk_pixbuf_get_height(s->original_image);
  float sx = (width - 24.0f) / image_w;
  float sy = (height - 24.0f) / image_h;
  float scale = fminf(sx, sy);
  if (scale <= 0) scale = 1;
  ui->image_width = image_w * scale;
  ui->image_height = image_h * scale;
  ui->image_x = (width - ui->image_width) * 0.5f;
  ui->image_y = (height - ui->image_height) * 0.5f;
  s->scaling_factor = scale;
  float left = fmaxf(8, (width - PANEL_WIDTH) * 0.5f);
  float bottom = ui->image_y + ui->image_height - PANEL_HEIGHT - PANEL_MARGIN;
  float top = fmaxf(8, fminf(height - PANEL_HEIGHT - 8, bottom));
  Rslot_set_position(ui->toolbar, left, top);
  Rslot_set_size(ui->canvas, width, height);
  Rslot_set_size(ui->toolbar, PANEL_WIDTH, PANEL_HEIGHT);
  Rwidget_invalidate(ui->canvas);
}

static void on_resize(Rid window, int width, int height, void *ctx) {
  (void)window;
  update_layout(ctx, width, height);
}

static void paint_canvas(Rid widget, RPainter *p, float width, float height, void *ctx) {
  (void)widget; (void)width; (void)height;
  struct swappy_state *s = ctx;
  struct swappy_state_ui *ui = s->ui;
  if (Rtexture_is_valid(s->image_texture))
    Rdraw_image(p, s->image_texture, ui->image_x, ui->image_y,
                ui->image_width, ui->image_height, 1, 1, 1, 1);
  struct swappy_box selected;
  if (selection_get_box(s, &selected)) {
    float x = ui->image_x + selected.x * s->scaling_factor;
    float y = ui->image_y + selected.y * s->scaling_factor;
    float w = selected.width * s->scaling_factor;
    float h = selected.height * s->scaling_factor;
    Rdraw_rect_outline(p, x, y, w, h, 0, 2, 1, 1, 1, 0.95f);
    Rdraw_rect_outline(p, x+2, y+2, fmaxf(w-4, 0), fmaxf(h-4, 0),
                       0, 1, 0.0f, 0.3f, 0.6f, 0.85f);
    for (int k = 0; k < 4; k++) {
      float cx = x + ((k&1) ? w : 0), cy = y + ((k&2) ? h : 0);
      Rdraw_rect(p, cx - 4, cy - 4, 8, 8, 2, 1, 1, 1, 1);
    }
  }
}

static int in_image(struct swappy_state *s, const REvent *ev,
                    float *x, float *y) {
  struct swappy_state_ui *ui = s->ui;
  *x = (ev->local_x - ui->image_x) / s->scaling_factor;
  *y = (ev->local_y - ui->image_y) / s->scaling_factor;
  return *x >= 0 && *y >= 0 &&
    *x < gdk_pixbuf_get_width(s->original_image) &&
    *y < gdk_pixbuf_get_height(s->original_image);
}

static int on_canvas_event(Rid widget, const REvent *ev, void *ctx) {
  struct swappy_state *s = ctx;
  struct swappy_state_ui *ui = s->ui;
  float x = 0, y = 0;
  int inside = in_image(s, ev, &x, &y);
  if (ev->type == R_EVENT_MOUSE_DOWN && ev->button == R_MOUSE_LEFT && inside) {
    Rwidget_focus(widget);
    ui->dragging = true;
    if (s->selection.enabled) {
      ui->selecting = selection_begin_drag(s, x, y, 6/s->scaling_factor, 6/s->scaling_factor);
    } else {
      commit_drawing(s);
      paint_add_temporary(s, x, y, s->mode);
      sync_paint(s);
    }
    return R_TRUE;
  }
  if (ev->type == R_EVENT_MOUSE_MOVE && ui->dragging) {
    x = CLAMP(x, 0, gdk_pixbuf_get_width(s->original_image));
    y = CLAMP(y, 0, gdk_pixbuf_get_height(s->original_image));
    if (s->selection.enabled && ui->selecting) {
      selection_update_drag(s, x, y);
      Rwidget_invalidate(widget);
      update_controls(s);
    } else if (s->temp_paint) {
      paint_update_temporary_shape(s, x, y, (ev->modifiers & R_MOD_CTRL) != 0);
      sync_paint(s);
    }
    return R_TRUE;
  }
  if (ev->type == R_EVENT_MOUSE_UP && ev->button == R_MOUSE_LEFT && ui->dragging) {
    ui->dragging = false;
    if (s->selection.enabled) {
      if (ui->selecting) selection_finish_drag(s, x, y);
      ui->selecting = false;
      Rwidget_invalidate(widget);
      update_controls(s);
    } else if (s->temp_paint && s->mode == SWAPPY_PAINT_MODE_TEXT) {
      paint_update_temporary_text_clip(s, x, y);
      sync_paint(s);
    } else {
      commit_drawing(s);
    }
    return R_TRUE;
  }
  if (ev->type == R_EVENT_KEY_DOWN) {
    if ((ev->modifiers & R_MOD_CTRL) && ev->key == R_KEY_Z) {
      if (ev->modifiers & R_MOD_SHIFT) redo(s); else undo(s);
      return R_TRUE;
    }
    if ((ev->modifiers & R_MOD_CTRL) && ev->key == R_KEY_Y) { redo(s); return R_TRUE; }
    if ((ev->modifiers & R_MOD_CTRL) && ev->key == R_KEY_S) { save_default(s); return R_TRUE; }
    if ((ev->modifiers & R_MOD_CTRL) && ev->key == R_KEY_C && !s->temp_paint) {
      clipboard_copy_drawing_area_to_selection(s); return R_TRUE;
    }
    if (s->temp_paint && s->mode == SWAPPY_PAINT_MODE_TEXT) {
      paint_update_temporary_text(s, ev->key);
      sync_paint(s);
      return R_TRUE;
    }
    if (ev->key == R_KEY_ESCAPE) { Rwindow_close(s->ui->window); return R_TRUE; }
  }
  if (ev->type == R_EVENT_TEXT && s->temp_paint &&
      s->temp_paint->type == SWAPPY_PAINT_MODE_TEXT) {
    char bytes[7] = {0};
    gint n = g_unichar_to_utf8(ev->codepoint, bytes);
    if (n > 0) { bytes[n] = 0; paint_update_temporary_str(s, bytes); sync_paint(s); }
    return R_TRUE;
  }
  return R_FALSE;
}

bool application_init(struct swappy_state *s) {
  gboolean version = FALSE;
  GOptionEntry flags[] = {
    {"file", 'f', 0, G_OPTION_ARG_STRING, &s->file_str, "Input image", "FILE"},
    {"output-file", 'o', 0, G_OPTION_ARG_STRING, &s->output_file, "Save image on exit", "FILE"},
    {"version", 'v', 0, G_OPTION_ARG_NONE, &version, "Print version", NULL},
    {NULL}
  };
  GOptionContext *opt = g_option_context_new("- screenshot annotation with Raven");
  g_option_context_add_main_entries(opt, flags, NULL);
  GError *error = NULL;
  if (!g_option_context_parse(opt, &s->argc, &s->argv, &error)) {
    g_printerr("swappy: %s\n", error->message);
    g_clear_error(&error);
    g_option_context_free(opt);
    return false;
  }
  g_option_context_free(opt);
  if (version) { g_print("swappy %s\n", SWAPPY_VERSION); exit(0); }
  config_load(s);
  if (!s->file_str) {
    g_printerr("swappy: specify -f IMAGE or -f - for standard input\n");
    return false;
  }
  if (strcmp(s->file_str, "-") == 0) {
    s->temp_file_str = file_dump_stdin_into_a_temp_file();
    if (!s->temp_file_str) return false;
  }
  if (!pixbuf_init_from_file(s)) return false;
  s->ui = g_new0(struct swappy_state_ui, 1);
  s->settings.r = 1; s->settings.g = 0; s->settings.b = 0; s->settings.a = 1;
  s->settings.w = s->config->line_size;
  s->settings.t = s->config->text_size;
  s->settings.tr = s->config->transparency;
  s->mode = s->config->paint_mode;
  if (s->mode < SWAPPY_PAINT_MODE_BRUSH || s->mode > SWAPPY_PAINT_MODE_BLUR)
    s->mode = SWAPPY_PAINT_MODE_BRUSH;
  return true;
}

int application_run(struct swappy_state *s) {
  if (!Rinit(R_BACKEND_VULKAN)) {
    g_printerr("Raven Vulkan backend failed to initialize (requires X11/XWayland)\n");
    return EXIT_FAILURE;
  }
  Rtheme_apply(detect_dark_mode() ? R_THEME_GLASS_DARK : R_THEME_GLASS_LIGHT);

  int iw = gdk_pixbuf_get_width(s->original_image);
  int ih = gdk_pixbuf_get_height(s->original_image);
  /* Fit ordinary screenshots at native resolution. Enlarge the *window* for
   * small screenshots so toolbar space never consumes the screenshot's size.
   * The screenshot itself is scaled with aspect ratio preserved. */
  const int max_width = 1360, max_height = 820;
  float fit = fminf(1.0f, fminf((max_width - 24.0f)/iw, (max_height - 24.0f)/ih));
  int width = CLAMP((int)ceilf(iw * fit + 24), PANEL_WIDTH + 28, max_width);
  int height = CLAMP((int)ceilf(ih * fit + 24), PANEL_HEIGHT + 60, max_height);
  s->ui->window = Rwindow_create("swappy — Raven Liquid Glass", width, height, R_WINDOW_DEFAULT);
  if (!Rwindow_is_valid(s->ui->window)) { Rshutdown(); return EXIT_FAILURE; }
  Rwindow_set_min_size(s->ui->window, PANEL_WIDTH + 28, PANEL_HEIGHT + 60);
  Rwindow_center(s->ui->window);
  Rid root = Rwidget_create(R_WIDGET_CANVAS);
  s->ui->canvas = Rwidget_add_child(root, Rwidget_create(R_WIDGET_CUSTOM));
  Rcustom_set_paint_callback(s->ui->canvas, paint_canvas, s);
  Rwidget_set_event_callback(s->ui->canvas, on_canvas_event, s);
  Rwidget_set_cursor(s->ui->canvas, R_CURSOR_CROSSHAIR);
  Rwidget_set_focusable(s->ui->canvas, R_TRUE);
  Rslot_set_position(s->ui->canvas, 0, 0);
  Rslot_set_size(s->ui->canvas, width, height);
  build_panel(s, root);
  Rwindow_set_content(s->ui->window, root);
  Rwindow_set_resize_callback(s->ui->window, on_resize, s);
  Rwindow_set_clear_color(s->ui->window, Rtheme_is_dark() ? 0.07f : 0.90f,
    Rtheme_is_dark() ? 0.09f : 0.92f,
    Rtheme_is_dark() ? 0.13f : 0.96f, 1);
  update_layout(s, width, height);
  pixbuf_create_surfaces(s);
  render_state(s);
  update_controls(s);
  Rwindow_show(s->ui->window);
  Rwidget_focus(s->ui->canvas);
  while (Rwait_frame(-1.0)) { /* no polling or GTK event loop */ }
  int ret = Rget_exit_code();
  if (s->output_file && s->rendering_surface)
    save_to_path(s, s->output_file);
  Rshutdown();
  s->image_texture = R_INVALID_ID;
  return ret;
}

void application_finish(struct swappy_state *s) {
  paint_free_all(s);
  pixbuf_free(s);
  if (s->original_image_surface) cairo_surface_destroy(s->original_image_surface);
  if (s->rendering_surface) cairo_surface_destroy(s->rendering_surface);
  if (s->temp_file_str) {
    g_unlink(s->temp_file_str);
    g_free(s->temp_file_str);
  }
  g_free(s->file_str);
  g_free(s->output_file);
  g_free(s->save_file);
  g_free(s->ui);
  g_free(s->window);
  g_free(s->geometry);
  config_free(s);
}
