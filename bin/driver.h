/** driver.h - Interface for RINEX file processing driver.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(DRIVER_H_aef69988_07fd_42c5_ba09_ae093fd43849)
#define DRIVER_H_aef69988_07fd_42c5_ba09_ae093fd43849

#include "rinex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

extern int verbose;

void start(void);
void process_file(struct rinex_parser *p, const char filename[]);
void finish(void);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */

#endif /* !defined(DRIVER_H_aef69988_07fd_42c5_ba09_ae093fd43849) */
