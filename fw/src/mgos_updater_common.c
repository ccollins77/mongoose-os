/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#include "mgos_updater_common.h"

#include <stdio.h>
#include <strings.h>

#include "common/cs_crc32.h"
#include "common/cs_file.h"
#include "common/str_util.h"

#include "mgos_hal.h"
#include "mgos_hooks.h"
#include "mgos_sys_config.h"
#include "mgos_timers.h"
#include "mgos_updater_hal.h"

/*
 * Using static variable (not only c->user_data), it allows to check if update
 * already in progress when another request arrives
 */
struct update_context *s_ctx = NULL;

/* Must be provided externally, usually auto-generated. */
extern const char *build_id;
extern const char *build_version;

#define UPDATER_CTX_FILE_NAME "updater.dat"
#define MANIFEST_FILENAME "manifest.json"
#define SHA1SUM_LEN 40

static mgos_upd_event_cb s_event_cb = NULL;
static void *s_event_cb_arg = NULL;

#define CALL_HOOK(ll, _state, _fmt, ...)                               \
  do {                                                                 \
    char buf[100];                                                     \
    snprintf(buf, sizeof(buf), _fmt, __VA_ARGS__);                     \
    LOG(ll, ("%s", buf));                                              \
    struct mgos_ota_status ota_status = {.state = _state, .msg = buf}; \
    struct mgos_hook_arg ha = {.ota_status = ota_status};              \
    mgos_hook_trigger(MGOS_HOOK_OTA_STATUS, &ha);                      \
  } while (0)

/*
 * --- Zip file local header structure ---
 *                                             size  offset
 * local file header signature   (0x04034b50)   4      0
 * version needed to extract                    2      4
 * general purpose bit flag                     2      6
 * compression method                           2      8
 * last mod file time                           2      10
 * last mod file date                           2      12
 * crc-32                                       4      14
 * compressed size                              4      18
 * uncompressed size                            4      22
 * file name length                             2      26
 * extra field length                           2      28
 * file name (variable size)                    v      30
 * extra field (variable size)                  v
 */

#define ZIP_LOCAL_HDR_SIZE 30U
#define ZIP_GENFLAG_OFFSET 6U
#define ZIP_COMPRESSION_METHOD_OFFSET 8U
#define ZIP_CRC32_OFFSET 14U
#define ZIP_COMPRESSED_SIZE_OFFSET 18U
#define ZIP_UNCOMPRESSED_SIZE_OFFSET 22U
#define ZIP_FILENAME_LEN_OFFSET 26U
#define ZIP_EXTRAS_LEN_OFFSET 28U
#define ZIP_FILENAME_OFFSET 30U
#define ZIP_FILE_DESCRIPTOR_SIZE 12U

const uint32_t c_zip_file_header_magic = 0x04034b50;
const uint32_t c_zip_cdir_magic = 0x02014b50;

enum update_state {
  US_INITED = 0,
  US_WAITING_MANIFEST_HEADER,
  US_WAITING_MANIFEST,
  US_WAITING_FILE_HEADER,
  US_WAITING_FILE,
  US_SKIPPING_DATA,
  US_SKIPPING_DESCRIPTOR,
  US_WRITE_FINISHED,
  US_FINALIZE,
  US_FINISHED,
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void updater_abort(void *arg) {
  struct update_context *ctx = (struct update_context *) arg;
  if (s_ctx != ctx) return;
  LOG(LL_ERROR, ("Update timed out"));
  /* Note that we do not free the context here, because whatever process
   * is stuck may still be referring to it. We close the network connection,
   * if there is one, to hopefully get things to wind down cleanly. */
  if (ctx->nc) ctx->nc->flags |= MG_F_CLOSE_IMMEDIATELY;
  s_ctx = NULL;
}

struct update_context *updater_context_create() {
  if (s_ctx != NULL) {
    LOG(LL_ERROR, ("Update already in progress"));
    return NULL;
  }

