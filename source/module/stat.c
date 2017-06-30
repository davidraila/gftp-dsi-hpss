/*
 * University of Illinois/NCSA Open Source License
 *
 * Copyright © 2015 NCSA.  All rights reserved.
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
 * Globus includes
 */
#include <globus_gridftp_server.h>

/*
 * HPSS includes
 */
#include <hpss_api.h>
#include <hpss_stat.h>
#include <u_signed64.h>

/*
 * Local includes
 */
#include "stat.h"
#include "logsupport.h"

int stat_hpss_lstat(char*p, hpss_stat_t* buf){
  int ret; 
  if((ret = hpss_Lstat(p, buf)) < 0) {
    ERR(": stat_lstat(%s) failed: code %d", p, ret);
    return ret;
  }
 return 0;
}

int stat_hpss_stat(char*p, hpss_stat_t* buf){
  int ret; 
  if((ret = hpss_Stat(p, buf)) < 0) {
    ERR(": stat_Stat(%s) failed: code %d", p, ret);
    return ret;
  }
 return 0;
}

int stat_hpss_dirent_count(char *p, hpss_fileattr_t *dir_attrs) {
  int ret;
  if ((ret = hpss_FileGetAttributes(p, dir_attrs)) < 0) {
    return -1;
  }
  int numdents = dir_attrs->Attrs.EntryCount ;
  DEBUG(": %d dir_attrs", numdents);
  return numdents;
}

int stat_hpss_getdents(ns_ObjHandle_t *ObjHandle, ns_DirEntry_t *hdents, int cnt){
  int ret;
  uint32_t end = FALSE;
  uint64_t count_out; // not used, just match api
  if((ret = hpss_ReadAttrsHandle(ObjHandle, 0, NULL, sizeof(ns_DirEntry_t)*cnt,
                              TRUE, &end, &count_out, hdents)) < 0) {
    return -1;
  }
  return count_out;
}

globus_result_t stat_translate_stat(char *Pathname, hpss_stat_t *HpssStat,
                                    globus_gfs_stat_t *GFSStat) {
  DEBUG(": stat_translate_stat: path (%s), hpss_stat(mode(%d), nlink(%d), uid(%d), gid(%d), size(%ld)", 
    Pathname, HpssStat->st_mode, HpssStat->st_nlink, HpssStat->st_uid, HpssStat->st_gid, HpssStat->st_size);
  GlobusGFSName(stat_translate_stat);

  /* If it is a symbolic link... */
  if (S_ISLNK(HpssStat->st_mode)) {
    DEBUG(": is a link");
    char symlink_target[HPSS_MAX_PATH_NAME];
    /* Read the target. */
    int retval;
    if ((retval = hpss_Readlink(Pathname, symlink_target, sizeof(symlink_target))) < 0) {
      ERR(": hpss_Readlink failed: code %d, path(%s), return", retval, Pathname);
      stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_Readlink", -retval);
      }
    if ((retval = stat_hpss_lstat(symlink_target, HpssStat)) < 0) {
      ERR(": hpss_lstat(%s) failed: code %d", symlink_target, retval);
      stat_destroy(GFSStat);
      return GlobusGFSErrorMemory("SymlinkTarget");
    }
    if ((GFSStat->symlink_target = globus_libc_strdup(symlink_target)) == NULL) {
      ERR(": globus_libc_strdup(%s) failed: code %d, return", symlink_target, retval);
      stat_destroy(GFSStat);
      return GlobusGFSErrorMemory("SymlinkTarget");
    }
  }
  return stat_translate_lstat(Pathname, HpssStat, GFSStat);
}

