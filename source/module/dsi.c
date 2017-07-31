/*
 * University of Illinois/NCSA Open Source License
 *
 * Copyright � 2012-2014 NCSA.  All rights reserved.
 *
 * Developed by:
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

/*
 * System includes
 */
#include <string.h>

/*
 * Globus includes
 */
#include <globus_gridftp_server.h>

/*
 * HPSS includes
 */
#include <hpss_api.h>

/*
 * Local includes
 */
#include "authenticate.h"
#include "commands.h"
#include "config.h"
#include "logging.h"
#include "markers.h"
#include "retr.h"
#include "stat.h"
#include "stor.h"
#include "config.h"
#include "execinfo.h"

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

static void dsi_init(globus_gfs_operation_t Operation,
              globus_gfs_session_info_t *SessionInfo) {
  globus_result_t result = GLOBUS_SUCCESS;
  config_t *config = NULL;
  char *home = NULL;
  sec_cred_t user_cred;

  GlobusGFSName(dsi_init);
  //INFO(": %s: %s", PACKAGE_VERSION, PACKAGE_STRING);
  INFO(": NCSA DSI 2.4");

  /*
   * Read in the config.
   */
  result = config_init(&config);
  if (result) {
    ERR(": config_init failed");
    goto cleanup;
  }
  INFO(": loglevel: %d", config->LogLevel);

    // set log level
    int mask = LOG_INFO;
    if (config->LogLevel) mask = config->LogLevel;
    dsi_setLogLevel(mask);
    ALERT(": alert");
    CRIT(": crit");
    ERR(": err");
    WARNING(": warn");
    NOTICE(": notice");
    INFO(": info");
    DEBUG(": debug");

  /* Now authenticate. */
  result = authenticate(config->LoginName, config->AuthenticationMech,
                        config->Authenticator, SessionInfo->username);
  if (result != GLOBUS_SUCCESS) {
    ERR(": authentication failed");
    goto cleanup;
  }

  /*
   * Pulling the HPSS directory from the user's credential will support
   * sites that use HPSS LDAP.
   */
  result = hpss_GetThreadUcred(&user_cred);
  if (result) {
    ERR(": hpss_GetThreadUcred failed: code %d", result);
    goto cleanup;
  }

  home = strdup(user_cred.Directory);
  if (!home) {
    result = GlobusGFSErrorMemory("home directory");
    goto cleanup;
  }

  result = commands_init(Operation);

cleanup:

  /*
   * Inform the server that we are done. If we do not pass in a username, the
   * server will use the name we mapped to with GSI. If we do not pass in a
   * home directory, the server will (1) look it up if we are root or
   * (2) leave it as the unprivileged user's home directory.
   *
   * As far as I can tell, the server keeps a pointer to home_directory and
   * frees it when it is done.
   */
  globus_gridftp_server_finished_session_start(Operation, result,
                                               result ? NULL : config,
                                               NULL, // username
                                               result ? NULL : home);

  if (result)
    config_destroy(config);
  if (result && home)
    free(home);
}

void dsi_destroy(void *Arg) {
  if (Arg)
    config_destroy(Arg);
}

int dsi_restart_transfer(globus_gfs_transfer_info_t *TransferInfo) {
  globus_off_t offset;
  globus_off_t length;
  DEBUG("(%s) type %s stripes %d depth %d",
       TransferInfo->pathname, TransferInfo->list_type,
       TransferInfo->stripe_count, TransferInfo->list_depth);

  if (globus_range_list_size(TransferInfo->range_list) != 1) {
    ERR(": range_list != 1");
    return 1;
  }

  globus_range_list_at(TransferInfo->range_list, 0, &offset, &length);
  return (offset != 0 || length != -1);
}

void dsi_send(globus_gfs_operation_t Operation,
              globus_gfs_transfer_info_t *TransferInfo, void *UserArg) {

  DEBUG(":npath %s", TransferInfo->pathname);
  GlobusGFSName(dsi_send);

  retr(Operation, TransferInfo);
  DEBUG(": path %s: return", TransferInfo->pathname);
}

