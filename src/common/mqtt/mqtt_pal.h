#pragma once

#include <stdint.h>
#include <stddef.h>

#include "lwip/def.h"
#include "lwip/sys.h"

#ifndef _SSIZE_T_DEFINED
typedef int ssize_t;
#define _SSIZE_T_DEFINED
#endif

typedef uint32_t mqtt_pal_time_t;
typedef void *mqtt_pal_socket_handle;
typedef int mqtt_pal_mutex_t;

#ifdef __cplusplus
extern "C" {
#endif
ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t len, int flags);
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf, size_t bufsz, int flags);
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
