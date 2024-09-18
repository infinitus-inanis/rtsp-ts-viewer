#include <argp.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#include <glib.h>
#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <gst/rtsp/rtsp.h>

#define GST_PL             "pl"
#define GST_PL_SOURCE      "pl-source"
#define GST_PL_DEPAYLOADER "pl-depayloader"
#define GST_PL_APPSINK     "pl-appsink"

#define __unused __attribute_maybe_unused__

typedef struct {
  gchar   *url;
  guint32  sampling_rate;
} args_t;

static error_t
args_parser(int key, char *arg, struct argp_state *state) {
  args_t *args = state->input;
  switch (key) {
    case 'f':
      args->sampling_rate = (guint32)(atoi(arg));
      break;

    case ARGP_KEY_ARG:
      if (state->arg_num == 0)
        args->url = arg;
      else
        argp_error(state, "To many arguments.");
      break;

    case ARGP_KEY_END:
      if (!args->url)
        argp_error(state, "Missing RTSP-URL argument.");
      break;

    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static void
args_parse(int argc, char **argv, args_t *args) {
  argp_parse(
    &(struct argp) {
      .args_doc = "<RTSP-URL>",
      .parser   = &args_parser,
      .options  = (struct argp_option[]) {
        { "sampling-rate", 'f', "NUM", 0, "Sampling Rate (Hz). Defaults to 90000", 0 },
        { /*sentinel*/ }
      }
    },
    argc, argv, 0, 0, args
  );
}

static void
timespec_print(struct timespec *ts) {
  time_t t = ts->tv_sec;
  struct tm *tm = localtime(&t);
  char tss[20];
  strftime(tss, sizeof(tss), "%Y-%m-%d %H:%M:%S", tm);
  g_print("%lu.%09ld == %s.%09ld\n",
    ts->tv_sec, ts->tv_nsec, tss, ts->tv_nsec);
}

static guint64
timespec2timestamp(struct timespec *ts) {
  return (guint64)(ts->tv_sec) * 1000000000ULL + (guint64)(ts->tv_nsec);
}

static struct timespec
timespec4timestamp(guint64 nsec) {
  return (struct timespec) {
    .tv_sec  = (guint32)(nsec / 1000000000),
    .tv_nsec = (guint32)(nsec % 1000000000)
  };
}

static struct timespec
ntp2utc(guint64 ntp_msw, guint64 ntp_lsw) {
  return (struct timespec) {
  /* NTP (1900) to UTC (1970) epoch conversion: <NTP_MSW> - (70 * 365 + 17) * 86400 */
    .tv_sec  = (guint32)(ntp_msw - 2208988800),
  /* NTP (1/2^32) to UTC (1/1e9) fraction conversion: <NTP_LSW> * 1e9 / 2^32 */
    .tv_nsec = (guint32)(ntp_lsw * 1000000000 >> 32)
  };
}

typedef struct {
  pthread_t tid;
  sigset_t  mask;
  gboolean  terminate;
  int       number;
} sigwait_ctx_t;

static void *
sigwait_routine(void *arg) {
  sigwait_ctx_t *ctx = (sigwait_ctx_t*)(arg);
  sigwait(&ctx->mask, &ctx->number);
  ctx->terminate = TRUE;
  return NULL;
}

static int
sigwait_start(sigwait_ctx_t *ctx) {
  int rc = 0;
  sigemptyset(&ctx->mask);
  sigaddset(&ctx->mask, SIGTERM);
  sigaddset(&ctx->mask, SIGINT);
  sigaddset(&ctx->mask, SIGQUIT);
  rc = pthread_sigmask(SIG_BLOCK, &ctx->mask, NULL);
  if (!(rc < 0))
    rc = pthread_create(&ctx->tid, NULL, sigwait_routine, ctx);
  return rc;
}

typedef struct {
  GstElement *pipeline;
  struct {
    guint32 sampling_rate;
  } config;
  struct {
    struct timespec utc; /* converted ntp timestamp */
    guint32         rtp; /* relative rtp timestamp in microseconds */
    gboolean        set; /* rtcp packet could be delayed and we don't want to caluclate anything without it */
  } last_rtcp;
} gst_ctx_t;

static void
gst_ctx_dispose(gst_ctx_t *ctx) {
  if (ctx->pipeline)
    gst_object_unref(ctx->pipeline);
}

static GstPadProbeReturn
rtph264depay_sink_pad_probe(__unused GstPad *pad, GstPadProbeInfo *info, gpointer udata) {
  gst_ctx_t *ctx = udata;
  if (!ctx->last_rtcp.set)
    goto __done;

  GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
  GstRTPBuffer rtp_buffer = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp_buffer))
    goto __done;

  guint32 rtp = gst_rtp_buffer_get_timestamp(&rtp_buffer);
  gdouble rtp_base = 1000000000.0 / ctx->config.sampling_rate;
  gdouble rtp_diff = (gdouble)(rtp - ctx->last_rtcp.rtp) * rtp_base;
  gdouble pts_nsec = (gdouble)(timespec2timestamp(&ctx->last_rtcp.utc)) + rtp_diff;
  struct timespec pts = timespec4timestamp((guint64)(pts_nsec));
  timespec_print(&pts);

__done:
  return GST_PAD_PROBE_OK;
}