static void dsi_recv(globus_gfs_operation_t Operation,
                     globus_gfs_transfer_info_t *TransferInfo, void *UserArg) {
  globus_result_t result = GLOBUS_SUCCESS;
  DEBUG("(%s)", TransferInfo->pathname);

  GlobusGFSName(dsi_recv);

  if (dsi_restart_transfer(TransferInfo) && !markers_restart_supported()) {
    result = GlobusGFSErrorGeneric("Restarts are not supported");
    globus_gridftp_server_finished_transfer(Operation, result);
    ERR("(%s): dsi_restart_transfer failed", TransferInfo->pathname);
    return;
  }
  stor(Operation, TransferInfo, UserArg);
  DEBUG("(%s): returns", TransferInfo->pathname);
}

void dsi_command(globus_gfs_operation_t Operation,
                 globus_gfs_command_info_t *CommandInfo, void *UserArg) {
  commands_run(Operation, CommandInfo, UserArg,
               globus_gridftp_server_finished_command);
}

void dsi_stat(globus_gfs_operation_t Operation,
              globus_gfs_stat_info_t *StatInfo, void *Arg) {
  DEBUG("(%s): file-only(%d)", StatInfo->pathname, StatInfo->file_only);
  GlobusGFSName(dsi_stat);
  globus_gfs_stat_t gstat = {0};         // gfs stat buf to send back
  char gstat_name[HPSS_MAX_PATH_NAME];
  char gstat_link_name[HPSS_MAX_PATH_NAME];
  hpss_stat_t hstat = {0};  // hpss stat buf for lstat
  int ret;
  char *p = StatInfo->pathname;   // the path being stat'd
  int fileOnly=StatInfo->file_only;
  
  // lstat, set isLink, isDir
  if ((ret = stat_hpss_lstat(p, &hstat))) {
    // not really error // ERR(": hpss_Lstat(%s) failed: code %d: %s return", p, ret, strerror(errno));
    ret = GlobusGFSErrorSystemError(strerror(errno), -ret);
    globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
    return;
  }
  int isLink = S_ISLNK(hstat.st_mode);
  int isDir = S_ISDIR(hstat.st_mode);
  DEBUG("(%s): isDir(%d), isLink(%d)", p, isDir, isLink);

  if (fileOnly || (!isDir && !isLink)) {  // copy out the data and return
    if ((ret = stat_translate_stat(p, &hstat, &gstat, gstat_name, gstat_link_name))< 0) {
      ERR(": stat translate_stat(%s) failed: code %d, return", p, ret);
      ret = GlobusGFSErrorSystemError(__func__, -ret);
      //NOstat_destroy(&gstat);
      globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
      return;
      }
    DEBUG(": stat(%s) returns: mode(%x), nlink(%d), uid(%d), gid(%d), ino(%d), name(%s), symlink(%s):modebit(%d)", 
      p, gstat.mode, gstat.nlink, gstat.uid, gstat.gid, gstat.ino, gstat.name, gstat.symlink_target, gstat.mode & S_IFLNK);
    globus_gridftp_server_finished_stat(Operation, ret, &gstat, 1);
    return;
    }
  
  // else: not file-only process directory
  const unsigned DEBUG_LIST_DENTS = 1;
  const unsigned MAX_DENTS_PER_MSG = 200;

  DEBUG("(%s): stat dirents", p);
  hpss_fileattr_t dir_attrs = {0};
  int ndents;
  if ((ndents = stat_hpss_dirent_count(p, &dir_attrs)) < 1) {
    ret = GlobusGFSErrorSystemError(strerror(ret), -ret);
    globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
    ERR(": no dirent count");
    return;
  }
  DEBUG("(%p): %d total entries, %d per message", p, ndents, MAX_DENTS_PER_MSG);

  // get the dir's handle
  char dir_path[HPSS_MAX_PATH_NAME];
  ns_ObjHandle_t fileset_root;
  if ((ret = hpss_GetPathHandle(&dir_attrs.ObjectHandle, &fileset_root, dir_path)) < 0){
    ERR(": hpss_GetPathHandle failed: code %d", ret);
    //stat_destroy(&gstat);
    //stat_destroy_array(gdents, ndents);//check
    ret = GlobusGFSErrorSystemError(strerror(ret), -ret);
    globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
    return;
  }
  
  uint32_t end = 0;
  unsigned offset = 0;
  uint64_t dir_offset = 0;
  // storage for results
  globus_gfs_stat_t gdents[MAX_DENTS_PER_MSG];
  char gdent_names[MAX_DENTS_PER_MSG][HPSS_MAX_PATH_NAME];
  char gdent_link_names[MAX_DENTS_PER_MSG][HPSS_MAX_PATH_NAME];
  ns_DirEntry_t hdents[MAX_DENTS_PER_MSG];
  int good_dents;
  
  while (!end) {
    memset(&hdents[0], 0, sizeof(hdents));
    memset(&gdents[0], 0, sizeof(gdents));

    DEBUG(": sizeof hdents %ld, sizeof gdents %ld, sizeof globus_gfs_stat_t %ld, sizeof ns_DirEntry_t %ld",
      sizeof(hdents), sizeof(gdents), sizeof(globus_gfs_stat_t), sizeof(ns_DirEntry_t));

    if ((ret = stat_hpss_getdents(&dir_attrs.ObjectHandle, &hdents[0], MAX_DENTS_PER_MSG, &dir_offset, &end)) < 0) {
    ERR(": stat_hpss_getDents failed: code %d", ret);
    ret = GlobusGFSErrorSystemError("getdents failed", -ret);
    break;
    }
    ndents = ret;
    DEBUG(": got %d entries at offset %d dir_off %ld", ndents, offset, dir_offset  );
    offset += ndents;

    // process hdents to gdents, resolving directories, skip errors
    good_dents = 0;
    for (int i = 0; i < ndents; i++){
      if ((ret = stat_translate_dir_entry(&dir_attrs.ObjectHandle, &hdents[i], &gdents[good_dents], 
        dir_path, gdent_names[good_dents], gdent_link_names[good_dents] )) < 0){
        DEBUG(": ignore dirent %d, %s", i, hdents[i].Name);
        } else {
        good_dents++;
      }
    }
    
    // send
    DEBUG(": sending %d direntries, end(%d)", good_dents, end);
    if(DEBUG_LIST_DENTS){
      for (int i = 0; i < good_dents; ++i){
        DEBUG(": name(%s): link(%s), mode(%d), nlink(%d),  uid(%d), gid(%d), size(%ld)", 
          gdents[i].name, gdents[i].symlink_target, gdents[i].mode, gdents[i].nlink,  
          gdents[i].uid, gdents[i].gid, gdents[i].size);
      }
    }
    if (end) {
      DEBUG(": finished, send %d", good_dents);
      globus_gridftp_server_finished_stat(Operation, GLOBUS_SUCCESS, &gdents[0], good_dents);
    } else {
      DEBUG(": partial, send %d", good_dents);
      globus_gridftp_server_finished_stat_partial(Operation, GLOBUS_SUCCESS, &gdents[0], good_dents);
    }
  }
  if (!end) {
    ERR("(%s): dir stat failed, code %d", p, ret);
    ret = GlobusGFSErrorGeneric(strerror(errno));
    globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
  }

  //stat_destroy_array(&gdents[0], ndents); // check
  DEBUG("(%s): return", StatInfo->pathname);
}

/* Can request ordered data in globus-gridftp-server 11.x, removing the need to
 * force transfers to a single stream */
#ifdef GLOBUS_GFS_DSI_DESCRIPTOR_REQUIRES_ORDERED_DATA
#define HPSS_DESC                                                              \
  GLOBUS_GFS_DSI_DESCRIPTOR_SENDER |                                           \
      GLOBUS_GFS_DSI_DESCRIPTOR_REQUIRES_ORDERED_DATA
#else
#define HPSS_DESC GLOBUS_GFS_DSI_DESCRIPTOR_SENDER
#endif

globus_gfs_storage_iface_t hpss_local_dsi_iface = {
    HPSS_DESC,   /* Descriptor       */
    dsi_init,    /* init_func        */
    dsi_destroy, /* destroy_func     */
    NULL,        /* list_func        */
    dsi_send,    /* send_func        */
    dsi_recv,    /* recv_func        */
    NULL,        /* trev_func        */
    NULL,        /* active_func      */
    NULL,        /* passive_func     */
    NULL,        /* data_destroy     */
    dsi_command, /* command_func     */
    dsi_stat,    /* stat_func        */
    NULL,        /* set_cred_func    */
    NULL,        /* buffer_send_func */
    NULL,        /* realpath_func    */
};
