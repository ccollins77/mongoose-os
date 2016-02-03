/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef SJ_CLUBBY_H
#define SJ_CLUBBY_H

#include "v7/v7.h"
#include "common/ubjserializer.h"
#include "clubby_proto.h"

#ifndef DISABLE_C_CLUBBY

#define TIMEOUT_CHECK_PERIOD 30000

extern const char clubby_cmd_ready[];
extern const char clubby_cmd_onopen[];
extern const char clubby_cmd_onclose[];

typedef void (*sj_clubby_callback_t)(struct clubby_event *evt, void *user_data);

void sj_init_clubby(struct v7 *v7);

void sj_clubby_send_reply(struct clubby_event *evt, int status,
                          const char *status_msg);

int sj_clubby_register_global_command(const char *cmd, sj_clubby_callback_t cb,
                                      void *user_data);

struct clubby_event *sj_clubby_create_reply(struct clubby_event *evt);

void sj_clubby_free_reply(struct clubby_event *reply);

char *sj_clubby_repl_to_bytes(struct clubby_event *reply, int *len);
struct clubby_event *sj_clubby_bytes_to_repl(char *buf, int len);

/* TODO(alashkin): add more sending functions to header */
#endif /* DISABLE_C_CLUBBY */

#endif /* SJ_CLUBBY_H */
