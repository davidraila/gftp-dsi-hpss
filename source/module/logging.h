/*
 * University of Illinois/NCSA Open Source License
 *
 * Copyright © 2017 NCSA.  All rights reserved.
 *
 * Author:  David Raila, raila@illinois.edu
 *
 * Storage Enabling Technologies (SET)
 *
 * Nation Center for Supercomputing Applications (NCSA)
 *
 * http://www.ncsa.illinois.edu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the .Software.),
 * to deal with the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 *    + Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimers.
 *
 *    + Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimers in the
 *      documentation and/or other materials provided with the distribution.
 *
 *    + Neither the names of SET, NCSA
 *      nor the names of its contributors may be used to endorse or promote
 *      products derived from this Software without specific prior written
 *      permission.
 *
 * THE SOFTWARE IS PROVIDED .AS IS., WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS WITH THE SOFTWARE.
 */
#ifndef LOGSUPPORT_h
#define LOGSUPPORT_h

#define DSI_SYSLOG // should be added to autoconf and/or cmdline flags

#include <syslog.h>
#include <unistd.h>
#include "dmalloc.h"

void dsi_setLogLevel(int level);
int dsi_isLogLevel(int level);
void backTrace();

//
// User interfaces
//
#define EMERG(format, args...) SYSLOG(LOG_EMERG, __FILE__, __LINE__, "", format, ##args)
#define ALERT(format, args...) SYSLOG(LOG_ALERT, __FILE__, __LINE__, "", format, ##args)
#define CRIT(format, args...) SYSLOG(LOG_CRIT, __FILE__, __LINE__, "", format, ##args)
#define ERR(format, args...) SYSLOG(LOG_ERR, __FILE__, __LINE__, "", format, ##args)
#define WARNING(format, args...) SYSLOG(LOG_WARNING, __FILE__, __LINE__, "", format, ##args)
#define NOTICE(format, args...) SYSLOG(LOG_NOTICE, __FILE__, __LINE__, "", format, ##args)
#define INFO(format, args...) SYSLOG(LOG_INFO, __FILE__, __LINE__, "", format, ##args)
#define DEBUG(format, args...) SYSLOG(LOG_DEBUG, __FILE__, __LINE__, "", format, ##args)

//
// Assembles a vsprintf style args "format", args...
//
#define LOGVARGS(file, line, prefix, format, args...) "dsi[%s:%d]%s" prefix format, file, line, __func__, ##args
//
// Send the LOGVARGS to syslog at the passed level
//

#ifdef DSI_SYSLOG
#define SYSLOG(level, file, line, prefix, format, args...)  { if (dsi_isLogLevel(level)) syslog(LOG_ALERT, LOGVARGS(file, line, prefix, format, ##args));}
#else
#define SYSLOG(level, file, line, prefix, format, args...)
#endif
//
// Send the LOGVARGS to printf
//
#define STDLOG(level, file, line, prefix, format, args...) {printf( LOGVARGS(file, line, prefix, format, ##args));}

#endif // LOGSUPPORT_H