  if (!mgos_upd_is_committed()) {
    CALL_HOOK(LL_ERROR, "error", "%s",
              "Previous update has not been committed yet");
    return NULL;
  }

  if (s_event_cb != NULL) {
    bool ok = s_event_cb(MGOS_UPD_EV_INIT, NULL, s_event_cb_arg);
    if (!ok) {
      LOG(LL_ERROR, ("Update declined by user callback"));
      return NULL;
    }
  }

  s_ctx = calloc(1, sizeof(*s_ctx));
  if (s_ctx == NULL) {
    LOG(LL_ERROR, ("Out of memory"));
    return NULL;
  }

  s_ctx->dev_ctx = mgos_upd_hal_ctx_create();

  CALL_HOOK(LL_INFO, "init", "starting, timeout %d",
            mgos_sys_config_get_update_timeout());
  s_ctx->wdt = mgos_set_timer(mgos_sys_config_get_update_timeout() * 1000,
                              false /* repeat */, updater_abort, s_ctx);
  return s_ctx;
}

struct update_context *updater_context_get_current(void) {
  return s_ctx;
}

void updater_set_status(struct update_context *ctx, enum update_state st) {
  LOG(LL_DEBUG, ("Update state %d -> %d", (int) ctx->update_state, (int) st));
  ctx->update_state = st;
}

/*
 * During its work, updater requires requires to store some data.
 * For example, manifest file, zip header - must be received fully, while
 * content FW/FS files can be flashed directly from recv_mbuf
 * To avoid extra memory usage, context contains plain pointer (*data)
 * and mbuf (unprocessed); data is storing in memory only if where is no way
 * to process it right now.
 */
static void context_update(struct update_context *ctx, const char *data,
                           size_t len) {
  if (ctx->unprocessed.len != 0) {
    /* We have unprocessed data, concatenate them with arrived */
    mbuf_append(&ctx->unprocessed, data, len);
    ctx->data = ctx->unprocessed.buf;
    ctx->data_len = ctx->unprocessed.len;
  } else {
    /* No unprocessed, trying to process directly received data */
    ctx->data = data;
    ctx->data_len = len;
  }

  LOG(LL_DEBUG, ("Added %u, size: %u", len, ctx->data_len));
}

static void context_save_unprocessed(struct update_context *ctx) {
  if (ctx->unprocessed.len == 0) {
    mbuf_append(&ctx->unprocessed, ctx->data, ctx->data_len);
    ctx->data = ctx->unprocessed.buf;
    ctx->data_len = ctx->unprocessed.len;
    if (ctx->data_len > 0) {
      LOG(LL_DEBUG, ("Added %d bytes to cached data", ctx->data_len));
    }
  }
}

void context_remove_data(struct update_context *ctx, size_t len) {
  if (ctx->unprocessed.len != 0) {
    /* Consumed data from unprocessed*/
    mbuf_remove(&ctx->unprocessed, len);
    ctx->data = ctx->unprocessed.buf;
    ctx->data_len = ctx->unprocessed.len;
  } else {
    /* Consumed received data */
    ctx->data = ctx->data + len;
    ctx->data_len -= len;
  }

  LOG(LL_DEBUG, ("Consumed %u, %u left", len, ctx->data_len));
}

static void context_clear_current_file(struct update_context *ctx) {
  memset(&ctx->info.current_file, 0, sizeof(ctx->info.current_file));
  ctx->current_file_crc = ctx->current_file_crc_calc = 0;
  ctx->current_file_has_descriptor = false;
}

int is_write_finished(struct update_context *ctx) {
  return ctx->update_state == US_WRITE_FINISHED;
}

int is_update_finished(struct update_context *ctx) {
  return ctx->update_state == US_FINISHED;
}

int is_reboot_required(struct update_context *ctx) {
  return ctx->need_reboot;
}

static int parse_zip_file_header(struct update_context *ctx) {
  if (ctx->data_len < ZIP_LOCAL_HDR_SIZE) {
    LOG(LL_DEBUG, ("Zip header is incomplete"));
    /* Need more data*/
    return 0;
  }

  if (memcmp(ctx->data, &c_zip_file_header_magic, 4) != 0) {
    ctx->status_msg = "Malformed archive (invalid file header)";
    return -1;
  }

  uint16_t file_name_len, extras_len;
  memcpy(&file_name_len, ctx->data + ZIP_FILENAME_LEN_OFFSET,
         sizeof(file_name_len));
  memcpy(&extras_len, ctx->data + ZIP_EXTRAS_LEN_OFFSET, sizeof(extras_len));

  LOG(LL_DEBUG, ("Filename len = %d bytes, extras len = %d bytes",
                 (int) file_name_len, (int) extras_len));
  if (ctx->data_len < ZIP_LOCAL_HDR_SIZE + file_name_len + extras_len) {
    /* Still need mode data */
    return 0;
  }

  uint16_t compression_method;
  memcpy(&compression_method, ctx->data + ZIP_COMPRESSION_METHOD_OFFSET,
         sizeof(compression_method));

  LOG(LL_DEBUG, ("Compression method=%d", (int) compression_method));
  if (compression_method != 0) {
    /* Do not support compressed archives */
    ctx->status_msg = "Cannot handle compressed .zip";
    LOG(LL_ERROR, ("File is compressed)"));
    return -1;
  }

  int i;
  char *nodir_file_name = (char *) ctx->data + ZIP_FILENAME_OFFSET;
  uint16_t nodir_file_name_len = file_name_len;
  LOG(LL_DEBUG,
      ("File name: %.*s", (int) nodir_file_name_len, nodir_file_name));

  for (i = 0; i < file_name_len; i++) {
    /* archive may contain folder, but we skip it, using filenames only */
    if (*(ctx->data + ZIP_FILENAME_OFFSET + i) == '/') {
      nodir_file_name = (char *) ctx->data + ZIP_FILENAME_OFFSET + i + 1;
      nodir_file_name_len -= (i + 1);
      break;
    }
  }

  LOG(LL_DEBUG,
      ("File name to use: %.*s", (int) nodir_file_name_len, nodir_file_name));

  if (nodir_file_name_len >= sizeof(ctx->info.current_file.name)) {
    /* We are in charge of file names, right? */
    LOG(LL_ERROR, ("Too long file name"));
    ctx->status_msg = "Too long file name";
    return -1;
  }
  memcpy(ctx->info.current_file.name, nodir_file_name, nodir_file_name_len);

  memcpy(&ctx->info.current_file.size, ctx->data + ZIP_COMPRESSED_SIZE_OFFSET,
         sizeof(ctx->info.current_file.size));

  uint32_t uncompressed_size;
  memcpy(&uncompressed_size, ctx->data + ZIP_UNCOMPRESSED_SIZE_OFFSET,
         sizeof(uncompressed_size));

  if (ctx->info.current_file.size != uncompressed_size) {
    /* Probably malformed archive*/
    LOG(LL_ERROR, ("Malformed archive"));
    ctx->status_msg = "Malformed archive";
    return -1;
  }

  LOG(LL_DEBUG, ("File size: %u", (unsigned int) ctx->info.current_file.size));

  uint16_t gen_flag;
  memcpy(&gen_flag, ctx->data + ZIP_GENFLAG_OFFSET, sizeof(gen_flag));
  ctx->current_file_has_descriptor = ((gen_flag & (1 << 3)) != 0);

  LOG(LL_DEBUG, ("General flag=%d", (int) gen_flag));

  memcpy(&ctx->current_file_crc, ctx->data + ZIP_CRC32_OFFSET,
         sizeof(ctx->current_file_crc));

  LOG(LL_DEBUG, ("CRC32: 0x%08x", (unsigned int) ctx->current_file_crc));

  context_remove_data(ctx, ZIP_LOCAL_HDR_SIZE + file_name_len + extras_len);

  return 1;
}

static int parse_manifest(struct update_context *ctx) {
  struct mgos_upd_info *info = &ctx->info;
  ctx->manifest_data = calloc(1, info->current_file.size);
  if (ctx->manifest_data == NULL) {
    ctx->status_msg = "Out of memory";
    return -1;
  }
  memcpy(ctx->manifest_data, ctx->data, info->current_file.size);

  if (json_scanf(
          ctx->manifest_data, info->current_file.size,
          "{name: %T, platform: %T, version: %T, build_id: %T, parts: %T}",
          &info->name, &info->platform, &info->version, &info->build_id,
          &info->parts) <= 0) {
    ctx->status_msg = "Failed to parse manifest";
    return -1;
  }

  if (info->platform.len == 0 || info->version.len == 0 ||
      info->build_id.len == 0 || info->parts.len == 0) {
    ctx->status_msg = "Required manifest field missing";
    return -1;
  }

  LOG(LL_INFO,
      ("FW: %.*s %.*s %s %s -> %.*s %.*s", (int) info->name.len, info->name.ptr,
       (int) info->platform.len, info->platform.ptr, build_version, build_id,
       (int) info->version.len, info->version.ptr, (int) info->build_id.len,
       info->build_id.ptr));

  context_remove_data(ctx, info->current_file.size);

  return 1;
}

static int finalize_write(struct update_context *ctx, struct mg_str tail) {
  /* We have to add the tail to CRC now to be able to verify it. */
  if (tail.len > 0) {
    ctx->current_file_crc_calc = cs_crc32(ctx->current_file_crc_calc,
                                          (const uint8_t *) tail.p, tail.len);
  }

  if (ctx->current_file_crc != 0 &&
      ctx->current_file_crc != ctx->current_file_crc_calc) {
    LOG(LL_ERROR, ("Invalid CRC, want 0x%x, got 0x%x",
                   (unsigned int) ctx->current_file_crc,
                   (unsigned int) ctx->current_file_crc_calc));
    ctx->status_msg = "Invalid CRC";
    return -1;
  }

  int ret = mgos_upd_file_end(ctx->dev_ctx, &ctx->info.current_file, tail);
  if (ret != (int) tail.len) {
    if (ret < 0) {
      ctx->status_msg = mgos_upd_get_status_msg(ctx->dev_ctx);
    } else {
      ctx->status_msg = "Not all data was processed";
      ret = -1;
    }
    return ret;
  }

  context_remove_data(ctx, tail.len);

  return 1;
}

static int updater_process_int(struct update_context *ctx, const char *data,
                               size_t len) {
  int ret;
  if (len != 0) {
    context_update(ctx, data, len);
  }

  while (true) {
    switch (ctx->update_state) {
      case US_INITED: {
        updater_set_status(ctx, US_WAITING_MANIFEST_HEADER);
      } /* fall through */
      case US_WAITING_MANIFEST_HEADER: {
        if ((ret = parse_zip_file_header(ctx)) <= 0) {
          if (ret == 0) {
            context_save_unprocessed(ctx);
          }
          return ret;
        }
        if (strncmp(ctx->info.current_file.name, MANIFEST_FILENAME,
                    sizeof(MANIFEST_FILENAME)) != 0) {
          /* We've got file header, but it isn't not metadata */
          LOG(LL_ERROR, ("Get %s instead of %s", ctx->info.current_file.name,
                         MANIFEST_FILENAME));
          return -1;
        }
        updater_set_status(ctx, US_WAITING_MANIFEST);
      } /* fall through */
      case US_WAITING_MANIFEST: {
        /*
         * Assume metadata isn't too big and might be cached
         * otherwise we need streaming json-parser
         */
        if (ctx->data_len < ctx->info.current_file.size) {
          context_save_unprocessed(ctx);
          return 0;
        }

        if (ctx->current_file_crc != 0 &&
            cs_crc32(0, (const uint8_t *) ctx->data,
                     ctx->info.current_file.size) != ctx->current_file_crc) {
          ctx->status_msg = "Invalid CRC";
          return -1;
        }

        if ((ret = parse_manifest(ctx)) < 0) return ret;

        if (strncasecmp(ctx->info.platform.ptr,
                        CS_STRINGIFY_MACRO(FW_ARCHITECTURE),
                        strlen(CS_STRINGIFY_MACRO(FW_ARCHITECTURE))) != 0) {
          LOG(LL_ERROR,
              ("Wrong platform: want \"%s\", got \"%s\"",
               CS_STRINGIFY_MACRO(FW_ARCHITECTURE), ctx->info.platform.ptr));
          ctx->status_msg = "Wrong platform";
          return -1;
        }

        if (ctx->ignore_same_version &&
            strncmp(ctx->info.version.ptr, build_version,
                    ctx->info.version.len) == 0 &&
            strncmp(ctx->info.build_id.ptr, build_id, ctx->info.build_id.len) ==
                0) {
          ctx->status_msg = "Version is the same as current";
          return 1;
        }

        if (s_event_cb != NULL) {
          bool ok = s_event_cb(MGOS_UPD_EV_BEGIN, &ctx->info, s_event_cb_arg);
          if (!ok) {
            ctx->status_msg = "Update declined by user callback";
            return -101;
          }
        }

        if ((ret = mgos_upd_begin(ctx->dev_ctx, &ctx->info.parts)) < 0) {
          ctx->status_msg = mgos_upd_get_status_msg(ctx->dev_ctx);
          LOG(LL_ERROR, ("Bad manifest: %d %s", ret, ctx->status_msg));
          return ret;
        }

        context_clear_current_file(ctx);
        updater_set_status(ctx, US_WAITING_FILE_HEADER);
      } /* fall through */
      case US_WAITING_FILE_HEADER: {
        if (ctx->data_len < 4) {
          context_save_unprocessed(ctx);
          return 0;
        }
        if (memcmp(ctx->data, &c_zip_cdir_magic, 4) == 0) {
          LOG(LL_DEBUG, ("Reached the end of archive"));
          updater_set_status(ctx, US_WRITE_FINISHED);
          break;
        }
        if ((ret = parse_zip_file_header(ctx)) <= 0) {
          if (ret == 0) context_save_unprocessed(ctx);
          return ret;
        }

        enum mgos_upd_file_action r =
            mgos_upd_file_begin(ctx->dev_ctx, &ctx->info.current_file);

        if (r == MGOS_UPDATER_ABORT) {
          ctx->status_msg = mgos_upd_get_status_msg(ctx->dev_ctx);
          return -1;
        } else if (r == MGOS_UPDATER_SKIP_FILE) {
          updater_set_status(ctx, US_SKIPPING_DATA);
          break;
        }
        if (s_event_cb != NULL) {
          s_event_cb(MGOS_UPD_EV_PROGRESS, &ctx->info, s_event_cb_arg);
        }
        updater_set_status(ctx, US_WAITING_FILE);
        ctx->current_file_crc_calc = 0;
      } /* fall through */
      case US_WAITING_FILE: {
        struct mg_str to_process;
        to_process.p = ctx->data;
        to_process.len =
            MIN(ctx->info.current_file.size - ctx->info.current_file.processed,
                ctx->data_len);

        int num_processed = mgos_upd_file_data(
            ctx->dev_ctx, &ctx->info.current_file, to_process);
        if (num_processed < 0) {
          ctx->status_msg = mgos_upd_get_status_msg(ctx->dev_ctx);
          return num_processed;
        } else if (num_processed > 0) {
          ctx->current_file_crc_calc =
              cs_crc32(ctx->current_file_crc_calc,
                       (const uint8_t *) to_process.p, num_processed);
          context_remove_data(ctx, num_processed);
          ctx->info.current_file.processed += num_processed;
        }
        LOG(LL_DEBUG,
            ("Processed %d, up to %u, %u left in the buffer", num_processed,
             (unsigned int) ctx->info.current_file.processed, ctx->data_len));
        CALL_HOOK(LL_DEBUG, "progress", "%s %d of %d",
                  ctx->info.current_file.name, ctx->info.current_file.processed,
                  ctx->info.current_file.size);
        if (s_event_cb != NULL) {
          s_event_cb(MGOS_UPD_EV_PROGRESS, &ctx->info, s_event_cb_arg);
        }

        uint32_t bytes_left =
            ctx->info.current_file.size - ctx->info.current_file.processed;
        if (bytes_left > ctx->data_len) {
          context_save_unprocessed(ctx);
          return 0;
        }

        to_process.p = ctx->data;
        to_process.len = bytes_left;

        if (finalize_write(ctx, to_process) < 0) {
          return -1;
        }
        context_clear_current_file(ctx);
        updater_set_status(ctx, US_WAITING_FILE_HEADER);
        break;
      }
      case US_SKIPPING_DATA: {
        uint32_t to_skip =
            MIN(ctx->data_len,
                ctx->info.current_file.size - ctx->info.current_file.processed);
        ctx->info.current_file.processed += to_skip;
        LOG(LL_DEBUG, ("Skipping %u bytes, %u total", (unsigned int) to_skip,
                       (unsigned int) ctx->info.current_file.processed));
        context_remove_data(ctx, to_skip);
        if (s_event_cb != NULL) {
          s_event_cb(MGOS_UPD_EV_PROGRESS, &ctx->info, s_event_cb_arg);
        }

        if (ctx->info.current_file.processed < ctx->info.current_file.size) {
          context_save_unprocessed(ctx);
          return 0;
        }

        context_clear_current_file(ctx);
        updater_set_status(ctx, US_SKIPPING_DESCRIPTOR);
      } /* fall through */
      case US_SKIPPING_DESCRIPTOR: {
        bool has_descriptor = ctx->current_file_has_descriptor;
        LOG(LL_DEBUG, ("Has descriptor : %d", has_descriptor));
        context_clear_current_file(ctx);
        ctx->current_file_has_descriptor = false;
        if (has_descriptor) {
          /* If file has descriptor we have to skip 12 bytes after its body */
          ctx->info.current_file.size = ZIP_FILE_DESCRIPTOR_SIZE;
          updater_set_status(ctx, US_SKIPPING_DATA);
        } else {
          updater_set_status(ctx, US_WAITING_FILE_HEADER);
        }

        context_save_unprocessed(ctx);
        break;
      }
      case US_WRITE_FINISHED: {
        /* We will stay in this state until explicitly finalized. */
        return 0;
      }
      case US_FINALIZE: {
        ret = 1;
        ctx->status_msg = "Update applied, finalizing";
        CALL_HOOK(LL_INFO, "finalizing", "commit timeout %d",
                  ctx->fctx.commit_timeout);
        if (ctx->fctx.commit_timeout > 0) {
          if (!mgos_upd_set_commit_timeout(ctx->fctx.commit_timeout)) {
            ctx->status_msg = "Cannot save update status";
            return -1;
          }
        }
        if ((ret = mgos_upd_finalize(ctx->dev_ctx)) < 0) {
          ctx->status_msg = mgos_upd_get_status_msg(ctx->dev_ctx);
          return ret;
        }
        ctx->result = 1;
        ctx->need_reboot = 1;
        updater_finish(ctx);
        break;
      }
      case US_FINISHED: {
        /* After receiving manifest, fw & fs just skipping all data */
        context_remove_data(ctx, ctx->data_len);
        if (ctx->result_cb != NULL) {
          ctx->result_cb(ctx);
          ctx->result_cb = NULL;
        }
        return ctx->result;
      }
    }
  }
}

int updater_process(struct update_context *ctx, const char *data, size_t len) {
  ctx->result = updater_process_int(ctx, data, len);
  if (ctx->result != 0) {
    updater_finish(ctx);
  }
  return ctx->result;
}

int updater_finalize(struct update_context *ctx) {
  updater_set_status(ctx, US_FINALIZE);
  return updater_process(ctx, NULL, 0);
}

void updater_finish(struct update_context *ctx) {
  if (ctx->update_state == US_FINISHED) return;
  updater_set_status(ctx, US_FINISHED);
  const char *msg = (ctx->status_msg ? ctx->status_msg : "???");
  CALL_HOOK(LL_INFO, "done", "Finished: %d %s", ctx->result, msg);
  updater_process_int(ctx, NULL, 0);
  if (s_event_cb != NULL) {
    (void) s_event_cb(MGOS_UPD_EV_END, ctx, s_event_cb_arg);
  }
}

void updater_context_free(struct update_context *ctx) {
  if (!is_update_finished(ctx)) {
    LOG(LL_ERROR, ("Update terminated unexpectedly"));
  }
  mgos_clear_timer(ctx->wdt);
  mgos_upd_hal_ctx_free(ctx->dev_ctx);
  mbuf_free(&ctx->unprocessed);
  free(ctx->manifest_data);
  free(ctx);
  if (ctx == s_ctx) s_ctx = NULL;
}

void bin2hex(const uint8_t *src, int src_len, char *dst) {
  int i = 0;
  for (i = 0; i < src_len; i++) {
    sprintf(dst, "%02x", (int) *src);
    dst += 2;
    src += 1;
  }
}

static bool file_copy(const char *old_path, const char *new_path,
                      const char *name, char tmp_name[MG_MAX_PATH]) {
  bool ret = false;
  FILE *old_f = NULL, *new_f = NULL;
  struct stat st;
  int readen, to_read = 0, total = 0;

  LOG(LL_INFO, ("Copying %s", name));

  sprintf(tmp_name, "%s/%s", old_path, name);
  old_f = fopen(tmp_name, "r");
  if (old_f == NULL) {
    LOG(LL_ERROR, ("Failed to open %s for reading", tmp_name));
    goto out;
  }
  if (stat(tmp_name, &st) != 0) {
    LOG(LL_ERROR, ("Cannot get previous %s size", tmp_name));
    goto out;
  }

  sprintf(tmp_name, "%s/%s", new_path, name);
  new_f = fopen(tmp_name, "w");
  if (new_f == NULL) {
    LOG(LL_ERROR, ("Failed to open %s for writing", tmp_name));
    goto out;
  }

  char buf[128];
  to_read = MIN(sizeof(buf), (size_t) st.st_size);
  while (to_read != 0) {
    if ((readen = fread(buf, 1, to_read, old_f)) < 0) {
      LOG(LL_ERROR, ("Failed to read %d bytes from %s", to_read, name));
      goto out;
    }

    if (fwrite(buf, 1, readen, new_f) != (size_t) readen) {
      LOG(LL_ERROR, ("Failed to write %d bytes to %s", readen, name));
      goto out;
    }

    total += readen;
    to_read = MIN(sizeof(buf), (size_t)(st.st_size - total));
  }

  LOG(LL_DEBUG, ("Wrote %d to %s", total, tmp_name));

  ret = true;

out:
  if (old_f != NULL) fclose(old_f);
  if (new_f != NULL) {
    fclose(new_f);
    if (!ret) remove(tmp_name);
  }
  return ret;
}

bool mgos_upd_merge_fs(const char *old_fs_path, const char *new_fs_path) {
  bool ret = false;
  DIR *dir = opendir(old_fs_path);
  if (dir == NULL) {
    LOG(LL_ERROR, ("Failed to open root directory"));
    goto out;
  }

  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    struct stat st;
    char tmp_name[MG_MAX_PATH];
    sprintf(tmp_name, "%s/%s", new_fs_path, de->d_name);
    if (stat(tmp_name, &st) != 0) {
      /* File not found on the new fs, copy. */
      if (!file_copy(old_fs_path, new_fs_path, de->d_name, tmp_name)) {
        LOG(LL_ERROR, ("Failed to copy %s", de->d_name));
        goto out;
      }
    }
    mgos_wdt_feed();
  }
  ret = true;

out:
  if (dir != NULL) closedir(dir);
  return ret;
}

