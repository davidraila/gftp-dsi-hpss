/*
 * University of Illinois/NCSA Open Source License
 *
 * Copyright � 2015 NCSA.  All rights reserved.
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
#include <assert.h>

/*
 * Local includes
 */
#include "cksm.h"
#include "pio.h"
#include "stat.h"
#include "logging.h"


int sigcatch(int sig, void** pp_oldhandler, void** pp_oldset, int*);
int sigreset(int sig, void** pp_oldhandler, void** pp_oldset, int*);

int cksm_pio_callout(char *Buffer, uint32_t *Length, uint64_t Offset,
                     void *CallbackArg);

void cksm_transfer_complete_callback(globus_result_t Result, void *UserArg);

void cksm_update_markers(cksm_marker_t *Marker, globus_off_t Bytes) {
  if (Marker) {
    pthread_mutex_lock(&Marker->Lock);
    { Marker->TotalBytes += Bytes; }
    pthread_mutex_unlock(&Marker->Lock);
  }
}

void cksm_send_markers(void *UserArg) {
  cksm_marker_t *marker = (cksm_marker_t *)UserArg;
  char total_bytes_string[128];

  pthread_mutex_lock(&marker->Lock);
  {
    /* Convert the byte count to a string. */
    sprintf(total_bytes_string, "%" GLOBUS_OFF_T_FORMAT, marker->TotalBytes);

    /* Send the intermediate response. */
    globus_gridftp_server_intermediate_command(
        marker->Operation, GLOBUS_SUCCESS, total_bytes_string);
  }
  pthread_mutex_unlock(&marker->Lock);
}

globus_result_t cksm_start_markers(cksm_marker_t **Marker,
                                   globus_gfs_operation_t Operation) {
  int marker_freq = 0;
  globus_reltime_t delay;
  globus_result_t result = GLOBUS_SUCCESS;

  GlobusGFSName(cksm_start_markers);

  /* Get the frequency for maker updates. */
  globus_gridftp_server_get_update_interval(Operation, &marker_freq);

  if (marker_freq > 0) {
    *Marker = malloc(sizeof(cksm_marker_t));
    if (!*Marker){
      ERR(": malloc(%ld) failed", sizeof(cksm_marker_t));
      return GlobusGFSErrorMemory("cksm_marker_t");
    }

    pthread_mutex_init(&(*Marker)->Lock, NULL);
    (*Marker)->TotalBytes = 0;
    (*Marker)->Operation = Operation;

    /* Setup the periodic callback. */
    GlobusTimeReltimeSet(delay, marker_freq, 0);
    result =
        globus_callback_register_periodic(&(*Marker)->CallbackHandle, &delay,
                                          &delay, cksm_send_markers, (*Marker));
    if (result) {
      free(*Marker);
      *Marker = NULL;
    }
  }

  return result;
}

void cksm_stop_markers(cksm_marker_t *Marker) {
  if (Marker) {
    globus_callback_unregister(Marker->CallbackHandle, NULL, NULL, NULL);
    pthread_mutex_destroy(&Marker->Lock);
    free(Marker);
  }
}

globus_result_t cksm_open_for_reading(char *Pathname, int *FileFD,
                                      int *FileStripeWidth) {
  char p[HPSS_MAX_PATH_NAME];
  strncpy(p, Pathname, HPSS_MAX_PATH_NAME);
  DEBUG("(%s)", p);
  hpss_cos_hints_t hints_in;
  hpss_cos_hints_t hints_out;
  hpss_cos_priorities_t priorities;

  GlobusGFSName(cksm_open_for_reading);

  *FileFD = -1;

  /* Initialize the hints in. */
  memset(&hints_in, 0, sizeof(hpss_cos_hints_t));

  /* Initialize the hints out. */
  memset(&hints_out, 0, sizeof(hpss_cos_hints_t));

  /* Initialize the priorities. */
  memset(&priorities, 0, sizeof(hpss_cos_priorities_t));

  /* Open the HPSS file. */
  *FileFD = hpss_Open(p, O_RDONLY, S_IRUSR | S_IWUSR, &hints_in,
                      &priorities, &hints_out);
  if (*FileFD < 0){
    ERR(": hpss_Open(%s) failed", p);
    return GlobusGFSErrorSystemError("hpss_Open", -(*FileFD));
  }

  /* Copy out the file stripe width. */
  *FileStripeWidth = hints_out.StripeWidth;

  return GLOBUS_SUCCESS;
}

