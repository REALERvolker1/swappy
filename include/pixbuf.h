#pragma once
#include "swappy.h"
GdkPixbuf *pixbuf_init_from_file(struct swappy_state *state);
GdkPixbuf *pixbuf_get_from_state(struct swappy_state *state);
gboolean pixbuf_crop_to_selection(struct swappy_state *state);
void pixbuf_save_state_to_folder(GdkPixbuf *, char *, char *);
void pixbuf_save_to_file(GdkPixbuf *, char *);
void pixbuf_save_to_stdout(GdkPixbuf *);
void pixbuf_create_surfaces(struct swappy_state *state);
void pixbuf_upload_texture(struct swappy_state *state);
void pixbuf_free(struct swappy_state *state);
