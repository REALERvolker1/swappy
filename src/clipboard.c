#include "clipboard.h"
#include "pixbuf.h"
#include <gio/gio.h>
static gboolean send_png(const gchar *data,gsize size,gboolean wayland){
 GError *e=NULL;GSubprocess *p=wayland?
 g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE,&e,"wl-copy","-t","image/png",NULL):
 g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE,&e,"xclip","-selection","clipboard","-t","image/png",NULL);
 if(!p){g_clear_error(&e);return FALSE;}
 GBytes *b=g_bytes_new(data,size);
 gboolean ok=g_subprocess_communicate(p,b,NULL,NULL,NULL,&e)&&g_subprocess_get_successful(p);
 g_bytes_unref(b);g_object_unref(p);g_clear_error(&e);return ok;
}
bool clipboard_copy_drawing_area_to_selection(struct swappy_state *s){
 GdkPixbuf *p=pixbuf_get_from_state(s);if(!p)return false;
 gchar *data=NULL;gsize size=0;GError *e=NULL;
 gboolean ok=gdk_pixbuf_save_to_buffer(p,&data,&size,"png",&e,NULL);
 if(ok)ok=send_png(data,size,TRUE)||send_png(data,size,FALSE);
 g_free(data);g_object_unref(p);g_clear_error(&e);
 if(ok&&s->config->early_exit)Rquit(0);
 return ok;
}
