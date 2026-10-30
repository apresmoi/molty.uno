/* Host tests for main/diag/log_upload.c: job tags, the upload filter, the upload frame, the
 * backlog bound and the persisted copy's format + write policy. */
#include "log_upload.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_job_tag(void)
{
    char t[LOG_TAG_MAX + 1];
    assert(log_job_tag("molty-k7m2q9xa4c.j_AbC-_123xyz0000a", t) && !strcmp(t, "AbC-_123xyz0000a"));
    assert(!log_job_tag("molty-k7m2q9xa4c.j_", t) && !t[0]);           /* empty tag */
    assert(!log_job_tag("j1", t) && !t[0]);                              /* no ".j_" */
    assert(!log_job_tag("molty-k7m2q9xa4c.j_ab.cd", t));                 /* other characters */
    assert(!log_job_tag("molty-k7m2q9xa4c.j_ab cd", t));
    assert(!log_job_tag(NULL, t));
    char id[64];
    snprintf(id, sizeof(id), "m.j_%s", "0123456789abcdef0123456789abcdef");   /* 32: fits */
    assert(log_job_tag(id, t) && strlen(t) == 32);
    snprintf(id, sizeof(id), "m.j_%s", "0123456789abcdef0123456789abcdefX");  /* 33: too long */
    assert(!log_job_tag(id, t) && !t[0]);
}

static void test_jobs(void)
{
    log_jobs_t j = {0};
    assert(!strcmp(log_jobs_current(&j), ""));
    log_jobs_begin(&j, "A");
    assert(!strcmp(log_jobs_current(&j), "A"));
    log_jobs_begin(&j, "B");                       /* an ask overlapping a listen: the newer tags */
    assert(!strcmp(log_jobs_current(&j), "B"));
    log_jobs_end(&j, "B");
    assert(!strcmp(log_jobs_current(&j), "A"));
    log_jobs_end(&j, "A");
    assert(!strcmp(log_jobs_current(&j), ""));
    /* counted: begin, begin, end keeps it (an update's download starts as its question closes) */
    log_jobs_begin(&j, "U"); log_jobs_begin(&j, "U"); log_jobs_end(&j, "U");
    assert(!strcmp(log_jobs_current(&j), "U"));
    log_jobs_end(&j, "U");
    assert(!strcmp(log_jobs_current(&j), ""));
    /* ending a job that is not running changes nothing */
    log_jobs_begin(&j, "A"); log_jobs_end(&j, "Z"); log_jobs_end(&j, "");
    assert(!strcmp(log_jobs_current(&j), "A"));
    /* a third concurrent job replaces the least recent */
    log_jobs_begin(&j, "B"); log_jobs_begin(&j, "C");
    assert(!strcmp(log_jobs_current(&j), "C"));
    log_jobs_end(&j, "C");
    assert(!strcmp(log_jobs_current(&j), "B"));     /* A was replaced */
    log_jobs_end(&j, "A");
    assert(!strcmp(log_jobs_current(&j), "B"));
    log_jobs_end(&j, "B");
    assert(!strcmp(log_jobs_current(&j), ""));
    /* re-beginning a running job makes it the current one */
    log_jobs_begin(&j, "A"); log_jobs_begin(&j, "B"); log_jobs_begin(&j, "A");
    assert(!strcmp(log_jobs_current(&j), "A"));
}

static void test_filter(void)
{
    assert(log_upload_wanted("E (1234) agent_link: forget failed", NULL));
    assert(log_upload_wanted("W (5) wifi: beacon timeout", ""));
    assert(!log_upload_wanted("I (5) wifi: connected", NULL));
    assert(!log_upload_wanted("D (5) x: y", NULL));
    assert(log_upload_wanted("\033[0;31mE (5) t: m\033[0m", NULL));       /* colour escape */
    assert(log_upload_wanted("\033[0;33mW (5) t: m\033[0m", NULL));
    assert(!log_upload_wanted("\033[0;32mI (5) t: m\033[0m", NULL));
    assert(!log_upload_wanted("EVT TL int=1/2 sim=3/4ms", NULL));          /* the routine timeline */
    assert(log_upload_wanted("EVT TL! int=1/2 sim=300/400ms", NULL));      /* its trouble form */
    assert(log_upload_wanted("EVT link.connect int_free=1", NULL));
    assert(log_upload_wanted("EVT audio.say end ESP_OK took=1ms", NULL));
    assert(log_upload_wanted("I (5) wifi: anything", "AbC"));               /* a job's line, any level */
    assert(log_upload_wanted("EVT TL int=1", "AbC"));
    assert(!log_upload_wanted("Error without the ESP_LOG shape", NULL));
    assert(!log_upload_wanted("EVTX", NULL));
    assert(!log_upload_wanted(NULL, NULL));
    assert(!log_upload_wanted("", NULL));
}

