#ifndef MQTTC_PAL_H
#define MQTTC_PAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "lwip/def.h"
#include "lwip/sys.h"

#ifndef _SSIZE_T_DEFINED
typedef int ssize_t;
#define _SSIZE_T_DEFINED
#endif

typedef uint32_t mqtt_pal_time_t;
typedef int mqtt_pal_mutex_t;
typedef int mqtt_pal_socket_handle;

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int mqttc_pal_last_send_errno;
extern volatile int mqttc_pal_last_recv_errno;
extern volatile int mqttc_pal_last_send_rv;
extern volatile int mqttc_pal_last_recv_rv;
extern volatile uint32_t mqttc_pal_total_sent;
extern volatile uint32_t mqttc_pal_total_recv;

#ifdef __cplusplus
}
#endif

#ifndef MQTT_PAL_HTONS
#define MQTT_PAL_HTONS(s) lwip_htons(s)
#endif
#ifndef MQTT_PAL_NTOHS
#define MQTT_PAL_NTOHS(s) lwip_ntohs(s)
#endif

#define MQTT_PAL_TIME() (sys_now() / 1000U)

#define MQTT_PAL_MUTEX_INIT(mtx_ptr) ((void)(mtx_ptr))
#define MQTT_PAL_MUTEX_LOCK(mtx_ptr) ((void)(mtx_ptr))
#define MQTT_PAL_MUTEX_UNLOCK(mtx_ptr) ((void)(mtx_ptr))

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t len, int flags);
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf, size_t bufsz, int flags);

#endif