int cksm_pio_callout(char *Buffer, uint32_t *Length, uint64_t Offset,
                     void *CallbackArg) {
  int rc = 0;
  cksm_info_t *cksm_info = CallbackArg;

  GlobusGFSName(cksm_pio_callout);

  assert(*Length <= cksm_info->BlockSize);

  rc = MD5_Update(&cksm_info->MD5Context, Buffer, *Length);
  if (rc != 1) {
    ERR(": MD5_Update failed");
    cksm_info->Result = GlobusGFSErrorGeneric("MD5_Update() failed");
    return 1;
  }

  cksm_update_markers(cksm_info->Marker, *Length);

  return 0;
}

void cksm_range_complete_callback(globus_off_t *Offset, globus_off_t *Length,
                                  int *Eot, void *UserArg) {
  cksm_info_t *cksm_info = UserArg;

  *Offset += *Length;
  cksm_info->RangeLength -= *Length;
  *Length = cksm_info->RangeLength;

  if (*Length == 0)
    *Eot = 1;
}

void cksm_transfer_complete_callback(globus_result_t Result, void *UserArg) {
  DEBUG();
  globus_result_t result = Result;
  cksm_info_t *cksm_info = UserArg;
  int rc = 0;
  unsigned char md5_digest[MD5_DIGEST_LENGTH];
  char cksm_string[2 * MD5_DIGEST_LENGTH + 1];
  int i;

  GlobusGFSName(cksm_transfer_complete_callback);
  DEBUG("(%s)", cksm_info->Pathname);

  /* Give our error priority. */
  if (cksm_info->Result)
    result = cksm_info->Result;

  rc = hpss_Close(cksm_info->FileFD);
  if (rc && !result){
    ERR(":hpss_Close(%d) failed", cksm_info->FileFD);
    result = GlobusGFSErrorSystemError("hpss_Close", -rc);
  }

  if (!result) {
    rc = MD5_Final(md5_digest, &cksm_info->MD5Context);
    if (rc != 1){
      ERR(": MD5_Final failed");
      result = GlobusGFSErrorGeneric("MD5_Final() failed");
    }
  }

  if (!result) {
    for (i = 0; i < MD5_DIGEST_LENGTH; i++) {
      sprintf(&(cksm_string[i * 2]), "%02x", (unsigned int)md5_digest[i]);
    }
  }

  cksm_stop_markers(cksm_info->Marker);

  DEBUG(": Callback");
  cksm_info->Callback(cksm_info->Operation, result,
                      result ? NULL : cksm_string);

  if (!result && cksm_info->CommandInfo->cksm_offset == 0 &&
      cksm_info->CommandInfo->cksm_length == -1)
    cksm_set_checksum(cksm_info->Pathname, cksm_info->Config, cksm_string);

    DEBUG(": free cksm_info %p", cksm_info);
  //CHECKfree(cksm_info);
  DEBUG(": return, freed %p", cksm_info);
}

