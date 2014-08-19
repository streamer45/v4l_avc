#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>
#include <linux/videodev2.h>
#include <libswscale/swscale.h>
#include "avc.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
  void   *start;
  size_t  length;
};

typedef struct {
  uint64_t raw_sz;
  uint64_t coded_sz;
  uint64_t enc_time;
} enc_stats;

typedef struct {
  char dev_name[32];
  char output[256];
  int fd;
  struct buffer *buffers;
  uint n_buffers;
  int frame_count;
  int frame_number;
  int width;
  int height;
  int qp;
  int pix_fmt;
  va_context *va_ctx;
  unsigned char *rawframe;
  size_t frame_sz;
  enc_stats stats;
  struct SwsContext *sws;
} v4l_st;

static inline uint64_t ts_to_ms(struct timespec ts) { /* ty krad_timer :p */
  return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void errno_exit(const char *s) {
  fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
  exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg) {
  int r;
  do {
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);
  return r;
}

static void yuyv222_to_yuv420(void *yuv_frame,
 const void *yuyv_frame, v4l_st *v4l) {
  int ret;
  int strides[4];
  int dstrides[4];
  const uint8_t *src_px[4];
  uint8_t *dst_px[4];
  v4l->sws = sws_getCachedContext(v4l->sws, v4l->width, v4l->height,
   AV_PIX_FMT_YUYV422, v4l->width, v4l->height,
    AV_PIX_FMT_YUV420P, SWS_SINC, NULL, NULL, NULL);
  if ( v4l->sws == NULL) {
    fprintf(stderr, "error setting sws context\n");
    exit(-1);
  }

  strides[0] = v4l->width * 2;
  strides[1] = 0;
  strides[2] = 0;
  strides[3] = 0;

  dstrides[0] = v4l->width;
  dstrides[1] = v4l->width/2;
  dstrides[2] = v4l->width/2;
  dstrides[3] = 0;

  src_px[0] = yuyv_frame;
  src_px[1] = NULL;
  src_px[2] = NULL;
  src_px[3] = NULL;

  dst_px[0] = yuv_frame;
  dst_px[1] = yuv_frame + (v4l->width * v4l->height);
  dst_px[2] = dst_px[1] + (v4l->width >> 1) * (v4l->height >> 1);
  dst_px[3] = NULL;
  ret = sws_scale( v4l->sws, (const uint8_t * const*)src_px, strides, 0,
    v4l->height, dst_px, dstrides);
  if (ret != v4l->height) {
    fprintf(stderr, "Error converting raw frame\n");
    exit(-1);
  }
}

static void process_image(v4l_st *v4l, const void *p, int size) {
  int ret;
  struct timespec start;
  struct timespec finish;
  v4l->stats.raw_sz += size;
  if (v4l->pix_fmt != V4L2_PIX_FMT_YUV420) {
    if (v4l->pix_fmt == V4L2_PIX_FMT_YUYV) {
      yuyv222_to_yuv420(v4l->rawframe, p, v4l);
      v4l->va_ctx->rawframe = v4l->rawframe;
    } else {
      fprintf(stderr, "Unsupported raw frame pixel format!");
      exit(-1);
    }
  } else {
    v4l->va_ctx->rawframe = (unsigned char *)p;
  }
  clock_gettime(CLOCK_MONOTONIC, &start);
  v4l->va_ctx->fn = v4l->frame_number;
/*  FILE *yuv;
  yuv = fopen("test.yuv", "ab+");
  fwrite(v4l->rawframe, v4l->frame_sz, 1, yuv);
  fclose(yuv);*/
  ret = encode_frame_h264(v4l->va_ctx);
  if (ret != 0) {
    fprintf(stderr, "Error encoding frame\n");
  }
  clock_gettime(CLOCK_MONOTONIC, &finish);
  v4l->stats.enc_time += ts_to_ms(finish) - ts_to_ms(start);
  printf("\rencoded %d frames", v4l->frame_number + 1);
  v4l->frame_number++;
  fflush(stdout);
}

static int read_frame(v4l_st *v4l){
  struct v4l2_buffer buf;
  CLEAR(buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (-1 == xioctl(v4l->fd, VIDIOC_DQBUF, &buf)) {
    switch (errno) {
      case EAGAIN:
      return 0;
      case EIO:
      default:
        errno_exit("VIDIOC_DQBUF");
    }
  }
  assert(buf.index < v4l->n_buffers);
  process_image(v4l, v4l->buffers[buf.index].start, buf.bytesused);
  if (-1 == xioctl(v4l->fd, VIDIOC_QBUF, &buf))
    errno_exit("VIDIOC_QBUF");
  return 1;
}

static void mainloop(v4l_st *v4l) {
  unsigned int count;
  int ret;
  struct pollfd pfds[1];
  count = v4l->frame_count;
  pfds[0].fd = v4l->fd;
  pfds[0].events = POLLIN;
  while (count-- > 0) {
    for (;;) {
      ret = poll(pfds, 1, -1);
      if (ret < 0) {
        if (EINTR == errno)
          continue;
        errno_exit("poll");
      }
      if (pfds[0].revents & POLLIN) {
        if (read_frame(v4l))
          break;
      }
    }
  }
}

static void stop_capturing(v4l_st *v4l) {
  enum v4l2_buf_type type;
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(v4l->fd, VIDIOC_STREAMOFF, &type))
    errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(v4l_st *v4l) {
  unsigned int i;
  enum v4l2_buf_type type;
  for (i = 0; i < v4l->n_buffers; ++i) {
    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (-1 == xioctl(v4l->fd, VIDIOC_QBUF, &buf))
      errno_exit("VIDIOC_QBUF");
  }
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(v4l->fd, VIDIOC_STREAMON, &type))
    errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(v4l_st *v4l) {
  unsigned int i;
  for (i = 0; i < v4l->n_buffers; ++i)
    if (-1 == munmap(v4l->buffers[i].start, v4l->buffers[i].length))
      errno_exit("munmap");
    free(v4l->buffers);
}

static void init_mmap(v4l_st *v4l) {
  struct v4l2_requestbuffers req;
  CLEAR(req);
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (-1 == xioctl(v4l->fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s does not support "
       "memory mapping\n", v4l->dev_name);
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_REQBUFS");
    }
  }
  if (req.count < 2) {
    fprintf(stderr, "Insufficient buffer memory on %s\n",
     v4l->dev_name);
    exit(EXIT_FAILURE);
  }
  v4l->buffers = calloc(req.count, sizeof(*v4l->buffers));
  if (!v4l->buffers) {
    fprintf(stderr, "Out of memory\n");
    exit(EXIT_FAILURE);
  }
  for (v4l->n_buffers = 0; v4l->n_buffers < req.count; ++v4l->n_buffers) {
    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory      = V4L2_MEMORY_MMAP;
    buf.index       = v4l->n_buffers;
    if (-1 == xioctl(v4l->fd, VIDIOC_QUERYBUF, &buf))
      errno_exit("VIDIOC_QUERYBUF");
    v4l->buffers[v4l->n_buffers].length = buf.length;
    v4l->buffers[v4l->n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
     MAP_SHARED, v4l->fd, buf.m.offset);
    if (MAP_FAILED == v4l->buffers[v4l->n_buffers].start)
      errno_exit("mmap");
  }
}

static void init_device(v4l_st *v4l) {
  struct v4l2_capability cap;
  struct v4l2_cropcap cropcap;
  struct v4l2_crop crop;
  struct v4l2_format fmt;
  unsigned int min;
  if (-1 == xioctl(v4l->fd, VIDIOC_QUERYCAP, &cap)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s is no V4L2 device\n",
       v4l->dev_name);
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_QUERYCAP");
    }
  }
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is no video capture device\n",
     v4l->dev_name);
    exit(EXIT_FAILURE);
  }
  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "%s does not support streaming i/o\n",
     v4l->dev_name);
    exit(EXIT_FAILURE);
  }
  /* Select video input, video standard and tune here. */
  CLEAR(cropcap);
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (0 == xioctl(v4l->fd, VIDIOC_CROPCAP, &cropcap)) {
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                crop.c = cropcap.defrect; /* reset to default */

    if (-1 == xioctl(v4l->fd, VIDIOC_S_CROP, &crop)) {
      switch (errno) {
        case EINVAL:
                                /* Cropping not supported. */
        break;
        default:
                                /* Errors ignored. */
        break;
      }
    }
  } else {
                /* Errors ignored. */
  }
  CLEAR(fmt);

  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;
  if (xioctl(v4l->fd, VIDIOC_G_FMT, &fmt) < 0) {
    errno_exit("VIDIOC_G_FMT");
  }
  v4l->pix_fmt = fmt.fmt.pix.pixelformat;
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = v4l->width; //replace
  fmt.fmt.pix.height      = v4l->height; //replace
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;
  if (xioctl(v4l->fd, VIDIOC_S_FMT, &fmt) < 0) {
    errno_exit("VIDIOC_S_FMT");
  }
  min = fmt.fmt.pix.width * 2;
  if (fmt.fmt.pix.bytesperline < min)
    fmt.fmt.pix.bytesperline = min;
  min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
  if (fmt.fmt.pix.sizeimage < min)
    fmt.fmt.pix.sizeimage = min;
  init_mmap(v4l);
}

