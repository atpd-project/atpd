#ifndef ATP_WIFI_H
#define ATP_WIFI_H

#include <stddef.h>

int wifi_parse_ssid(const char *status, char *ssid, size_t size);
int wifi_get_ssid(char *ssid, size_t size);

#endif
