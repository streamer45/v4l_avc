#include <va/va.h>
#include <va/va_enc_h264.h>
#include "va_display.h"

typedef struct {
  VADisplay dpy;
  FILE *fp;
  unsigned char *rawframe;
  uint fn;
  uint enc_fn;
} va_context;

va_context *va_context_init(unsigned char *buffer, char *output,
  int w, int h, int qp);
size_t va_context_free(va_context *ctx);
int encode_frame_h264(va_context *ctx);
