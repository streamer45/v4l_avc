all:
		gcc v4l_avc.c avc.c\
		-DLINUX \
		-DVA_DRIVERS_PATH=\"/usr/lib/dri\" \
		-DHAVE_VA_X11 \
		-I/usr/include/libdrm \
		-Iva/display \
		-Iva/ \
		va/va.c \
		va/va_fool.c \
		va/va_trace.c \
		va/va_tpi.c \
		va/va_compat.c \
		va/x11/va_x11.c \
		va/x11/dri2_util.c \
		va/x11/va_nvctrl.c \
		va/x11/va_fglrx.c \
		va/x11/va_dri2.c \
    va/x11/va_dricommon.c \
		va/display/va_display.c \
		va/display/va_display_x11.c \
		-g -o v4l_avc  \
		-lm -lpthread -ldl -lX11 -lXext -lXfixes -ldrm -lswscale
clean:
		rm v4l_avc
