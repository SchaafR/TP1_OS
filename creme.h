#ifndef CREME_H
#define CREME_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version independante de biceps */
#define CREME_VERSION_MAJOR 1
#define CREME_VERSION_MINOR 0
#define CREME_VERSION_PATCH 0

int creme_start_server(const char *pseudo);
int creme_stop_server(void);
int creme_list_peers(void);
int creme_send_to_peer(const char *pseudo, const char *message);
int creme_send_all(const char *message);
pid_t creme_server_pid(void);
const char *creme_version_string(void);

#ifdef __cplusplus
}
#endif

#endif