globus_result_t stat_translate_lstat(char *p, hpss_stat_t *HpssStat,
                                    globus_gfs_stat_t *GFSStat)
{
  DEBUG("(%s): hpss_stat(mode(%d), nlink(%d), uid(%d), gid(%d), size(%ld)", 
    p, HpssStat->st_mode, HpssStat->st_nlink, HpssStat->st_uid, HpssStat->st_gid, HpssStat->st_size);
  GlobusGFSName(stat_translate_stat);

  GFSStat->mode = HpssStat->st_mode;
  GFSStat->nlink = HpssStat->st_nlink;
  GFSStat->uid = HpssStat->st_uid;
  GFSStat->gid = HpssStat->st_gid;
  GFSStat->dev = 0;
  GFSStat->atime = HpssStat->hpss_st_atime;
  GFSStat->mtime = HpssStat->hpss_st_mtime;
  GFSStat->ctime = HpssStat->hpss_st_ctime;
  GFSStat->ino = cast32m(HpssStat->st_ino);
  CONVERT_U64_TO_LONGLONG(HpssStat->st_size, GFSStat->size);

  /* Copy out the base name. */
  char *basename = strrchr(p, '/');
  char *namecopy =  basename ? globus_libc_strdup(basename + 1) : globus_libc_strdup(p);
  GFSStat->name =  namecopy;
  if (!GFSStat->name) {
    stat_destroy(GFSStat);
    ERR(": globus_libc_strdup failed, return");
    return GlobusGFSErrorMemory("GFSStat->name");
  }

  DEBUG("(%s): success", p);
  return GLOBUS_SUCCESS;
}

// Stat target, follow links
globus_result_t stat_target(char *Pathname, globus_gfs_stat_t *GFSStat) {
  DEBUG("(%s)",  Pathname);
  GlobusGFSName(stat_object);

  hpss_stat_t hpss_stat_buf;
  int retval = hpss_Stat(Pathname, &hpss_stat_buf);
  if (retval) {
    ERR(": hpss_Stat(%s) failed, return",  Pathname);
    return GlobusGFSErrorSystemError("hpss_Lstat", -retval);
  }
  if ((retval = stat_translate_stat(Pathname, &hpss_stat_buf, GFSStat)))
    DEBUG(": stat_translate_stat(%s) failed: code %d, ",  Pathname, retval);
  return retval;
}

// stat
globus_result_t stat_link(char *Pathname, globus_gfs_stat_t *GFSStat) {
  GlobusGFSName(stat_link);
  DEBUG("(%s)",  Pathname);

  hpss_stat_t hpss_stat_buf;
  int retval;
  if ((retval = stat_hpss_lstat(Pathname, &hpss_stat_buf))) {
    ERR(": stat_hpss_lstat(%s) failed: code %d, return",  Pathname, retval);
    return GlobusGFSErrorSystemError("hpss_Lstat", -retval);
  }
  if (S_ISLNK(hpss_stat_buf.st_mode)) {
    char symlink_target[HPSS_MAX_PATH_NAME];
    int retval = hpss_Readlink(Pathname, symlink_target, sizeof(symlink_target));
    if (retval < 0) {
      ERR(": hpss_Readlink(%s) failed: code %d, return", Pathname, retval);
      stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_Readlink", -retval);
    }
    /* Copy out the symlink target. */
    GFSStat->symlink_target = globus_libc_strdup(symlink_target);
    if (GFSStat->symlink_target == NULL) {
      stat_destroy(GFSStat);
      ERR(": globus_libc_strdup failed, return");
      return GlobusGFSErrorMemory("SymlinkTarget");
    }
    // fill in target info
    stat_target(symlink_target, GFSStat);
  }
  DEBUG("(%s): return %d",  Pathname, retval);
  return stat_translate_stat(Pathname, &hpss_stat_buf, GFSStat);
}