static void
rtspsrc_on_pad_added(__unused GstElement *rtspsrc, GstPad *new_pad, gst_ctx_t *ctx) {
	if (!g_str_has_prefix(GST_PAD_NAME(new_pad), "recv_rtp_src_"))
		return;

  GstElement *depayloader = gst_bin_get_by_name(GST_BIN(ctx->pipeline), GST_PL_DEPAYLOADER);

	GstPad *sink_pad = gst_element_get_static_pad(depayloader, "sink");
  gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, rtph264depay_sink_pad_probe, ctx, NULL);

  if (!gst_pad_is_linked(sink_pad))
    gst_pad_link(new_pad, sink_pad);

	gst_object_unref(sink_pad);
}

static void
rtpsession_on_receiving_rtcp(__unused GstElement *session, GstBuffer *buffer, gst_ctx_t *ctx) {
  GstRTCPBuffer rtcp_buffer = GST_RTCP_BUFFER_INIT;
  if (!gst_rtcp_buffer_map(buffer, GST_MAP_READ, &rtcp_buffer))
    return;

  GstRTCPPacket rtcp_packet;
  if (!gst_rtcp_buffer_get_first_packet(&rtcp_buffer, &rtcp_packet))
    return;

  do {
    if (rtcp_packet.type != GST_RTCP_TYPE_SR)
      continue;

    /* gst_rtcp_packet_sr_get_sender_info will auto convert network to host byte order */
    guint64 ntp;
    guint32 rtp;
    gst_rtcp_packet_sr_get_sender_info(&rtcp_packet, NULL, &ntp, &rtp, NULL, NULL);

    ctx->last_rtcp.utc = ntp2utc(
      (guint32)((ntp & 0xFFFFFFFF00000000) >> 32), /* NTP MSW */
      (guint32)((ntp & 0x00000000FFFFFFFF) >>  0)  /* NTP LSW */
    );
    ctx->last_rtcp.rtp = rtp;
    ctx->last_rtcp.set = TRUE;
    timespec_print(&ctx->last_rtcp.utc);
  } while (gst_rtcp_packet_move_to_next(&rtcp_packet));
}

static void
rtspsrc_manager_on_new_ssrc(GstElement *rtpbin, guint session, __unused guint ssrc, gst_ctx_t *ctx) {
  GObject *session_obj;
  g_signal_emit_by_name(rtpbin, "get-internal-session", session, &session_obj);
  g_signal_connect_after(session_obj, "on-receiving-rtcp", G_CALLBACK(rtpsession_on_receiving_rtcp), ctx);
  g_object_unref(session_obj);
  g_print("Waiting first RTCP Packet...\n");
}

static void
rtspsrc_on_new_manager(__unused GstElement *rtspsrc, GstElement *manager, gst_ctx_t *ctx) {
  g_signal_connect(manager, "on-new-ssrc", G_CALLBACK(rtspsrc_manager_on_new_ssrc), ctx);
}