static void close_device(v4l_st *v4l) {
  if (close(v4l->fd) < 0)
    errno_exit("close");
  v4l->fd = -1;
}

static void open_device(v4l_st *v4l) {
  struct stat st;
  if (stat(v4l->dev_name, &st) < 0) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\n",
     v4l->dev_name, errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
  if (!S_ISCHR(st.st_mode)) {
    fprintf(stderr, "%s is no device\n", v4l->dev_name);
    exit(EXIT_FAILURE);
  }
  v4l->fd = open(v4l->dev_name, O_RDWR | O_NONBLOCK, 0);
  if (v4l->fd < 0) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n",
     v4l->dev_name, errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

void print_usage(char *cmd) {
  fprintf(stdout,
   "Usage: %s [options]\n\n"
   "Options:\n"
   "-d | --device name   Video device name [/dev/video0]\n"
   "-c | --frames        Number of frames to capture [250]\n"
   "-W | --width         Capture width [640]\n"
   "-H | --height        Capture height [480]\n"
   "-o | --output        H264 output file [out.264]\n"
   "-q | --qp            Quantization parameter (0-51) [28]\n"
   "-h | --help          Print this message\n"
   "",
   cmd);
}

static const char short_options[] = "d:c:W:H:q:o:h";

static const struct option
long_options[] = {
        { "device", required_argument, NULL, 'd' },
        { "frames", required_argument, NULL, 'c' },
        { "qp", required_argument, NULL, 'q' },
        { "width", required_argument, NULL, 'W' },
        { "height", required_argument, NULL, 'H' },
        { "output", required_argument, NULL, 'o' },
        { "help",   no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 }
};

void parse_options(v4l_st *v4l, int argc, char **argv) {
  int idx;
  int c;
  for (;;) {
    c = getopt_long(argc, argv,
      short_options, long_options, &idx);
    if (c < 0) break;
    switch (c) {
      case 'd':
        snprintf(v4l->dev_name, sizeof(v4l->dev_name), "%s", optarg);
        break;
      case 'c':
        v4l->frame_count = atoi(optarg);
        break;
      case 'q':
        v4l->qp = atoi(optarg);
        break;
      case 'W':
        v4l->width = atoi(optarg);
        break;
      case 'H':
        v4l->height = atoi(optarg);
        break;
      case 'o':
        snprintf(v4l->output, sizeof(v4l->output), "%s", optarg);
        break;
      case 'h':
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
      default:
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
  }
}

int main(int argc, char **argv) {
  v4l_st v4l;
  CLEAR(v4l);
  sprintf(v4l.dev_name, "/dev/video0");
  sprintf(v4l.output, "out.264");
  v4l.fd = -1;
  v4l.width = 640;
  v4l.height = 480;
  v4l.frame_count = 250;
  v4l.qp = 28;

  if (argc == 1) {
    print_usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  parse_options(&v4l, argc, argv);

  open_device(&v4l);
  init_device(&v4l);
  v4l.rawframe = calloc(v4l.width * v4l.height * 1.5, sizeof(unsigned char));
  v4l.frame_sz = v4l.width * v4l.height * 1.5;
  v4l.va_ctx = va_context_init(v4l.rawframe, v4l.output,
   v4l.width, v4l.height, v4l.qp);
  if (v4l.va_ctx == NULL) {
    fprintf(stderr, "Error initializing va_ctx\n");
    return 1;
  }
  start_capturing(&v4l);
  mainloop(&v4l);
  stop_capturing(&v4l);
  v4l.stats.coded_sz = va_context_free(v4l.va_ctx);
  free(v4l.rawframe);
  if (v4l.sws != NULL) {
    sws_freeContext(v4l.sws);
  }
  uninit_device(&v4l);
  close_device(&v4l);
  printf("\nraw data captured: %0.2fMB\n", (float)(v4l.stats.raw_sz / (float)(1024 * 1024)));
  printf("encoded data: %0.2fMB\n", ((float)v4l.stats.coded_sz / (float)(1024 * 1024)));
  printf("compression ratio: %d:1\n", (int) ((float)v4l.stats.raw_sz / (float)v4l.stats.coded_sz));
  printf("encoded %d frames in %"PRIu64"ms, average of %0.2ffps\n",
   v4l.frame_number, v4l.stats.enc_time,
    (float)(v4l.frame_number) / (float)((float)v4l.stats.enc_time / 1000.00f));
  return 0;
}