static log_line_t L(uint32_t seq, const char *job, const char *text) { return (log_line_t){.seq = seq, .ms = seq * 10, .job = job, .text = text}; }

static void test_frame(void)
{
    log_line_t lines[] = {
        L(10, "", "I (1) skipped: not wanted"),
        L(11, "", "EVT link.connect int_free=5"),
        L(12, "AbC", "I (2) audio: tagged \"quoted\"\nline"),
        L(13, NULL, "EVT TL int=1"),
        L(14, "", "E (3) x: boom"),
    };
    log_frame_opts_t o = {.boot = 42, .up_ms = 99000, .max_lines = 48};
    uint32_t cur = 0;
    cJSON *f = log_upload_frame(lines, 5, &o, 15, &cur);
    assert(f && cur == 15);
    char *s = cJSON_PrintUnformatted(f);
    assert(!strcmp(s, "{\"t\":\"log\",\"boot\":42,\"up\":99000,\"lines\":["
                      "[11,110,\"\",\"EVT link.connect int_free=5\"],"
                      "[12,120,\"AbC\",\"I (2) audio: tagged \\\"quoted\\\"\\nline\"],"
                      "[14,140,\"\",\"E (3) x: boom\"]]}"));
    free(s);
    /* JSON escaping round-trips */
    cJSON *back = cJSON_Parse(cJSON_PrintUnformatted(f));
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(back, "lines"), 1), 3)->valuestring,
                   "I (2) audio: tagged \"quoted\"\nline"));
    cJSON_Delete(back);
    cJSON_Delete(f);
    /* from a cursor: older lines are not sent again */
    cur = 13;
    f = log_upload_frame(lines, 5, &o, 15, &cur);
    assert(f && cur == 15 && cJSON_GetArraySize(cJSON_GetObjectItem(f, "lines")) == 1);
    cJSON_Delete(f);
    /* nothing wanted: NULL, the cursor at the end */
    cur = 0;
    assert(!log_upload_frame(lines, 1, &o, 11, &cur) && cur == 11);
    cur = 15;
    assert(!log_upload_frame(lines, 5, &o, 15, &cur) && cur == 15);
    /* previous boot, with and without its sync */
    o.prev = true; o.has_sync = true; o.sync_up_ms = 5000; o.sync_wall_ms = 1790000000123LL;
    cur = 0;
    f = log_upload_frame(lines, 5, &o, 15, &cur);
    s = cJSON_PrintUnformatted(f);
    assert(strstr(s, "\"prev\":true,\"sync\":[5000,1790000000123],\"lines\":"));
    free(s); cJSON_Delete(f);
    o.has_sync = false; cur = 0;
    f = log_upload_frame(lines, 5, &o, 15, &cur);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(f, "prev")) && !cJSON_GetObjectItem(f, "sync"));
    cJSON_Delete(f);
}

static void test_caps(void)
{
    static char texts[200][400];
    static log_line_t many[200];
    for (int i = 0; i < 200; i++) {
        snprintf(texts[i], sizeof(texts[i]), "EVT n=%d", i);
        many[i] = L(100 + (uint32_t)i, "", texts[i]);
    }
    /* max_lines: 3 lines, the cursor at the first left out */
    log_frame_opts_t o = {.boot = 1, .up_ms = 1, .max_lines = 3};
    uint32_t cur = 0;
    cJSON *f = log_upload_frame(many, 200, &o, 300, &cur);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(f, "lines")) == 3 && cur == 103);
    cJSON_Delete(f);
    /* the next frame starts there */
    f = log_upload_frame(many, 200, &o, 300, &cur);
    assert(cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(f, "lines"), 0), 0)->valueint == 103 && cur == 106);
    cJSON_Delete(f);
    /* clamped: 0 -> 1, 1000 -> LOG_UPLOAD_LINES_MAX */
    o.max_lines = 0; cur = 0;
    f = log_upload_frame(many, 200, &o, 300, &cur);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(f, "lines")) == 1);
    cJSON_Delete(f);
    o.max_lines = 1000; cur = 0;
    f = log_upload_frame(many, 200, &o, 300, &cur);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(f, "lines")) == LOG_UPLOAD_LINES_MAX && cur == 100 + LOG_UPLOAD_LINES_MAX);
    cJSON_Delete(f);
    /* the byte cap: long, escape-heavy lines stop the frame under LOG_UPLOAD_FRAME_MAX */
    for (int i = 0; i < 200; i++) {
        memset(texts[i], '"', 399); texts[i][399] = 0;
        memcpy(texts[i], "EVT ", 4);
        many[i].job = "0123456789abcdef0123456789abcdef";
    }
    cur = 0;
    f = log_upload_frame(many, 200, &o, 300, &cur);
    int n = cJSON_GetArraySize(cJSON_GetObjectItem(f, "lines"));
    char *s = cJSON_PrintUnformatted(f);
    assert(n > 0 && n < LOG_UPLOAD_LINES_MAX && strlen(s) <= LOG_UPLOAD_FRAME_MAX && cur == 100 + (uint32_t)n);
    /* text clipped to LOG_UPLOAD_TEXT_MAX */
    assert(strlen(cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(f, "lines"), 0), 3)->valuestring) == LOG_UPLOAD_TEXT_MAX);
    free(s); cJSON_Delete(f);
    /* never mid UTF-8 sequence */
    char utf[160];
    memset(utf, 'a', 149); memcpy(utf + 149, "\xc3\xa9zz", 5);   /* é straddles byte 150 */
    memcpy(utf, "EVT ", 4);
    log_line_t one = L(1, "", utf);
    cur = 0; o.max_lines = 5;
    f = log_upload_frame(&one, 1, &o, 2, &cur);
    assert(strlen(cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(f, "lines"), 0), 3)->valuestring) == 149);
    cJSON_Delete(f);
}

