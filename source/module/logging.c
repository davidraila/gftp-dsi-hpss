/*
 * University of Illinois/NCSA Open Source License
 *
 * Copyright © 2017 NCSA.  All rights reserved.
 *
 * Developed by: David Raila raila@illinois.edu
 *
 * Storage Enabling Technologies (SET) Group
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

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "logging.h"


int dsi_loglevel = 0;

void dsi_setLogLevel(int level){
  dsi_loglevel = level;
}

int dsi_isLogLevel(int level){
  return level <= dsi_loglevel;
}

void backTrace() {
  const unsigned MAX_FRAMES=256;
  const unsigned MAX_FRAME_STRBUF=MAX_FRAMES*256; 
  void *p_frames[MAX_FRAMES];
  char bt_storage[MAX_FRAME_STRBUF];

  size_t num_frames = backtrace(p_frames, MAX_FRAMES);
  char ** bt_strings = backtrace_symbols(p_frames, num_frames);
  char *cur = bt_storage;
  for (int i = 0; i < num_frames; ++i)
    cur += sprintf(cur, "%s\n", bt_strings[i]);
  DEBUG(": %s", bt_storage);
}

static int sighit = 0;
// Just print backtrace and continue
void fault_handler(int sig) {
  sighit++;
  return;
  DEBUG(": handling");
  const unsigned MAX_FRAMES=256;
  const unsigned MAX_FRAME_STRBUF=MAX_FRAMES*256; 
  void *p_frames[MAX_FRAMES];
  char bt_storage[MAX_FRAME_STRBUF];

  size_t num_frames = backtrace(p_frames, MAX_FRAMES);
  char ** bt_strings = backtrace_symbols(p_frames, num_frames);
  char *cur = bt_storage;
  for (int i = 0; i < num_frames; ++i)
    cur += sprintf(cur, "%s\n", bt_strings[i]);
  DEBUG(": %s %s", strsignal(sig), bt_storage);
}

int sigcatch(int sig, void** pp_oldhandler, void** pp_oldset, int *hit){
  DEBUG(": setting handler sighit: %d", sighit);
  sigset_t newset, oldset;
  sigemptyset(&newset);
  sigemptyset(&oldset);
  sigaddset(&newset, SIGSEGV);
   sigaddset(&newset, SIGABRT);
  if (sigprocmask(SIG_UNBLOCK, &newset, &oldset))
    return -1;
    
  void * oldhandler = signal(sig, fault_handler);
  signal(SIGABRT, fault_handler);
  if (oldhandler == SIG_ERR){
    ERR(": signal(%d) failed", sig);
    sigprocmask(SIG_SETMASK, &oldset, NULL);
    return -1;
  }
  *pp_oldhandler = malloc(sizeof(void*));
  memcpy(*pp_oldhandler, &oldhandler, sizeof(oldhandler));
  *pp_oldset = malloc(sizeof(sigset_t));
  memcpy(*pp_oldset, &oldset, sizeof(oldset));
  DEBUG(": handler set");
  return 0;
}

void sigreset(int sig, void ** pp_oldhandler, void** pp_oldset, int *hit){
  DEBUG(": resetting handler");
  int rc = sigprocmask(SIG_SETMASK, *pp_oldset, NULL);
  if (rc) DEBUG(": restore failed");
  signal(sig, *pp_oldhandler);
  free(pp_oldhandler);
  free(pp_oldset);
  *hit = sighit;
  DEBUG(": handler reset");
}

