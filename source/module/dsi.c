/*
 * University of Illinois/NCSA Open Source License
 *
 * Copyright © 2012-2014 NCSA.  All rights reserved.
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
#include "logsupport.h"
#include "markers.h"
#include "retr.h"
#include "stat.h"
#include "stor.h"

static void dsi_init(globus_gfs_operation_t Operation,
              globus_gfs_session_info_t *SessionInfo) {
  globus_result_t result = GLOBUS_SUCCESS;
  config_t *config = NULL;
  char *home = NULL;
  sec_cred_t user_cred;

  GlobusGFSName(dsi_init);
  INFO();

  /*
   * Read in the config.
   */
  result = config_init(&config);
  if (result) {
    ERR("config_init failed");
    goto cleanup;
  }

  /* Now authenticate. */
  result = authenticate(config->LoginName, config->AuthenticationMech,
                        config->Authenticator, SessionInfo->username);
  if (result != GLOBUS_SUCCESS) {
    ERR("authenticate failed");
    goto cleanup;
  }

  /*
   * Pulling the HPSS directory from the user's credential will support
   * sites that use HPSS LDAP.
   */
  result = hpss_GetThreadUcred(&user_cred);
  if (result) {
    ERR("hpss_GetThreadUcred failed");
    goto cleanup;
  }

  home = strdup(user_cred.Directory);
  if (!home) {
    ERR("strdup home directory failed");
    result = GlobusGFSErrorMemory("home directory");
    goto cleanup;
  }

  result = commands_init(Operation);

cleanup:
  INFO("cleanup");

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

static void dsi_destroy(void *Arg) {
  if (Arg)
    config_destroy(Arg);
}

static int dsi_restart_transfer(globus_gfs_transfer_info_t *TransferInfo) {
  globus_off_t offset;
  globus_off_t length;
  DEBUG(": path %s type %s stripes %d depth %d",
       TransferInfo->pathname, TransferInfo->list_type,
       TransferInfo->stripe_count, TransferInfo->list_depth);

  if (globus_range_list_size(TransferInfo->range_list) != 1) {
    ERR("range_list != 1");
    return 1;
  }

  globus_range_list_at(TransferInfo->range_list, 0, &offset, &length);
  DEBUG(": return %d", (offset != 0 || length != -1));
  return (offset != 0 || length != -1);
}

static void dsi_send(globus_gfs_operation_t Operation,
              globus_gfs_transfer_info_t *TransferInfo, void *UserArg) {

  DEBUG(":npath %s", TransferInfo->pathname);
  GlobusGFSName(dsi_send);

  retr(Operation, TransferInfo);
  DEBUG(": path %s: return", TransferInfo->pathname);
}

static void dsi_recv(globus_gfs_operation_t Operation,
                     globus_gfs_transfer_info_t *TransferInfo, void *UserArg) {
  globus_result_t result = GLOBUS_SUCCESS;
  DEBUG(": path %s", TransferInfo->pathname);

  GlobusGFSName(dsi_recv);

  if (dsi_restart_transfer(TransferInfo) && !markers_restart_supported()) {
    result = GlobusGFSErrorGeneric("Restarts are not supported");
    globus_gridftp_server_finished_transfer(Operation, result);
    ERR("path %s: restart error, return", TransferInfo->pathname);
    return;
  }
  stor(Operation, TransferInfo, UserArg);
  DEBUG("(%s): returns", TransferInfo->pathname);
}

static void dsi_command(globus_gfs_operation_t Operation,
                 globus_gfs_command_info_t *CommandInfo, void *UserArg) {
  DEBUG(": command %d", CommandInfo->command);
  commands_run(Operation, CommandInfo, UserArg,
               globus_gridftp_server_finished_command);
  DEBUG(": command %d return", CommandInfo->command);
}

//
// Stat a file (StatInfo->file_only), or directory
//
// 
// Case:            StatInfo->file_only           !StatInfo->file_only
//
// directory        lstat of dir                  lstats of dirents
// file             lstat of file                 lstat of file
// link to file     lstat of target               lstat of target
// link to dir      lstat of target               lstats of dirents  
//
static void dsi_stat(globus_gfs_operation_t Operation,
              globus_gfs_stat_info_t *StatInfo, void *Arg) {
  DEBUG("(%s): file-only(%d)", StatInfo->pathname, StatInfo->file_only);
  GlobusGFSName(dsi_stat);
  globus_gfs_stat_t gstat = {0};         // gfs stat buf to send back
  hpss_stat_t hstat = {0};  // hpss stat buf for lstat
  int ret;
  char *p = StatInfo->pathname;   // the path being stat'd
  char t[HPSS_MAX_PATH_NAME];     // target (if SLINK)
  int isLink, isDir, isLinkTarget, isDirTarget;
  int fileOnly=StatInfo->file_only;
  
  // lstat, set isLink, isDir
  if ((ret = stat_hpss_lstat(p, &hstat))) {
    ERR(": hpss_Lstat(%s) failed: code %d, return", p, ret);
    globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
    return;
  }
  isLink = S_ISLNK(hstat.st_mode);
  isDir = S_ISDIR(hstat.st_mode);
  DEBUG(": lstat(%s): isDir(%d), isLink(%d)", p, isDir, isLink);

  if (fileOnly && !isDir) {  // copy out the data and return
    if ((ret = stat_translate_stat(p, &hstat, &gstat))< 0) {
      ERR("stat translation failed");
      stat_destroy(&gstat);
      globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
      return;
      }
    DEBUG(": stat(%s) returns: mode(%x), nlink(%d), uid(%d), gid(%d), ino(%d), name(%s), symlink(%s):modebit(%d)", 
      p, gstat.mode, gstat.nlink, gstat.uid, gstat.gid, gstat.ino, gstat.name, gstat.symlink_target, gstat.mode & S_IFLNK);
    globus_gridftp_server_finished_stat(Operation, ret, &gstat, 1);
    return;
    }
  
  // else: not file-only process directory
  DEBUG("(%s): stat dirents", p);
  hpss_fileattr_t dir_attrs = {0};
  int ndents = stat_hpss_dirent_count(p, &dir_attrs);
  globus_gfs_stat_t gdents[ndents];
  ns_DirEntry_t hdents[ndents];
  memset(&hdents[0], 0, sizeof(hdents));
  memset(&gdents[0], 0, sizeof(gdents));

  if ((ret = stat_hpss_getdents(&dir_attrs.ObjectHandle, hdents, ndents)) != ndents){
    ERR(": stat_hpss_getDents failed");
    stat_destroy(&gstat);
    globus_gridftp_server_finished_stat(Operation, ret, NULL, 0);
    return;
  }
  // Set the output, resolving directories
  for (int i = 0; i < ndents; i++) {
    stat_translate_dir_entry(&dir_attrs.ObjectHandle, &hdents[i], &gdents[i]);
  }

  DEBUG("dirstat: done, returning %d entries", ndents);
  for (int i=0; i <ndents;i++){
    DEBUG(": name(%s): link(%s), mode(%d), nlink(%d),  uid(%d), gid(%d), size(%ld)", 
      gdents[i].name, gdents[i].symlink_target, gdents[i].mode, gdents[i].nlink,  
      gdents[i].uid, gdents[i].gid, gdents[i].size);
  }

  globus_gridftp_server_finished_stat(Operation, 0, &gdents[0], ndents);
  stat_destroy_array(&gdents[0], ndents);
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
