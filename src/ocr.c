#include "ocr.h"
#include "pixbuf.h"
#include <tesseract/capi.h>
char *ocr_extract_text(struct swappy_state *s){
 GdkPixbuf *p=pixbuf_get_from_state(s);if(!p)return NULL;
 TessBaseAPI *api=TessBaseAPICreate();if(!api){g_object_unref(p);return NULL;}
 char *result=NULL;
 if(TessBaseAPIInit3(api,NULL,"eng")==0){
  TessBaseAPISetPageSegMode(api,PSM_SPARSE_TEXT);
  TessBaseAPISetImage(api,gdk_pixbuf_get_pixels(p),gdk_pixbuf_get_width(p),gdk_pixbuf_get_height(p),gdk_pixbuf_get_n_channels(p),gdk_pixbuf_get_rowstride(p));
  TessBaseAPISetSourceResolution(api,96);
  if(TessBaseAPIRecognize(api,NULL)==0){char *t=TessBaseAPIGetUTF8Text(api);if(t){result=g_strdup(t);TessDeleteText(t);}}
 }
 TessBaseAPIEnd(api);TessBaseAPIDelete(api);g_object_unref(p);return result;
}