bool mgos_upd_commit() {
  if (mgos_upd_is_committed()) return false;
  CALL_HOOK(LL_INFO, "commit", "%s", "Committing update");
  if (s_event_cb) (void) s_event_cb(MGOS_UPD_EV_COMMIT, NULL, s_event_cb_arg);
  mgos_upd_boot_commit();
  remove(UPDATER_CTX_FILE_NAME);
  return true;
}

bool mgos_upd_is_committed() {
  struct mgos_upd_boot_state s;
  if (!mgos_upd_boot_get_state(&s)) return false;
  return s.is_committed;
}

bool mgos_upd_revert(bool reboot) {
  if (mgos_upd_is_committed()) return false;
  CALL_HOOK(LL_INFO, "rollback", "%s", "Reverting update");
  if (s_event_cb) (void) s_event_cb(MGOS_UPD_EV_ROLLBACK, NULL, s_event_cb_arg);
  mgos_upd_boot_revert();
  if (reboot) mgos_system_restart();
  return true;
}

void mgos_upd_watchdog_cb(void *arg) {
  if (!mgos_upd_is_committed()) {
    /* Timer fired and update has not been committed. Revert! */
    LOG(LL_ERROR, ("Update commit timeout expired"));
    mgos_upd_revert(true /* reboot */);
  }
  (void) arg;
}

