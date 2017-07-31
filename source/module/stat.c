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
#include "logging.h"

int stat_hpss_lstat(char*p, hpss_stat_t* buf){
  int ret; 
  if((ret = hpss_Lstat(p, buf)) < 0) {
    //INFO ERR("(%s) failed: code %d: %s", p, ret, strerror(errno));
    return ret;
  }
 return 0;
}

int stat_hpss_stat(char*p, hpss_stat_t* buf){
  int ret; 
  if((ret = hpss_Stat(p, buf)) < 0) {
    //INFO ERR("(%s) failed: code %d: %s", p, ret, strerror(errno));
    return ret;
  }
 return 0;
}

int stat_hpss_dirent_count(char *p, hpss_fileattr_t *dir_attrs) {
  int ret;
  if ((ret = hpss_FileGetAttributes(p, dir_attrs)) < 0) {
    ERR("(%s): hpss_FileGetAttributes failed", p);
    return -1;
  }
  int numdents = dir_attrs->Attrs.EntryCount ;
  DEBUG(": %d dir_attrs", numdents);
  return numdents;
}

int stat_hpss_getdents(ns_ObjHandle_t *ObjHandle, ns_DirEntry_t *hdents, int num_hdents, uint64_t *dir_offset, uint32_t *end){
  DEBUG(": read %d at dir offset %ld", num_hdents, *dir_offset);
  int ret;
  uint64_t bufsz = sizeof(ns_DirEntry_t)*num_hdents;
  if ((ret = hpss_ReadAttrsHandle(ObjHandle, *dir_offset, NULL, bufsz,
                              TRUE, end, dir_offset, hdents)) < 0) {
    ERR(": hpss_ReadAttrsHandle failed: code %d: %s, return", ret, strerror(errno));
    return -1;
  } 
  DEBUG(": return %d", ret);
  return ret;
}

globus_result_t stat_translate_stat(char *Pathname, hpss_stat_t *HpssStat,
                                    globus_gfs_stat_t *GFSStat, char *name_storage, char *symlink_storage ) {
  DEBUG("(%s): hpss_stat(mode(%d), nlink(%d), uid(%d), gid(%d), size(%ld)", 
    Pathname, HpssStat->st_mode, HpssStat->st_nlink, HpssStat->st_uid, HpssStat->st_gid, HpssStat->st_size);
  GlobusGFSName(stat_translate_stat);

  /* If it is a symbolic link... */
  char symlink_target[HPSS_MAX_PATH_NAME];      // possibly relative link target
  if (S_ISLNK(HpssStat->st_mode)) {
    DEBUG(": is a link");
    
    //char symlink_full_target[HPSS_MAX_PATH_NAME]; // full-path link target
    /* Read the target. */  
    int retval;
    if ((retval = hpss_Readlink(Pathname, symlink_target, sizeof(symlink_target))) < 0) {
      ERR(": hpss_Readlink failed: code %d, path(%s), return", retval, Pathname);
      //stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_Readlink", -retval);
      }
      DEBUG(": target %s", symlink_target);
    if ((retval = stat_hpss_stat(Pathname, HpssStat)) < 0) {
      ERR(": hpss_lstat(%s) failed: code %d", Pathname, retval);
      //stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_Lstat target", -retval);
    }

    //snprintf(full_target, HPSS_MAX_PATH_NAME, "%s/%s",  Pathname, symlink_target);
    GFSStat->symlink_target = symlink_storage;
    strcpy(symlink_storage, symlink_target);
  #ifdef NO
    if ((GFSStat->symlink_target = globus_libc_strdup(symlink_target)) == NULL) {
    ERR(": globus_libc_strdup(%s) failed: code %d, return", symlink_target, retval);
    stat_destroy(GFSStat);
    return GlobusGFSErrorMemory("SymlinkTarget");
    }
  #endif
  }
  int ret = stat_translate_lstat(Pathname, HpssStat, GFSStat, name_storage);
  DEBUG(": return(%d), name:%s, link:%s", ret, GFSStat->name, GFSStat->symlink_target);
  return ret;
}

globus_result_t stat_translate_lstat(char *p, hpss_stat_t *HpssStat,
                                    globus_gfs_stat_t *GFSStat, char *name_storage)
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
  strcpy(name_storage, basename ? (basename + 1) : p);
  GFSStat->name =  name_storage;