void cksm(globus_gfs_operation_t Operation,
          globus_gfs_command_info_t *CommandInfo, config_t *Config,
          commands_callback Callback) {
  GlobusGFSName(cksm);
  DEBUG("(%s): ", CommandInfo->pathname);
  globus_result_t result = GLOBUS_SUCCESS;
  cksm_info_t *cksm_info = NULL;
  int rc = 0;
  int file_stripe_width = 0;
  char checksum_string[HPSS_XML_SIZE];
  hpss_stat_t hpss_stat_buf;

  if (CommandInfo->cksm_offset == 0 && CommandInfo->cksm_length == -1) {
    result =
        checksum_get_file_sum(CommandInfo->pathname, Config, checksum_string);
    if (result) {
      DEBUG(": Callback");
      Callback(Operation, result, result ? NULL : checksum_string);
      return;
    }
  }

  if ((rc = hpss_Stat(CommandInfo->pathname, &hpss_stat_buf))){
		ERR(": hpss_Stat(%s) failed, code %d, %s", CommandInfo->pathname, rc, strerror(errno));
    result = GlobusGFSErrorSystemError("hpss_Stat", -rc);
    Callback(Operation, result, NULL);
    DEBUG(": returns");
    return;
  }

  cksm_info = malloc(sizeof(cksm_info_t));
  if (!cksm_info) {
    ERR(": malloc(%ld) failed", sizeof(cksm_info_t));
    result = GlobusGFSErrorMemory("cksm_info_t");
    goto cleanup;
  }
  memset(cksm_info, 0, sizeof(cksm_info_t));
  cksm_info->Operation = Operation;
  cksm_info->CommandInfo = CommandInfo;
  cksm_info->Callback = Callback;
  cksm_info->Config = Config;
  cksm_info->FileFD = -1;
  strcpy(cksm_info->Pathname, CommandInfo->pathname);
  cksm_info->RangeLength = CommandInfo->cksm_length;
  if (cksm_info->RangeLength == -1)
    cksm_info->RangeLength = hpss_stat_buf.st_size - CommandInfo->cksm_offset;

  rc = MD5_Init(&cksm_info->MD5Context);
  if (rc != 1) {
    ERR(": MD5_Init failed");
    result = GlobusGFSErrorGeneric("Failed to create MD5 context");
    goto cleanup;
  }

  globus_gridftp_server_get_block_size(Operation, &cksm_info->BlockSize);

  /*
   * Open the file.
   */
  result = cksm_open_for_reading(CommandInfo->pathname, &cksm_info->FileFD,
                                 &file_stripe_width);
  if (result)
    goto cleanup;

  result = cksm_start_markers(&cksm_info->Marker, Operation);
  if (result)
    goto cleanup;

  /*
   * Setup PIO
   */
  result = pio_start(
      HPSS_PIO_READ, cksm_info->FileFD, file_stripe_width, cksm_info->BlockSize,
      CommandInfo->cksm_offset, cksm_info->RangeLength, cksm_pio_callout,
      cksm_range_complete_callback, cksm_transfer_complete_callback, cksm_info);

cleanup:
  if (result) {
    if (cksm_info) {
      if (cksm_info->FileFD != -1)
        hpss_Close(cksm_info->FileFD);
      DEBUG(": free cksm_info %p", cksm_info);
      //free(cksm_info);
    }
    Callback(Operation, result, NULL);
  }
  DEBUG(": returns");
}

/*
 * /hpss/user/cksum/algorithm                                  md5
 * /hpss/user/cksum/checksum               93b885adfe0da089cdf634904fd59f71
 * /hpss/user/cksum/lastupdate                          1376424299
 * /hpss/user/cksum/errors                                       0
 * /hpss/user/cksum/state                                    Valid
 * /hpss/user/cksum/app                                    hpsssum
 * /hpss/user/cksum/filesize                                     1
 */
