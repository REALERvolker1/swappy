#include "ocr.h"

#include <math.h>
#include <pango/pango.h>
#include <tesseract/capi.h>

#include "pixbuf.h"

#define OCR_TEXT_PADDING 2

static void show_ocr_error(struct swappy_state *state, const char *message) {
  GtkWidget *dialog = gtk_message_dialog_new(
      state->ui->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

void ocr_clear_overlay(struct swappy_state *state) {
  if (!state->ui || !state->ui->ocr_overlay) {
    return;
  }

  GList *children =
      gtk_container_get_children(GTK_CONTAINER(state->ui->ocr_overlay));
  for (GList *elem = children; elem; elem = elem->next) {
    gtk_widget_destroy(GTK_WIDGET(elem->data));
  }
  g_list_free(children);
  gtk_widget_hide(state->ui->ocr_overlay);
}

static gchar *copy_stripped_text(const char *text) {
  gchar *copy = g_strdup(text ? text : "");
  g_strstrip(copy);
  return copy;
}

static gboolean ocr_text_key_press_handler(GtkWidget *widget,
                                           GdkEventKey *event,
                                           gpointer user_data) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
  GtkTextIter start;
  GtkTextIter end;

  if (!(event->state & GDK_CONTROL_MASK)) {
    return FALSE;
  }

  switch (event->keyval) {
    case GDK_KEY_a:
    case GDK_KEY_A:
      gtk_text_buffer_get_bounds(buffer, &start, &end);
      gtk_text_buffer_select_range(buffer, &start, &end);
      return TRUE;
    case GDK_KEY_c:
    case GDK_KEY_C:
      if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, text, -1);
        g_free(text);
        return TRUE;
      }
      return FALSE;
    default:
      return FALSE;
  }
}

static void measure_text(GtkWidget *widget, const gchar *text, gint font_pixels,
                         gint *width, gint *height) {
  PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
  PangoFontDescription *font = pango_font_description_new();

  pango_font_description_set_family(font, "Sans");
  pango_font_description_set_absolute_size(font, font_pixels * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_single_paragraph_mode(layout, TRUE);
  pango_layout_get_pixel_size(layout, width, height);

  pango_font_description_free(font);
  g_object_unref(layout);
}

static GtkWidget *create_text_view(const gchar *text, gint font_pixels) {
  GtkWidget *view = gtk_text_view_new();
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkTextIter start;
  GtkTextIter end;
  PangoFontDescription *font = pango_font_description_new();

  gtk_text_buffer_set_text(buffer, text, -1);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  pango_font_description_set_family(font, "Sans");
  pango_font_description_set_absolute_size(
      font, CLAMP(font_pixels, 6, 96) * PANGO_SCALE);
  GtkTextTag *tag =
      gtk_text_buffer_create_tag(buffer, NULL, "font-desc", font, NULL);
  gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
  pango_font_description_free(font);

  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 0);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 0);
  gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(view), 0);
  gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(view), 0);
  gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(view), 0);
  gtk_widget_set_can_focus(view, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(view), "ocr-text");
  g_signal_connect(G_OBJECT(view), "key-press-event",
                   G_CALLBACK(ocr_text_key_press_handler), NULL);

  return view;
}

static void add_selectable_text(struct swappy_state *state, const gchar *text,
                                gint left, gint top, gint right, gint bottom,
                                gint font_pixels, gdouble scale_x,
                                gdouble scale_y) {
  gint box_width = MAX(1, (gint)ceil((right - left) * scale_x));
  gint box_height = MAX(1, (gint)ceil((bottom - top) * scale_y));
  gint text_width = 0;
  gint text_height = 0;
  GtkWidget *view;

  measure_text(state->ui->area, text, font_pixels, &text_width, &text_height);
  while ((text_width > box_width || text_height > box_height) &&
         font_pixels > 6) {
    font_pixels--;
    measure_text(state->ui->area, text, font_pixels, &text_width, &text_height);
  }

  gint x = MAX(0, (gint)floor(left * scale_x));
  gint y =
      MAX(0, (gint)floor(top * scale_y + (box_height - text_height) / 2.0));
  gint width = text_width + OCR_TEXT_PADDING;
  gint height = text_height + OCR_TEXT_PADDING;
  view = create_text_view(text, font_pixels);

  gtk_widget_set_size_request(view, width, height);
  gtk_fixed_put(GTK_FIXED(state->ui->ocr_overlay), view, x, y);
  gtk_widget_show(view);
}

static void append_word_to_line(gchar **line_text, const gchar *word) {
  if (!*line_text) {
    *line_text = g_strdup(word);
    return;
  }

  gchar *old_line_text = *line_text;
  *line_text = g_strconcat(old_line_text, " ", word, NULL);
  g_free(old_line_text);
}

