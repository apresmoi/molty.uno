/* Every esp_transport_connect (the TLS handshake, ~1-3 s of CPU) runs on a short-lived task that
 * holds no mutex.
 *
 * Why: esp_websocket_client runs the handshake while its task holds client->lock (a recursive
 * mutex, true up to esp-protocols master). When tcpip_thread (prio 18) blocks briefly on lwIP's
 * protect mutex that the handshake holds, the websocket task inherits prio 18, and FreeRTOS only
 * drops an inherited priority once a task holds no mutex at all: so it kept 18 until the
 * handshake ended and froze the fluid (prio 2) on its core for ~1 s on some reconnects
 * (stall.probe sim=ready core1=websocket_task ws_prio=18). Here the handshake's own task holds
 * nothing else, so any inheritance ends with the lwIP section that caused it. The caller only
 * waits, which costs no CPU even if it is ever boosted. TLS itself is unchanged.
 *
 * Linked with -Wl,--wrap=esp_transport_connect (main/CMakeLists.txt), so esp_http_client's
 * connects (self-update) go through here too. */
#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_transport.h"

#define CONNECT_TASK  "link_connect"
#define CONNECT_PRIO  1      /* below the fluid (2), as the websocket task itself */
#define CONNECT_STACK 6144   /* as the websocket task's; PSRAM: no flash ops run on it */

int __real_esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms);

typedef struct {
    esp_transport_handle_t t; const char *host; int port, timeout_ms;
    int ret, err;
    SemaphoreHandle_t done;
} connect_job_t;

static void connect_task(void *arg)
{
    connect_job_t *j = arg;
    j->ret = __real_esp_transport_connect(j->t, j->host, j->port, j->timeout_ms);
    j->err = errno;
    xSemaphoreGive(j->done);
    vTaskSuspend(NULL);   /* the caller deletes this task */
}

int __wrap_esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    /* nested connects (ws -> ssl -> tcp) are already on the connect task */
    if (strcmp(pcTaskGetName(NULL), CONNECT_TASK) == 0) return __real_esp_transport_connect(t, host, port, timeout_ms);
    connect_job_t j = {.t = t, .host = host, .port = port, .timeout_ms = timeout_ms, .ret = -1,
                       .done = xSemaphoreCreateBinary()};
    TaskHandle_t task = NULL;
    if (!j.done || xTaskCreatePinnedToCoreWithCaps(connect_task, CONNECT_TASK, CONNECT_STACK, &j, CONNECT_PRIO, &task,
                                                   tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        if (j.done) vSemaphoreDelete(j.done);
        return __real_esp_transport_connect(t, host, port, timeout_ms);   /* no memory: connect in place */
    }
    xSemaphoreTake(j.done, portMAX_DELAY);   /* the transport bounds the connect with timeout_ms */
    vTaskDeleteWithCaps(task);
    vSemaphoreDelete(j.done);
    errno = j.err;   /* callers log errno after a failed connect */
    return j.ret;
}