globus_result_t cksm_set_checksum(char *p, config_t *Config,
                                  char *Checksum) {
  DEBUG("(%s)", p);
  
  int retval = 0;
  globus_result_t result = GLOBUS_SUCCESS;
  
  GlobusGFSName(checksum_set_file_sum);

  if (Config->UDAChecksumSupport) {
    char gstat_name[HPSS_MAX_PATH_NAME];
    char gstat_link_name[HPSS_MAX_PATH_NAME];
    globus_gfs_stat_t gfs_stat = {0};
    result = stat_object(p, &gfs_stat, gstat_name, gstat_link_name);
    if (result != GLOBUS_SUCCESS){
      ERR(": stat_object(%s) failed", p);
      return GlobusGFSErrorSystemError("File stat error", -retval);
    }

    hpss_userattr_list_t alist;
    alist.len = 7;
    alist.Pair = malloc((1+alist.len) * sizeof(hpss_userattr_t));
    for (int i = 0; i < alist.len; i++) {
      alist.Pair[i].Value = malloc(HPSS_XML_SIZE);
      alist.Pair[i].Key = malloc(HPSS_XML_SIZE);
    }

    strcpy(alist.Pair[0].Key, "/hpss/user/cksum/algorithm");
    strcpy(alist.Pair[1].Key, "/hpss/user/cksum/checksum");
    strcpy(alist.Pair[2].Key, "/hpss/user/cksum/lastupdate");
    strcpy(alist.Pair[3].Key, "/hpss/user/cksum/errors");
    strcpy(alist.Pair[4].Key, "/hpss/user/cksum/state");
    strcpy(alist.Pair[5].Key, "/hpss/user/cksum/app");
    strcpy(alist.Pair[6].Key, "/hpss/user/cksum/filesize");
      
    strcpy(alist.Pair[0].Value, "md5");
    strcpy(alist.Pair[1].Value, Checksum);
    snprintf(alist.Pair[2].Value, MAX_XPATH_ELEM_SIZE, "%lu", time(NULL));
    strcpy(alist.Pair[3].Value, "0");
    strcpy(alist.Pair[4].Value, "Valid");
    strcpy(alist.Pair[5].Value, "GridFTP");
    snprintf(alist.Pair[6].Value, MAX_XPATH_ELEM_SIZE, "%lu", gfs_stat.size);

    alist.Pair[7].Key = alist.Pair[7].Value = 0;  // sanity

    DEBUG(": write attrs");
    retval = hpss_UserAttrSetAttrs(p, &alist, NULL);

    for (int i = 0; i < alist.len; i++){
      free (alist.Pair[i].Key);
      free (alist.Pair[i].Value);
    }
    free (alist.Pair);
  }
  if (retval){
    ERR(": hpss_UserAttrSetAttrs(%s) failed, code %d, %s", p, retval, strerror(errno));
    return GlobusGFSErrorSystemError("hpss_UserAttrSetAttrs", -retval);
  }

  DEBUG("(%s): success", p);
  return GLOBUS_SUCCESS;
}