static guint add_selectable_lines(struct swappy_state *state,
                                  TessResultIterator *iterator,
                                  gint image_width, gint image_height,
                                  gdouble scale_x, gdouble scale_y) {
  guint lines = 0;
  gchar *line_text = NULL;
  gint line_left = image_width;
  gint line_top = image_height;
  gint line_right = 0;
  gint line_bottom = 0;
  gint line_font_pixels = 6;

  do {
    const TessPageIterator *page_iterator =
        TessResultIteratorGetPageIteratorConst(iterator);
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    char *ocr_text = TessResultIteratorGetUTF8Text(iterator, RIL_WORD);

    if (!ocr_text) {
      continue;
    }

    gchar *text = copy_stripped_text(ocr_text);
    TessDeleteText(ocr_text);

    if (text[0] == '\0') {
      g_free(text);
      continue;
    }

    if (TessPageIteratorBoundingBox(page_iterator, RIL_WORD, &left, &top,
                                    &right, &bottom)) {
      left = CLAMP(left, 0, image_width);
      right = CLAMP(right, left, image_width);
      top = CLAMP(top, 0, image_height);
      bottom = CLAMP(bottom, top, image_height);

      append_word_to_line(&line_text, text);
      line_left = MIN(line_left, left);
      line_top = MIN(line_top, top);
      line_right = MAX(line_right, right);
      line_bottom = MAX(line_bottom, bottom);
      line_font_pixels =
          MAX(line_font_pixels, (gint)ceil((bottom - top) * scale_y));
    }

    if (line_text && TessPageIteratorIsAtFinalElement(page_iterator,
                                                      RIL_TEXTLINE, RIL_WORD)) {
      add_selectable_text(state, line_text, line_left, line_top, line_right,
                          line_bottom, line_font_pixels, scale_x, scale_y);
      g_free(line_text);
      line_text = NULL;
      line_left = image_width;
      line_top = image_height;
      line_right = 0;
      line_bottom = 0;
      line_font_pixels = 6;
      lines++;
    }

    g_free(text);
  } while (TessResultIteratorNext(iterator, RIL_WORD));

  if (line_text) {
    add_selectable_text(state, line_text, line_left, line_top, line_right,
                        line_bottom, line_font_pixels, scale_x, scale_y);
    g_free(line_text);
    lines++;
  }

  return lines;
}

gboolean ocr_make_text_selectable(struct swappy_state *state) {
  GtkAllocation alloc;
  GdkPixbuf *pixbuf = NULL;
  TessBaseAPI *api = NULL;
  TessResultIterator *iterator = NULL;

  if (!state->rendering_surface || !state->ui || !state->ui->ocr_overlay) {
    return FALSE;
  }

  ocr_clear_overlay(state);

  pixbuf = pixbuf_get_from_state(state);
  if (!pixbuf) {
    show_ocr_error(state, "Unable to prepare the image for OCR.");
    return FALSE;
  }

  api = TessBaseAPICreate();
  if (!api) {
    g_object_unref(pixbuf);
    show_ocr_error(state, "Unable to initialize Tesseract OCR.");
    return FALSE;
  }

  if (TessBaseAPIInit3(api, NULL, "eng") != 0) {
    TessBaseAPIDelete(api);
    g_object_unref(pixbuf);
    show_ocr_error(state, "Unable to load Tesseract language data for 'eng'.");
    return FALSE;
  }

  TessBaseAPISetPageSegMode(api, PSM_SPARSE_TEXT);
  TessBaseAPISetImage(
      api, gdk_pixbuf_get_pixels(pixbuf), gdk_pixbuf_get_width(pixbuf),
      gdk_pixbuf_get_height(pixbuf), gdk_pixbuf_get_n_channels(pixbuf),
      gdk_pixbuf_get_rowstride(pixbuf));
  TessBaseAPISetSourceResolution(api, 96);

  if (TessBaseAPIRecognize(api, NULL) != 0) {
    TessBaseAPIEnd(api);
    TessBaseAPIDelete(api);
    g_object_unref(pixbuf);
    show_ocr_error(state, "Tesseract OCR failed for this image.");
    return FALSE;
  }

  iterator = TessBaseAPIGetIterator(api);
  if (iterator) {
    gtk_widget_get_allocation(state->ui->area, &alloc);
    gint image_width = gdk_pixbuf_get_width(pixbuf);
    gint image_height = gdk_pixbuf_get_height(pixbuf);
    gdouble scale_x = (gdouble)alloc.width / image_width;
    gdouble scale_y = (gdouble)alloc.height / image_height;
    guint lines = add_selectable_lines(state, iterator, image_width,
                                       image_height, scale_x, scale_y);

    if (lines > 0) {
      gtk_widget_show(state->ui->ocr_overlay);
    }
  }

  TessBaseAPIEnd(api);
  TessBaseAPIDelete(api);
  g_object_unref(pixbuf);
  return TRUE;
}