#ifdef NO
  if (!GFSStat->name) {
    stat_destroy(GFSStat);
    ERR(": globus_libc_strdup failed, return");
    return GlobusGFSErrorMemory("GFSStat->name");
  }
#endif

  DEBUG("(%s): success, name is %s", p, GFSStat->name);
  return GLOBUS_SUCCESS;
}

/*
 * It is possible to have a broken symbolic link and we don't want
 * that to cause complex operations to fail. So, we'll return
 * symlink information if the link is broken.
 */
globus_result_t stat_object(char *Pathname, globus_gfs_stat_t *GFSStat, char* gname_storage, char *glink_storage) {
  GlobusGFSName(stat_object);

  memset(GFSStat, 0, sizeof(globus_gfs_stat_t));

  hpss_stat_t hpss_stat_buf;
  int retval = hpss_Lstat(Pathname, &hpss_stat_buf);
  if (retval)
    return GlobusGFSErrorSystemError("hpss_Lstat", -retval);

  if (S_ISLNK(hpss_stat_buf.st_mode)) {
    retval = hpss_Stat(Pathname, &hpss_stat_buf);
    if (retval && retval != -ENOENT)
      return GlobusGFSErrorSystemError("hpss_Stat", -retval);
  }

  return stat_translate_stat(Pathname, &hpss_stat_buf, GFSStat, gname_storage, glink_storage);
} 



globus_result_t stat_translate_dir_entry(ns_ObjHandle_t *ParentObjHandle,
                                         ns_DirEntry_t *DirEntry,
                                         globus_gfs_stat_t *GFSStat, char* dir_path, char *name_storage, char *symlink_storage) {
  DEBUG("(%s)",  DirEntry->Name);
  GlobusGFSName(stat_translate_dir_entry);

  /* If it is a symbolic link... */
  if (DirEntry->Attrs.Type == NS_OBJECT_TYPE_SYM_LINK) {
    DEBUG(": isALink(%s): get target",  DirEntry->Name);
    int ret;
    hpss_stat_t tattr = {0};
    /* Read the target. */
    char link_target[HPSS_MAX_PATH_NAME];
    if((ret = hpss_ReadlinkHandle(ParentObjHandle, DirEntry->Name, 
        link_target, HPSS_MAX_PATH_NAME, NULL)) < 0) {
      ERR("(%s): hpssReaLinkHandle failed: code %d return",  DirEntry->Name, ret);
      //stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_ReadlinkHandle", -ret);
    }
    char stat_target[HPSS_MAX_PATH_NAME];
    snprintf(stat_target, HPSS_MAX_PATH_NAME, "%s/%s", dir_path, DirEntry->Name);
    if((ret = stat_hpss_stat(stat_target, &tattr)) < 0) {
      ERR(": stat_hpss_stat(%s) failed: code %d, return", stat_target, ret);
      //stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_ReadlinkHandle", -ret);
    }
    if ((ret = stat_translate_lstat(DirEntry->Name, &tattr, GFSStat, name_storage)) < 0) {
      ERR(": stat(%s) failed: code %d, return",  DirEntry->Name, ret);
      //stat_destroy(GFSStat);
      return GlobusGFSErrorSystemError("hpss_ReadlinkHandle", -ret);
    }
    strcpy(symlink_storage, link_target);
    GFSStat->symlink_target = symlink_storage;
#ifdef NO
    if ((GFSStat->symlink_target = globus_libc_strdup(symlink_target)) == NULL) {
      ERR(": globus_libc_strdup failed, return");
      //stat_destroy(GFSStat);
      return GlobusGFSErrorMemory("strdup SymlinkTarget");
    }
#endif
     DEBUG("(%s): target: %s: done", DirEntry->Name, symlink_storage);
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

  strcpy(name_storage, DirEntry->Name);
  GFSStat->name = name_storage;
#ifdef NO
  if (!(GFSStat->name = globus_libc_strdup(DirEntry->Name))) {
    ERR("(%s): strdup failed",  DirEntry->Name);
    return GlobusGFSErrorMemory("GFSStat->name");
  }
#endif
  DEBUG("(%s): success: mode(%d), nlink(%d), link(%s), uid(%d), gid(%d), size(%ld), ... ", 
    GFSStat->name, GFSStat->mode, GFSStat->nlink, GFSStat->symlink_target, GFSStat->uid, GFSStat->gid, GFSStat->size);
  return GLOBUS_SUCCESS;
}

#ifdef NO
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
#endif