static void test_backlog(void)
{
    const uint32_t seqs[] = {1, 3, 5, 7, 9};
    assert(log_upload_backlog(seqs, 5, 0, 2) == 7);    /* keep the newest 2 */
    assert(log_upload_backlog(seqs, 5, 0, 10) == 0);   /* all fit */
    assert(log_upload_backlog(seqs, 5, 6, 1) == 9);
    assert(log_upload_backlog(seqs, 5, 8, 5) == 8);    /* never moves back */
    assert(log_upload_backlog(seqs, 0, 4, 1) == 4);
}

static void test_persist(void)
{
    const int64_t S = 1000000;
    /* never in the first 90 s */
    assert(log_persist_delay_us(10 * S, 0) == 80 * S);
    assert(log_persist_delay_us(90 * S, 0) == 0);
    assert(log_persist_delay_us(100 * S, 0) == 0);
    /* at least 5 min apart */
    assert(log_persist_delay_us(100 * S, 50 * S) == 250 * S);
    assert(log_persist_delay_us(350 * S, 50 * S) == 0);
    assert(log_persist_delay_us(400 * S, 50 * S) == 0);
    /* an error writes; the periodic tick only with new uploadable lines */
    assert(log_persist_due(true, 0));
    assert(!log_persist_due(false, 0));
    assert(log_persist_due(false, 3));
    assert(LOG_PERSIST_PERIOD_MS == 30 * 60 * 1000 && LOG_PERSIST_BYTES == 12 * 1024);

    char buf[256];
    uint32_t boot, up; int64_t wall;
    assert(log_persist_header(buf, sizeof(buf), 42, 5000, 1790000000123LL) > 0);
    assert(!strcmp(buf, "#molty-log 2 boot=42 sync_up=5000 sync_wall=1790000000123"));
    assert(log_persist_parse_header(buf, &boot, &up, &wall) && boot == 42 && up == 5000 && wall == 1790000000123LL);
    assert(log_persist_parse_header("#molty-log 2 boot=7 sync_up=0 sync_wall=0", &boot, &up, &wall) && boot == 7 && !wall);
    assert(!log_persist_parse_header("12 3.456 an old-format line", &boot, &up, &wall));
    assert(!log_persist_parse_header("#molty-log 1 boot=1", &boot, &up, &wall));

    log_line_t l = L(77, "AbC", "EVT audio.say end  two  spaces");
    assert(log_persist_line(buf, sizeof(buf), &l) > 0 && !strcmp(buf, "77 770 AbC EVT audio.say end  two  spaces"));
    log_line_t back;
    assert(log_persist_parse_line(buf, &back) && back.seq == 77 && back.ms == 770 && !strcmp(back.job, "AbC") &&
           !strcmp(back.text, "EVT audio.say end  two  spaces"));
    l = L(78, "", "E (1) x: multi\nline");
    assert(log_persist_line(buf, sizeof(buf), &l) > 0 && !strcmp(buf, "78 780 - E (1) x: multi line"));
    assert(log_persist_parse_line(buf, &back) && !strcmp(back.job, "") && !strcmp(back.text, "E (1) x: multi line"));
    assert(log_persist_line(buf, 8, &l) == -1);                     /* does not fit */
    char old[] = "12 3.456 an old-format line";
    assert(!log_persist_parse_line(old, &back));
    char junk[] = "x y z";
    assert(!log_persist_parse_line(junk, &back));
}

int main(void)
{
    test_job_tag();
    test_jobs();
    test_filter();
    test_frame();
    test_caps();
    test_backlog();
    test_persist();
    puts("LOG UPLOAD PASS: job tags (ids, begin/end, overlap, refcount), upload filter (E/W, colour, EVT TL vs TL!, tagged), frames (fields, order, prev+sync, escaping, cursor), caps (max_lines, clamp, 12 KB, 150-char clip, UTF-8), backlog bound, persisted format + write policy");
    return 0;
}
