#include "pixbuf.h"
#include "paint.h"
#include "selection.h"
#include <gio/gunixoutputstream.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static GdkPixbuf *to_pixbuf(cairo_surface_t *surface,int x,int y,int w,int h){
 if(!surface||w<=0||h<=0)return NULL;
 cairo_surface_flush(surface);
 GdkPixbuf *pix=gdk_pixbuf_new(GDK_COLORSPACE_RGB,TRUE,8,w,h);
 const unsigned char *src=cairo_image_surface_get_data(surface);
 unsigned char *dst=gdk_pixbuf_get_pixels(pix);
 int ss=cairo_image_surface_get_stride(surface),ds=gdk_pixbuf_get_rowstride(pix);
 for(int j=0;j<h;j++)for(int i=0;i<w;i++){
  uint32_t v=((const uint32_t*)(src+(j+y)*ss))[i+x];
  unsigned a=(v>>24)&255;
  unsigned char *p=dst+j*ds+i*4;
  for(int c=0;c<3;c++){unsigned z=(v>>(16-c*8))&255;p[c]=a?MIN(255,(z*255+a/2)/a):0;}
  p[3]=a;
 }
 return pix;
}
GdkPixbuf *pixbuf_get_from_state(struct swappy_state *s){
 struct swappy_box b;
 if(!selection_get_box(s,&b))b=(struct swappy_box){0,0,cairo_image_surface_get_width(s->rendering_surface),cairo_image_surface_get_height(s->rendering_surface)};
 return to_pixbuf(s->rendering_surface,b.x,b.y,b.width,b.height);
}
void pixbuf_create_surfaces(struct swappy_state *s){
 if(s->original_image_surface)cairo_surface_destroy(s->original_image_surface);
 if(s->rendering_surface)cairo_surface_destroy(s->rendering_surface);
 int w=gdk_pixbuf_get_width(s->original_image),h=gdk_pixbuf_get_height(s->original_image),n=gdk_pixbuf_get_n_channels(s->original_image);
 s->original_image_surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
 s->rendering_surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
 unsigned char *dst=cairo_image_surface_get_data(s->original_image_surface);
 const unsigned char *src=gdk_pixbuf_read_pixels(s->original_image);
 int ss=gdk_pixbuf_get_rowstride(s->original_image),ds=cairo_image_surface_get_stride(s->original_image_surface);
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){
  const unsigned char *p=src+y*ss+x*n;unsigned a=n==4?p[3]:255;
  ((uint32_t*)(dst+y*ds))[x]=(a<<24)|(((p[0]*a+127)/255)<<16)|(((p[1]*a+127)/255)<<8)|((p[2]*a+127)/255);
 }
 cairo_surface_mark_dirty(s->original_image_surface);
}
void pixbuf_upload_texture(struct swappy_state *s){
 int w=cairo_image_surface_get_width(s->rendering_surface),h=cairo_image_surface_get_height(s->rendering_surface);
 GdkPixbuf *p=to_pixbuf(s->rendering_surface,0,0,w,h);if(!p)return;
 unsigned char *rgba=g_malloc((size_t)w*h*4);
 for(int j=0;j<h;j++)memcpy(rgba+(size_t)j*w*4,gdk_pixbuf_get_pixels(p)+(size_t)j*gdk_pixbuf_get_rowstride(p),(size_t)w*4);
 if(Rtexture_is_valid(s->image_texture))Rtexture_update(s->image_texture,rgba,w,h);
 else s->image_texture=Rtexture_create_from_pixels(rgba,w,h);
 g_free(rgba);g_object_unref(p);
 if(s->ui&&Rwindow_is_valid(s->ui->window))Rwindow_request_redraw(s->ui->window);
}
gboolean pixbuf_crop_to_selection(struct swappy_state *s){
 struct swappy_box b;if(!selection_get_box(s,&b))return FALSE;
 GdkPixbuf *p=pixbuf_get_from_state(s);if(!p)return FALSE;
 paint_free_all(s);g_object_unref(s->original_image);s->original_image=p;
 s->selection.box=(struct swappy_box){0,0,b.width,b.height};s->selection.previous_box=s->selection.box;
 pixbuf_create_surfaces(s);return TRUE;
}
static void write_image(GdkPixbuf *p,const char *path){GError *e=NULL;if(!gdk_pixbuf_save(p,path,"png",&e,NULL)){g_warning("Save failed: %s",e?e->message:"unknown");g_clear_error(&e);}}
void pixbuf_save_to_stdout(GdkPixbuf *p){GError *e=NULL;GOutputStream *out=g_unix_output_stream_new(STDOUT_FILENO,FALSE);gdk_pixbuf_save_to_stream(p,out,"png",NULL,&e,NULL);g_clear_error(&e);g_object_unref(out);}
void pixbuf_save_to_file(GdkPixbuf *p,char *path){if(!strcmp(path,"-"))pixbuf_save_to_stdout(p);else write_image(p,path);}
void pixbuf_save_state_to_folder(GdkPixbuf *p,char *folder,char *format){
 if(!p||!folder||!format)return;
 char name[256];time_t now=time(NULL);
 if(!strftime(name,sizeof(name),format,localtime(&now)))return;
 char *path=g_build_filename(folder,name,NULL);write_image(p,path);g_free(path);
}
GdkPixbuf *pixbuf_init_from_file(struct swappy_state *s){
 GError *e=NULL;s->original_image=gdk_pixbuf_new_from_file(s->temp_file_str?s->temp_file_str:s->file_str,&e);
 if(!s->original_image){g_warning("Open failed: %s",e?e->message:"unknown");g_clear_error(&e);}
 return s->original_image;
}
void pixbuf_free(struct swappy_state *s){if(s->original_image)g_object_unref(s->original_image);}
