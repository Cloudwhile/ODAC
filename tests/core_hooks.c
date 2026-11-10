#include <stddef.h>
#include <time.h>

/* Test-only stand-ins for the platform and the injected clock. The production
 * build never links this file. */
#ifdef ODAC_TEST_HOOKS
char odac_test_backup[256], odac_test_restore[256];
long long odac_test_clock = 1800000000;
#endif