globus_result_t stat_translate_dir_entry(ns_ObjHandle_t *ParentObjHandle,
                                         ns_DirEntry_t *DirEntry,
                                         globus_gfs_stat_t *GFSStat) {
  INFO("(%s)",  DirEntry->Name);
  GlobusGFSName(stat_translate_dir_entry);

  /* If it is a symbolic link... */
  if (DirEntry->Attrs.Type == NS_OBJECT_TYPE_SYM_LINK) {
    DEBUG(": isALink(%s): get target",  DirEntry->Name);
    char symlink_target[HPSS_MAX_PATH_NAME];
    int ret;
    hpss_stat_t tattr = {0};
    /* Read the target. */
    if((ret = hpss_ReadlinkHandle(ParentObjHandle, DirEntry->Name, 
        symlink_target, HPSS_MAX_PATH_NAME, NULL)) < 0) {
      ERR("(%s): hpssReaLinkHandle failed: code %d return",  DirEntry->Name, ret);
      stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_ReadlinkHandle", -ret);
    }
    DEBUG("(%s): target: %s", DirEntry->Name, symlink_target);
    if((ret = stat_hpss_lstat(symlink_target, &tattr)) < 0) {
      ERR("lstat(%s) failed, return", symlink_target);
      stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_ReadlinkHandle", -ret);
    }
    if ((ret = stat_translate_lstat(DirEntry->Name, &tattr, GFSStat)) < 0) {
      ERR(": stat(%s) failed: code %d, return",  DirEntry->Name, ret);
      stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_ReadlinkHandle", -ret);
    }
    /* Copy out the symlink target. */
    if ((GFSStat->symlink_target = globus_libc_strdup(symlink_target)) == NULL) {
      ERR(": globus_libc_strdup failed, return");
      stat_destroy(GFSStat);
      return GlobusGFSErrorMemory("SymlinkTarget");
    }
     DEBUG("(%s): target: %s: done", DirEntry->Name, symlink_target);
  // return at end
  } else {
    DEBUG("(%s): use dirent data", DirEntry->Name);
    API_ConvertModeToPosixMode(&DirEntry->Attrs, (mode_t*)&GFSStat->mode);
    switch (DirEntry->Attrs.Type) {
    case NS_OBJECT_TYPE_FILE:
    case NS_OBJECT_TYPE_HARD_LINK:
      GFSStat->mode |= S_IFREG;
      break;
    case NS_OBJECT_TYPE_DIRECTORY:
    case NS_OBJECT_TYPE_JUNCTION:
    case NS_OBJECT_TYPE_FILESET_ROOT:
      GFSStat->mode |= S_IFDIR;
      break;
    case NS_OBJECT_TYPE_SYM_LINK:
      GFSStat->mode |= S_IFLNK;
      break;
    }
 
    GFSStat->nlink = DirEntry->Attrs.LinkCount;
    GFSStat->uid = DirEntry->Attrs.UID;
    GFSStat->gid = DirEntry->Attrs.GID;
    GFSStat->dev = 0;
    GFSStat->atime = DirEntry->Attrs.TimeLastRead;
    GFSStat->mtime = DirEntry->Attrs.TimeLastWritten;
    GFSStat->ctime = DirEntry->Attrs.TimeCreated;
    GFSStat->ino = 0; // XXX
    GFSStat->size = DirEntry->Attrs.DataLength;
  }

  if (!(GFSStat->name = globus_libc_strdup(DirEntry->Name))) {
    ERR("(%s): strdup failed",  DirEntry->Name);
    return GlobusGFSErrorMemory("GFSStat->name");
  }
  DEBUG("(%s): success: mode(%d), nlink(%d), link(%s), uid(%d), gid(%d), size(%ld), ... ", 
    GFSStat->name, GFSStat->mode, GFSStat->nlink, GFSStat->symlink_target, GFSStat->uid, GFSStat->gid, GFSStat->size);
  return GLOBUS_SUCCESS;
}

void stat_destroy(globus_gfs_stat_t *GFSStat) {
  if (GFSStat) {
    if (GFSStat->symlink_target != NULL)
      free(GFSStat->symlink_target);
    if (GFSStat->name != NULL)
      free(GFSStat->name);
  }
  return;
}

void stat_destroy_array(globus_gfs_stat_t *GFSStatArray, int Count) {
  int i;
  for (i = 0; i < Count; i++) {
    stat_destroy(&(GFSStatArray[i]));
  }
  return;
}