static int
rc_from_gst_message(GstMessage *msg) {
  int rc = EXIT_FAILURE;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      rc = EXIT_SUCCESS;
      break;

    case GST_MESSAGE_ERROR:
      GError *err;
      gchar  *dbg;
      gst_message_parse_error(msg, &err, &dbg);
      g_printerr("Error from '%s': %s\n", GST_OBJECT_NAME(msg->src), err->message);
      g_printerr("Debug info: %s\n", dbg ? dbg : "none");
      g_clear_error(&err);
      g_free(dbg);
      break;

    default:
      g_print("Unexpected message.\n");
      break;
  }
  gst_message_unref(msg);
  return rc;
}

static int
rc_from_sig_number(int signo) {
  switch (signo) {
    case SIGINT:  // fallthrough
    case SIGTERM: // fallthrough
    case SIGQUIT: return EXIT_SUCCESS;
    default:      return EXIT_FAILURE;
  }
}

int main(int argc, char **argv) {
  args_t args = {
    .sampling_rate = 90000
  };
  args_parse(argc, argv, &args);
  
  int rc = EXIT_FAILURE;
  sigwait_ctx_t sig = {};
  if (sigwait_start(&sig) < 0) {
    g_printerr("Failed to start sigwait thread.\n");
    goto __exit;
  }
  
  /* May leak memory: 
     https://stackoverflow.com/questions/72811671/gstreamer-minimal-program-leaks-memory */
  gst_init(NULL, NULL);
  gst_ctx_t ctx = {
    .config.sampling_rate = args.sampling_rate
  };
  if (!(ctx.pipeline = gst_pipeline_new(GST_PL))) {
    g_printerr("Failed to make pipeline.\n");
    goto __gst_deinit;
  }

  GstElement *source;
  if (!(source = gst_element_factory_make("rtspsrc", GST_PL_SOURCE))) {
    g_printerr("Failed to make 'rtspsrc' element.\n");
    goto __gst_ctx_dispose;
  }
  gst_bin_add(GST_BIN(ctx.pipeline), source);
  g_object_set(source, "location", args.url, NULL);
  g_object_set(source, "latency", 0, NULL);
  g_object_set(source, "protocols", GST_RTSP_LOWER_TRANS_TCP, NULL);
  g_signal_connect(source, "pad-added", G_CALLBACK(rtspsrc_on_pad_added), &ctx);
  g_signal_connect(source, "new-manager", G_CALLBACK(rtspsrc_on_new_manager), &ctx);

  GstElement *depayloader;
  if (!(depayloader = gst_element_factory_make("rtph264depay", GST_PL_DEPAYLOADER))) {
    g_printerr("Failed to make 'rtph264depay' element.\n");
    goto __gst_ctx_dispose;
  }
  gst_bin_add(GST_BIN(ctx.pipeline), depayloader);

  GstElement *appsink;
  if (!(appsink = gst_element_factory_make("appsink", GST_PL_APPSINK))) {
    g_printerr("Failed to make 'appsink' element.\n");
    goto __gst_ctx_dispose;
  }
  gst_bin_add(GST_BIN(ctx.pipeline), appsink);

  /* Link 'rtspsrc' -> 'depayloader' will be done in `rtspsrc_on_pad_added` callback */
  if (!gst_element_link(depayloader, appsink)) {
    g_printerr("Failed to link 'depayloader' to 'appsink'.\n");
    goto __gst_ctx_dispose;
  }

  GstStateChangeReturn sc_ret = gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
  if (sc_ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to change pipeline state to GST_STATE_PLAYING.\n");
    goto __gst_ctx_dispose;
  }

  GstBus *bus = gst_element_get_bus(ctx.pipeline);
  do {
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 30 * 1000 * 1000, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg) {
      rc = rc_from_gst_message(msg);
      break;
    }
    if (sig.terminate) {
      rc = rc_from_sig_number(sig.number);
      break;
    }
  } while (TRUE);
  gst_object_unref(bus);
  gst_element_set_state(ctx.pipeline, GST_STATE_NULL);

__gst_ctx_dispose:
  gst_ctx_dispose(&ctx);
__gst_deinit:
  gst_deinit();
__exit:
  return rc;
}