int mgos_upd_get_commit_timeout() {
  size_t len;
  char *data = cs_read_file(UPDATER_CTX_FILE_NAME, &len);
  if (data == NULL) return 0;
  struct update_file_context *fctx = (struct update_file_context *) data;
  LOG(LL_INFO, ("Update state: %d", fctx->commit_timeout));
  int res = fctx->commit_timeout;
  free(data);
  return res;
}

bool mgos_upd_set_commit_timeout(int commit_timeout) {
  bool ret = false;
  LOG(LL_DEBUG, ("Writing update state to %s", UPDATER_CTX_FILE_NAME));
  FILE *fp = fopen(UPDATER_CTX_FILE_NAME, "w");
  if (fp == NULL) return false;
  struct update_file_context fctx;
  fctx.commit_timeout = commit_timeout;
  if (fwrite(&fctx, sizeof(fctx), 1, fp) == 1) {
    ret = true;
  }
  fclose(fp);
  return ret;
}

void mgos_upd_boot_finish(bool is_successful, bool is_first) {
  /*
   * If boot is not successful, there's only one thing to do:
   * revert update (if any) and reboot.
   * If this was the first boot after an update, this will revert it.
   */
  LOG(LL_DEBUG, ("%d %d", is_successful, is_first));
  if (!is_first) return;
  if (!is_successful) {
    if (s_event_cb)
      (void) s_event_cb(MGOS_UPD_EV_ROLLBACK, NULL, s_event_cb_arg);
    mgos_upd_boot_revert(true /* reboot */);
    /* Not reached */
    return;
  }
  /* We booted. Now see if we have any special instructions. */
  int commit_timeout = mgos_upd_get_commit_timeout();
  if (commit_timeout > 0) {
    LOG(LL_INFO, ("Arming commit watchdog for %d seconds", commit_timeout));
    mgos_set_timer(commit_timeout * 1000, 0 /* repeat */, mgos_upd_watchdog_cb,
                   NULL);
  } else {
    mgos_upd_commit();
  }
}

void mgos_upd_set_event_cb(mgos_upd_event_cb cb, void *cb_arg) {
  s_event_cb = cb;
  s_event_cb_arg = cb_arg;
}
