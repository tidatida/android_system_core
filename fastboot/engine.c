/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the 
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "fastboot.h"

char *mkmsg(const char *fmt, ...)
{
    char buf[256];
    char *s;
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    
    s = strdup(buf);
    if (s == 0) die("out of memory");
    return s;
}

#define OP_DOWNLOAD   1
#define OP_COMMAND    2
#define OP_QUERY      3
#define OP_NOTICE     4

typedef struct Action Action;

struct Action 
{
    unsigned op;
    Action *next;

    char cmd[64];    
    void *data;
    unsigned size;

    const char *msg;
    int (*func)(Action *a, int status, char *resp);
};

static Action *action_list = 0;
static Action *action_last = 0;

static int cb_default(Action *a, int status, char *resp)
{
    if (status) {
        fprintf(stderr,"FAILED (%s)\n", resp);
    } else {
        fprintf(stderr,"OKAY\n");
    }
    return status;
}

static Action *queue_action(unsigned op, const char *fmt, ...)
{
    Action *a;
    va_list ap;

    a = calloc(1, sizeof(Action));
    if (a == 0) die("out of memory");

    va_start(ap, fmt);
    vsprintf(a->cmd, fmt, ap);
    va_end(ap);

    if (action_last) {
        action_last->next = a;
    } else {
        action_list = a;
    }
    action_last = a;
    a->op = op;
    a->func = cb_default;
    return a;
}

void fb_queue_erase(const char *ptn)
{
    Action *a;
    a = queue_action(OP_COMMAND, "erase:%s", ptn);
    a->msg = mkmsg("erasing '%s'", ptn);
}

void fb_queue_flash(const char *ptn, void *data, unsigned sz)
{
    Action *a;

    a = queue_action(OP_DOWNLOAD, "");
    a->data = data;
    a->size = sz;
    a->msg = mkmsg("sending '%s' (%d KB)", ptn, sz / 1024);

    a = queue_action(OP_COMMAND, "flash:%s", ptn);
    a->msg = mkmsg("writing '%s'", ptn);
}

static int match(char *str, const char **value, unsigned count)
{
    const char *val;
    unsigned n;
    int len;

    for (n = 0; n < count; n++) {
        const char *val = value[n];
        int len = strlen(val);
        int match;

        if ((len > 1) && (val[len-1] == '*')) {
            len--;
            match = !strncmp(val, str, len);
        } else {
            match = !strcmp(val, str);
        }

        if (match) return 1;
    }

    return 0;
}



static int cb_check(Action *a, int status, char *resp, int invert)
{
    const char **value = a->data;
    unsigned count = a->size;
    unsigned n;
    int yes;

    if (status) {
        fprintf(stderr,"FAILED (%s)\n", resp);
        return status;
    }

    yes = match(resp, value, count);
    if (invert) yes = !yes;

    if (yes) {
        fprintf(stderr,"OKAY\n");
        return 0;
    }

    fprintf(stderr,"FAILED\n\n");
    fprintf(stderr,"Device %s is '%s'.\n", a->cmd + 7, resp);
    fprintf(stderr,"Update %s '%s'",
            invert ? "rejects" : "requires", value[0]);
    for (n = 1; n < count; n++) {
        fprintf(stderr," or '%s'", value[n]);
    }
    fprintf(stderr,".\n\n");
    return -1;
}

static int cb_require(Action *a, int status, char *resp)
{
    return cb_check(a, status, resp, 0);
}

static int cb_reject(Action *a, int status, char *resp)
{
    return cb_check(a, status, resp, 1);
}

void fb_queue_require(const char *var, int invert, unsigned nvalues, const char **value)
{
    Action *a;
    a = queue_action(OP_QUERY, "getvar:%s", var);
    a->data = value;
    a->size = nvalues;
    a->msg = mkmsg("checking %s", var);
    a->func = invert ? cb_reject : cb_require;
    if (a->data == 0) die("out of memory");
}

static int cb_display(Action *a, int status, char *resp)
{
    if (status) {
        fprintf(stderr, "%s FAILED (%s)\n", a->cmd, resp);
        return status;
    }
    fprintf(stderr, "%s: %s\n", (char*) a->data, resp);
    return 0;
}

void fb_queue_display(const char *var, const char *prettyname)
{
    Action *a;
    a = queue_action(OP_QUERY, "getvar:%s", var);
    a->data = strdup(prettyname);
    if (a->data == 0) die("out of memory");
    a->func = cb_display;
}

static int cb_do_nothing(Action *a, int status, char *resp)
{
    fprintf(stderr,"\n");
    return 0;
}

void fb_queue_reboot(void)
{
    Action *a = queue_action(OP_COMMAND, "reboot");
    a->func = cb_do_nothing;
    a->msg = "rebooting";
}

void fb_queue_command(const char *cmd, const char *msg)
{
    Action *a = queue_action(OP_COMMAND, cmd);
    a->msg = msg;
}

void fb_queue_download(const char *name, void *data, unsigned size)
{
    Action *a = queue_action(OP_DOWNLOAD, "");
    a->data = data;
    a->size = size;
    a->msg = mkmsg("downloading '%s'", name);
}

void fb_queue_notice(const char *notice)
{
    Action *a = queue_action(OP_NOTICE, "");
    a->data = (void*) notice;
}

void fb_execute_queue(usb_handle *usb)
{
    Action *a;
    char resp[FB_RESPONSE_SZ+1];
    int status;

    a = action_list;
    resp[FB_RESPONSE_SZ] = 0;

    for (a = action_list; a; a = a->next) {
        if (a->msg) {
            fprintf(stderr,"%s... ",a->msg);
        }
        if (a->op == OP_DOWNLOAD) {
            status = fb_download_data(usb, a->data, a->size);
            status = a->func(a, status, status ? fb_get_error() : "");
            if (status) break;
        } else if (a->op == OP_COMMAND) {
            status = fb_command(usb, a->cmd);
            status = a->func(a, status, status ? fb_get_error() : "");
            if (status) break;
        } else if (a->op == OP_QUERY) {
            status = fb_command_response(usb, a->cmd, resp);
            status = a->func(a, status, status ? fb_get_error() : resp);
            if (status) break;
        } else if (a->op == OP_NOTICE) {
            fprintf(stderr,"%s\n",(char*)a->data);
        } else {
            die("bogus action");
        }
    }
}

