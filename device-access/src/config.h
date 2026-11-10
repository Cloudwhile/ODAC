#ifndef ODAC_CONFIG_H
#define ODAC_CONFIG_H
#include "odac.h"
/* Must match the address limit enforced by odac-firewall init. */
#define ODAC_UPSTREAM_MAX 16
int odac_config_load(struct odac_config *config, const char *path);
#endif