globus_result_t checksum_get_file_sum(char *Pathname, config_t *Config,
                                      char *ChecksumString) {
  char p[HPSS_MAX_PATH_NAME];
  strncpy(p, Pathname, HPSS_MAX_PATH_NAME);
  DEBUG("(%s)", p);
  GlobusGFSName(checksum_get_file_sum);
  
  int retval = 0;
  char *tmp = NULL;
  ChecksumString[0] = '\0';

  // check for the object first
  char gstat_name[HPSS_MAX_PATH_NAME];
  char gstat_link_name[HPSS_MAX_PATH_NAME];
  globus_gfs_stat_t gfs_stat ={0};
  int result = stat_object(p, &gfs_stat, gstat_name, gstat_link_name);
  if (result != GLOBUS_SUCCESS){
    ERR(": stat_object(%s) failed", p);
    return GlobusGFSErrorSystemError("File stat error", -retval);
  }

  if (Config->UDAChecksumSupport) {
    hpss_userattr_list_t alist;
    alist.len = 3;
    alist.Pair = malloc((1+alist.len) * sizeof(hpss_userattr_t));
    for (int i = 0; i < alist.len; i++) {
      alist.Pair[i].Value = malloc(HPSS_XML_SIZE);
      alist.Pair[i].Key = malloc(HPSS_XML_SIZE);
    }

    strcpy(alist.Pair[0].Key, "/hpss/user/cksum/algorithm");
    strcpy(alist.Pair[1].Key, "/hpss/user/cksum/checksum");
    strcpy(alist.Pair[2].Key, "/hpss/user/cksum/state");
    alist.Pair[3].Value = alist.Pair[3].Key = 0;

    DEBUG("(%s): get attrs", p);
    retval = hpss_UserAttrLenGetAttrs(p, &alist, UDA_API_VALUE, HPSS_XML_SIZE);
    DEBUG("(%s): get attrs returned %d", p, retval);

    switch (retval) {
    case 0:
      break;
    case -ENOENT:
      DEBUG(": success (none)")
      return GLOBUS_SUCCESS;
      break;
    default:
      ERR(": hpss_UserAttrGetAttrs(%s) failed, code %d, %s", p, retval, strerror(errno));
      DEBUG("(%p) alist %p, alist.Pair %p", p, (void*)&alist, (void*)&alist.Pair[0]);
      return GlobusGFSErrorSystemError("hpss_UserAttrGetAttrs", -retval);
      break;
    }

    DEBUG(": decode attrs");
    char *algorithm = alist.Pair[0].Value;
    char *checksum = alist.Pair[1].Value;
    char *state = alist.Pair[2].Value;
    
    int cont = 1; // continuing, or skip to end
    tmp = hpss_ChompXMLHeader(algorithm, NULL);
    if (!tmp) {
      retval = 0;
      cont = 0;
    }

    if (cont && strcmp(tmp, "md5") != 0){
      retval = 0;
      cont = 0;
    }
    if (tmp) free(tmp);

    if (cont && ((tmp = hpss_ChompXMLHeader(state, NULL)) == NULL)){
      retval = 0;
      cont = 0;
    }

    if (cont && (strcmp(tmp, "Valid") != 0)) {
      retval = 0;
      cont = 0;
    }
    if (tmp) free(tmp);

    if (cont && ((tmp = hpss_ChompXMLHeader(checksum, NULL)) != NULL)){
      strcpy(ChecksumString, tmp);
      free(tmp);
    }

    for (int i = 0; i < alist.len; i++){
      free (alist.Pair[i].Key);
      free (alist.Pair[i].Value);
    }
    free (alist.Pair);
  }
  DEBUG(": returns");
  return GLOBUS_SUCCESS;
}

globus_result_t cksm_clear_checksum(char *Pathname, config_t *Config) {
  char p[HPSS_MAX_PATH_NAME];
  strncpy(p, Pathname, HPSS_MAX_PATH_NAME);
  DEBUG("(%s)", p);
  int retval = 0;

  GlobusGFSName(checksum_clear_file_sum);

  if (Config->UDAChecksumSupport) {
    hpss_userattr_list_t alist;
    alist.len = 1;
    alist.Pair = malloc((1+alist.len) * sizeof(hpss_userattr_t));
    for (int i = 0; i < alist.len; i++) {
      alist.Pair[i].Value = malloc(HPSS_XML_SIZE);
      alist.Pair[i].Key = malloc(HPSS_XML_SIZE);
    }

    strcpy(alist.Pair[0].Key, "/hpss/user/cksum/state");
    strcpy(alist.Pair[0].Value, "Invalid");
    alist.Pair[1].Key = alist.Pair[1].Value = 0;

    retval = hpss_UserAttrSetAttrs(p, &alist, NULL);
    if (retval) DEBUG(": hpss_UserAttrSetAttrs failed code %d", retval);
    if (retval && retval != -ENOENT) {
      ERR(": hpss_UserAttrSetAttrs(%p) failed: code %d", p, retval);
      return GlobusGFSErrorSystemError("hpss_UserAttrSetAttrs", -retval);
    }
  }
  DEBUG(": returns");
  return GLOBUS_SUCCESS;
}
