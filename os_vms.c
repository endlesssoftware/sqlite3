/*
** 2013 March 8
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code that is specific to OpenVMS.
*/
#include "sqliteInt.h"
#if SQLITE_OS_VMS               /* This file is used for OpenVMS only */
#include <armdef.h>
#include <atrdef.h>
#include <chpdef.h>
#include <descrip.h>
#include <efndef.h>
#include <fatdef.h>
#include <fibdef.h>
#include <iodef.h>
#include <jpidef.h>
#include <lckdef.h>
#include <libfisdef.h>
#include <lib$routines.h>
#include <lkidef.h>
#include <mth$routines.h>
#include <psldef.h>
#include <rms.h>
#include <sbkdef.h>
#include <secdef.h>
#include <ssdef.h>
#include <starlet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <str$routines.h>
#include <stsdef.h>
#include <time.h>
#include <tis.h>
#include <unistd.h>
#ifdef vax
# include <sys$routines.h>
# define FAT struct fatdef
#endif

/*
** Include code that is common to all os_*.c files
*/
#include "os_common.h"

#ifdef min
# undef min
#endif
#define min(m, n) ((m) < (n) ? (m) : (n))

#define LOCK_NAME_MAX 31

struct ile3 {
  unsigned short buflen;
  unsigned short itmcod;
  void *bufadr;
  unsigned int *retlen;
};

/*
** This array provides the conversion between SQLite locking levels
** and the equivalent OpenVMS DLM levels.
*/
const static int lock_modes[] = {
  LCK$K_NLMODE,    /* SQLITE_LOCK_NONE */
  LCK$K_PRMODE,    /* SQLITE_LOCK_SHARED */
  LCK$K_PWMODE,    /* SQLITE_LOCK_RESERVED */
  -1,              /* SQLITE_LOCK_PENDING */
  LCK$K_EXMODE     /* SQLITE_LOCK_EXCLUSIVE */
};

/*
** The vmsFile structure is a subclass of sqlite3_file *, specific to the
** OpenVMS portability layer.
*/
typedef struct vmsFile vmsFile;
struct vmsFile {
  const sqlite3_io_methods *pMethod;  /*** Must be first ***/
  int szChunk;
  int szHint;
  char lock_name[LOCK_NAME_MAX+1];
  char esa[NAM$C_MAXRSS+1];
  struct {
    unsigned short status;
    unsigned short reserved;
    unsigned long id;
  } lksb;
  struct FAB fab;
  struct NAM nam;
  unsigned short chan;
};

/*****************************************************************************
** The next group of routines implement the I/O methods specified
** by the sqlite3_io_methods object.
******************************************************************************/

static int vmsClose(
  sqlite3_file *id          /* File to close */
){
  vmsFile *pFile = (vmsFile *)id;

  sys$deq(pFile->lksb.id, 0, PSL$C_USER, LCK$M_CANCEL);
  sys$dassgn(pFile->chan);

  return SQLITE_OK;
}

static int vmsRead(
  sqlite3_file *id,         /* File to read from */
  void *vBuf,               /* Write content into this buffer */
  int amt,                  /* Number of bytes to read */
  sqlite3_int64  offset     /* Begin reading at this offset */
){
  int bcnt;
  char *pBuf = vBuf;
  int remainder;
  char buf[SQLITE_DEFAULT_SECTOR_SIZE];
  unsigned short iosb[4];
  int status = SS$_NORMAL;
  vmsFile *pFile = (vmsFile *)id;

  /*
  ** Determine the virtual block we are to read from, and a possbile byte
  ** position within that block.
  */
  int vbn = (offset / SQLITE_DEFAULT_SECTOR_SIZE) + 1;
  int vpos = offset % SQLITE_DEFAULT_SECTOR_SIZE;

  if( vpos ){
    status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_READVBLK, iosb,
        0, 0, buf, SQLITE_DEFAULT_SECTOR_SIZE, vbn++, 0, 0, 0);
    if( $VMS_STATUS_SUCCESS(status)
        && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
      bcnt = min(amt, SQLITE_DEFAULT_SECTOR_SIZE - vpos);
      memcpy(pBuf, &buf[vpos], bcnt);

      pBuf += bcnt;
      amt -= bcnt;
    }
  }

  if( (amt >= SQLITE_DEFAULT_SECTOR_SIZE) && $VMS_STATUS_SUCCESS(status) ){
    remainder = amt % SQLITE_DEFAULT_SECTOR_SIZE;

    status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_READVBLK, iosb,
        0, 0, pBuf, amt - remainder, vbn, 0, 0, 0);
    if( $VMS_STATUS_SUCCESS(status)
        && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
      bcnt = iosb[1] | (iosb[2] << 16);

      if( bcnt < (amt - remainder) ){
        remainder = amt - bcnt;
        status = SS$_ENDOFFILE;
      }else{
        bcnt = amt - remainder;
      }

      pBuf += bcnt;
      vbn += bcnt / SQLITE_DEFAULT_SECTOR_SIZE;
      amt = remainder;
    }
  }

  if( (amt > 0) && $VMS_STATUS_SUCCESS(status) ){
    status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_READVBLK, iosb,
        0, 0, buf, SQLITE_DEFAULT_SECTOR_SIZE, vbn, 0, 0, 0);
    if( $VMS_STATUS_SUCCESS(status)
        && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
      memcpy(pBuf, buf, amt);
    }
  }

  if( status == SS$_ENDOFFILE ){
    /*
    ** Unread parts of the buffer must be zero-filled.
    */
    memset(pBuf, '\0', amt);
    return SQLITE_IOERR_SHORT_READ;
  }else if( !$VMS_STATUS_SUCCESS(status) ){
    return SQLITE_IOERR_READ;
  }

  return SQLITE_OK;
}

static int vmsWrite(
  sqlite3_file *id,         /* File to read from */
  const void *vBuf,         /* The bytes to be written */
  int amt,                  /* Number of bytes to write */
  sqlite3_int64 offset      /* Offset into the file to begin writing at */
){
  struct atrdef atr[2];
  struct ile3 jpilst[2];
  FAT fat;
  struct fibdef fib;
  char buf[SQLITE_DEFAULT_SECTOR_SIZE];
  const char *pBuf = vBuf;
  int bcnt, extend = 0, filesize, needed, remainder;
  struct dsc$descriptor fibdsc;
  unsigned short iosb[4];
  int status;
  vmsFile *pFile = (vmsFile *)id;

  /*
  ** Determine the virtual block we are to write to, and a possbile byte
  ** position within that block.
  */
  int vbn = (offset / SQLITE_DEFAULT_SECTOR_SIZE) + 1;
  int vpos = offset % SQLITE_DEFAULT_SECTOR_SIZE;

  memset(&fib, 0, sizeof(fib));
  fib.fib$v_writethru = 1;
  fib.fib$w_fid[0] = pFile->nam.nam$w_fid[0];
  fib.fib$w_fid[1] = pFile->nam.nam$w_fid[1];
  fib.fib$w_fid[2] = pFile->nam.nam$w_fid[2];

  fibdsc.dsc$w_length = sizeof(fib);
  fibdsc.dsc$a_pointer = (char *)&fib;

  atr[0].atr$w_size = ATR$S_RECATTR;
  atr[0].atr$w_type = ATR$C_RECATTR;
  atr[0].atr$l_addr = &fat;
  atr[1].atr$w_size = 0;
  atr[1].atr$w_type = 0;

  /*
  ** Before doing a write we determine the size of the file
  ** and if we need to extend it to perform the requested
  ** I/O.
  */
  status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_ACCESS, iosb, 0,
      0, &fibdsc, 0, 0, 0, atr, 0);
  if( $VMS_STATUS_SUCCESS(status)
      && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
    filesize = (fat.fat$w_hiblkh << 16) | fat.fat$w_hiblkl;
    needed = (vbn + (amt / SQLITE_DEFAULT_SECTOR_SIZE)) + 1;

    if( pFile->szHint > 0 ){
      needed = pFile->szHint > needed ? pFile->szHint : needed;
      pFile->szHint = 0;
    }

    if( filesize < needed ){
      fib.fib$v_extend = 1;
      fib.fib$l_exvbn = 0;
      fib.fib$l_exsz = 0;

      /*
      ** Here we first check to see if the user has indicated their
      ** own extension size, otherwise we attempt to respect the
      ** default extensions of the file.  Someone might have
      ** SET FILE/EXTENSION on the database file as a performance
      ** measure. However, if none is set, we just extend by how much
      ** we need of the system default, whichever is larger.
      */
      if( pFile->szChunk > 0 ){
        do{
          fib.fib$l_exsz += pFile->szChunk;
        }while( fib.fib$l_exsz < needed );
      }else{
        if( fat.fat$w_defext ){
          do{
            fib.fib$l_exsz += fat.fat$w_defext;
          }while( fib.fib$l_exsz < needed );
        }else{
          jpilst[0].itmcod = JPI$_RMS_EXTEND_SIZE;
          jpilst[0].buflen = sizeof(extend);
          jpilst[0].bufadr = &extend;
          jpilst[0].retlen = 0;
          jpilst[1].itmcod = 0;
          jpilst[1].buflen = 0;

          sys$getjpiw(EFN$C_ENF, 0, 0, jpilst, iosb, 0, 0);

          fib.fib$l_exsz = extend ? extend : needed;
          fib.fib$v_aldef = 1;
        }
      }

      status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_MODIFY, iosb,
          0, 0, &fibdsc, 0, 0, 0, 0, 0);
      if( $VMS_STATUS_SUCCESS(status)
          && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
        fib.fib$v_extend = 0;
        fib.fib$l_exvbn += fib.fib$l_exsz;

        fat.fat$w_ffbyte = 0;
        fat.fat$w_efblkh = (unsigned short)(fib.fib$l_exvbn >> 16);
        fat.fat$w_efblkl = (unsigned short)fib.fib$l_exvbn;

        status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_MODIFY, iosb,
            0, 0, &fibdsc, 0, 0, 0, atr, 0);
        if( $VMS_STATUS_SUCCESS(status) ){
          status = iosb[0];
        }
      }
    }
  }

  if( vpos && $VMS_STATUS_SUCCESS(status) ){
    /*
    ** The requested offset is NOT on a block boundry, so we need
    ** to read the first block, apply the portion of pBuf and then
    ** write it back.
    */
    status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_READVBLK, iosb,
        0, 0, buf, SQLITE_DEFAULT_SECTOR_SIZE, vbn, 0, 0, 0);
    if( $VMS_STATUS_SUCCESS(status)
       && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
      bcnt = min(amt, SQLITE_DEFAULT_SECTOR_SIZE - vpos);
      memcpy(&buf[vpos], pBuf, bcnt);

      status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_WRITEVBLK, iosb,
           0, 0, buf, SQLITE_DEFAULT_SECTOR_SIZE, vbn++, 0, 0, 0);
      if( $VMS_STATUS_SUCCESS(status)
          && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
        amt -= bcnt;
        if( bcnt <= 0 ){
            pBuf += bcnt;
        }
      }
    }
  }

  if( (amt >= SQLITE_DEFAULT_SECTOR_SIZE) && $VMS_STATUS_SUCCESS(status) ){
    /*
    ** Now, let's write out the middle of the buffer to
    ** the file.
    */
    remainder = amt % SQLITE_DEFAULT_SECTOR_SIZE;

    status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_WRITEVBLK, iosb,
        0, 0, pBuf, amt - remainder, vbn, 0, 0, 0);
    if( $VMS_STATUS_SUCCESS(status)
        && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
      if( bcnt > 0 ){
        pBuf += (amt - remainder);
        vbn += (amt / SQLITE_DEFAULT_SECTOR_SIZE);
      }
      amt = remainder;
    }
  }

  if( (amt > 0) && $VMS_STATUS_SUCCESS(status) ){
    /*
    ** Finally, we write any trailing bytes out to the file.
    */
    status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_READVBLK, iosb,
        0, 0, buf, SQLITE_DEFAULT_SECTOR_SIZE, vbn, 0, 0, 0);
    if( $VMS_STATUS_SUCCESS(status)
        && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
      memcpy(buf, pBuf, amt);

      status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_WRITEVBLK, iosb,
          0, 0, buf, SQLITE_DEFAULT_SECTOR_SIZE, vbn, 0, 0, 0);
      if( $VMS_STATUS_SUCCESS(status) ){
        status = iosb[0];
      }
    }
  }

  return $VMS_STATUS_SUCCESS(status) ? SQLITE_OK : SQLITE_IOERR_WRITE;
}

static int vmsTruncate(
  sqlite3_file *id,         /* File to truncate */
  sqlite3_int64 nByte       /* Size to truncate file to */
){
  struct fibdef fib;
  struct dsc$descriptor fibdsc;
  unsigned short iosb[4];
  vmsFile *pFile = (vmsFile *)id;
  int status;

  memset(&fib, 0, sizeof(fib));
  fib.fib$v_trunc = 1;
  fib.fib$l_exvbn = (nByte / 512) + ((nByte % 512) ? 1 : 0);

  fibdsc.dsc$w_length = sizeof(fib);
  fibdsc.dsc$a_pointer = (char *)&fib;

  sys$qiow(EFN$C_ENF, pFile->chan, IO$_MODIFY, iosb, 0, 0, &fibdsc,
      0, 0, 0, 0, 0);
  return SQLITE_OK;
}

/*
** Exterything goes straight to disk, so this isn't really even
** necessary.
*/
static int vmsSync(
  sqlite3_file *id,         /* File to sync */
  int flags
){
  return SQLITE_OK;
}

static int vmsFileSize(
  sqlite3_file *id,         /* File to get size of */
  sqlite3_int64 *pSize      /* Write size of file here */
){
  struct atrdef atr[2];
  FAT fat;
  struct fibdef fib;
  struct dsc$descriptor fibdsc;
  unsigned short iosb[4];
  vmsFile *pFile = (vmsFile *)id;
  int status;

  memset(&fib, 0, sizeof(fib));
  fib.fib$w_fid[0] = pFile->nam.nam$w_fid[0];
  fib.fib$w_fid[1] = pFile->nam.nam$w_fid[1];
  fib.fib$w_fid[2] = pFile->nam.nam$w_fid[2];

  fibdsc.dsc$w_length = sizeof(fib);
  fibdsc.dsc$a_pointer = (char *)&fib;

  atr[0].atr$w_size = ATR$S_RECATTR;
  atr[0].atr$w_type = ATR$C_RECATTR;
  atr[0].atr$l_addr = &fat;
  atr[1].atr$w_size = 0;
  atr[1].atr$w_type = 0;

  status = sys$qiow(EFN$C_ENF, pFile->chan, IO$_ACCESS, iosb, 0, 0,
                    &fibdsc, 0, 0, 0, atr, 0);
  if( $VMS_STATUS_SUCCESS(status)
      && $VMS_STATUS_SUCCESS(iosb[0]) ){
    *pSize = ((fat.fat$w_hiblkh << 16) | fat.fat$w_hiblkl) *
        SQLITE_DEFAULT_SECTOR_SIZE;
    return SQLITE_OK;
  }

  return SQLITE_IOERR_FSTAT;
}

static int vmsLock(
  sqlite3_file *id,         /* File to query */
  int locktype              /* Requested lock mode */
){
  vmsFile *pFile = (vmsFile *)id;
  int status;

  status = sys$enqw(EFN$C_ENF, lock_modes[locktype], &pFile->lksb,
      LCK$M_CONVERT, 0, 0, 0, 0, 0, PSL$C_USER, 0, 0);
  if( $VMS_STATUS_SUCCESS(status)
      && $VMS_STATUS_SUCCESS(pFile->lksb.status) ){
    return SQLITE_OK;
  }

  return SQLITE_BUSY;
}

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
*/
static int vmsCheckReservedLock(
  sqlite3_file *id,         /* File to query */
  int *pResOut              /* Is current lock RESERVED? */
){
  char state[3];
  int iosb[2];
  int rc = SQLITE_OK;
  struct ile3 itmlst[2];
  vmsFile *pFile = (vmsFile *)id;
  int status;

  *pResOut = 0;

  itmlst[0].itmcod = LKI$_STATE;
  itmlst[0].buflen = sizeof(state);
  itmlst[0].bufadr = state;
  itmlst[0].retlen = 0;
  itmlst[1].buflen = 0;
  itmlst[1].itmcod = 0;

  status = sys$getlkiw(EFN$C_ENF, &pFile->lksb.id, itmlst, iosb, 0, 0, 0);
  if( $VMS_STATUS_SUCCESS(status)
    && $VMS_STATUS_SUCCESS(status = iosb[0]) ){
    if( state[1] == lock_modes[SQLITE_LOCK_RESERVED] ){
      *pResOut = 1;
    }
  }else{
    rc = SQLITE_IOERR_CHECKRESERVEDLOCK;
  }

  return rc;
}

static int vmsUnLock(
  sqlite3_file *id,         /* File to query */
  int locktype              /* Requested lock mode */
){
  vmsFile *pFile = (vmsFile *)id;
  int status;

  status = sys$enqw(EFN$C_ENF, lock_modes[locktype], &pFile->lksb,
      LCK$M_CONVERT, 0, 0, 0, 0, 0, PSL$C_USER, 0, 0);
  if( $VMS_STATUS_SUCCESS(status)
      && $VMS_STATUS_SUCCESS(pFile->lksb.status) ){
    return SQLITE_OK;
  }

  return SQLITE_IOERR_UNLOCK;
}

static int vmsFileControl(
  sqlite3_file *id,         /* File to query */
  int op,
  void *pArg
){
  vmsFile *pFile = (vmsFile *)id;

  switch( op ){
    case SQLITE_FCNTL_CHUNK_SIZE: {
      pFile->szChunk = *(int *)pArg;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_SIZE_HINT: {
      pFile->szHint = (*(int *)pArg / SQLITE_DEFAULT_SECTOR_SIZE) + 1;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_VFSNAME: {
      *(char**)pArg = sqlite3_mprintf("%s", pFile->pVfs->zName);
      return SQLITE_OK;
    }
  }
  return SQLITE_NOTFOUND;
}

static int vmsSectorSize(
  sqlite3_file *id          /* File to get sector size from */
){
  return SQLITE_DEFAULT_SECTOR_SIZE;
}

static int vmsDeviceCharacteristics(
  sqlite3_file *id          /* File to close */
){
  return SQLITE_IOCAP_ATOMIC512 | SQLITE_IOCAP_SEQUENTIAL;
}

static int vmsShmMap(
  sqlite3_file *id,               /* Handle open on database file */
  int iRegion,                    /* Region to retrieve */
  int szRegion,                   /* Size of regions */
  int bExtend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  vmsFile *pFile = (vmsFile *)id;
  int status;
#if 0
  // generate the lock name...

  status = sys$enqw(EFN$C_ENF, LCK$K_NLMODE, &lksb,
                    LCK$M_NOQUEUE | LCK$M_SYSTEM | LCK$M_EXPEDITE, lock_name,
                    0, 0, 0, 0, PSL$C_USER, 0, 0);
  if( $VMS_STATUS_SUCCESS(status) ){
    status = sys$enqw(EFN$C_ENF, LCK$K_EXMODE, &lksb,
                      LCK$M_NOQUEUE | LCK$M_SYSTEM | LCK$M_CONVERT, 0,
    if( $VMS_STATUS_SUCCESS(status) ){
      status = sys$mgblsc();
      if( $VMS_STATUS_SUCCESS(status) ){
      } else if( status == ... ){
        status = sys$crmpsc();
      } else {
        // error
      }
    } else {
      // if because its already allocate?
        // mpgblsc
      // else
        // error
    }
  } else {
    // report an error...
  }
#endif
  return SQLITE_OK;
}

static int vmsShmLock(
  sqlite3_file *id,
  int offset,
  int n,
  int flags
){
  int lkmode;
#if 0
  if( flags & SQLITE_SHM_UNLOCK ){
    lkmode = LCK$K_NLMODE;
  } else if( flags & SQLITE_SHM_SHARED ){
    lkmode = LCK$K_PRMODE;
  } else if( flags & SQLITE_SHM_EXCLUSIVE ){
    lkmode = LCK$K_EXMODE;
  }

  status = sys$enqw(EFN$C_ENF, lkmode,

  // report appropriate status value...
#endif
  return SQLITE_OK;
}

static void vmsShmBarrier(
  sqlite3_file *id
){
  int status;
#if 0
  status = sys$updsecw();
  if( $VMS_STATUS_SUCCESS(status)
    && $VMS_STATUS_SUCCESS(status == iosb[0]) ){
    // report success...
  } else {
    // translate error
  }
#endif
}

static int vmsShmUnmap(
  sqlite3_file *id,
  int deleteFlag
){
  // hmmm, how to unmap a temporary section? $delva?

  return SQLITE_OK;
}

/*
** Here ends the implementation of all sqlite3_file methods.
**
********************** End sqlite3_file Methods *******************************
******************************************************************************/

/*
** This vector defines all the methods that can operate on an
** sqlite3_file for OpenVMS.
*/
static const sqlite3_io_methods vmsIoMethod = {
  2,                              /* iVersion */
  vmsClose,                       /* xClose */
  vmsRead,                        /* xRead */
  vmsWrite,                       /* xWrite */
  vmsTruncate,                    /* xTruncate */
  vmsSync,                        /* xSync */
  vmsFileSize,                    /* xFileSize */
  vmsLock,                        /* xLock */
  vmsUnLock,                      /* xUnlock */
  vmsCheckReservedLock,           /* xCheckReservedLock */
  vmsFileControl,                 /* xFileControl */
  vmsSectorSize,                  /* xSectorSize */
  vmsDeviceCharacteristics,       /* xDeviceCharacteristics */
  vmsShmMap,                      /* xShmMap */
  vmsShmLock,                     /* xShmLock */
  vmsShmBarrier,                  /* xShmBarrier */
  vmsShmUnmap                     /* xShmUnmap */
};

/****************************************************************************
**************************** sqlite3_vfs methods ****************************
**
** This division contains the implementation of methods on the
** sqlite3_vfs object.
*/

static int vmsOpen(
  sqlite3_vfs *pVfs,        /* VFS record */
  const char *zFilename,    /* Name of file to open */
  sqlite3_file *id,         /* Write the SQLite file handle here */
  int flags,                /* Open mode flags */
  int *pOutFlags            /* Status return flags */
){
  vmsFile *pFile = (vmsFile *)id;
  struct dsc$descriptor resnam;
  int status;

  memset(pFile, 0, sizeof(*pFile));
  pFile->pMethod = &vmsIoMethod;

  pFile->nam = cc$rms_nam;
  pFile->nam.nam$l_esa = pFile->esa;
  pFile->nam.nam$b_ess = NAM$C_MAXRSS;

  pFile->fab = cc$rms_fab;
  pFile->fab.fab$l_nam = &pFile->nam;
  pFile->fab.fab$l_fna = (char *)zFilename;
  pFile->fab.fab$b_fns = zFilename ? strlen(zFilename) : 0;
  pFile->fab.fab$b_org = FAB$C_SEQ;
  pFile->fab.fab$b_rfm = FAB$C_FIX;
  pFile->fab.fab$w_mrs = 512;
  pFile->fab.fab$v_shrput = 1;
  pFile->fab.fab$v_shrupd = 1;
  pFile->fab.fab$v_upi = 1;
  pFile->fab.fab$v_ufo = 1;
  pFile->fab.fab$b_rtv = 255;
  pFile->fab.fab$v_cbt = 1;

  if( !zFilename || !*zFilename ){
    pFile->fab.fab$v_put = 1;
    pFile->fab.fab$v_tmd = 1;
    pFile->fab.fab$v_upd = 1;

    status = sys$create(&pFile->fab);
  }else{
    if( flags & SQLITE_OPEN_DELETEONCLOSE ){
      pFile->fab.fab$v_dlt = 1;
    }

    if( flags & SQLITE_OPEN_READONLY ){
      pFile->fab.fab$v_get = 1;
    }else{
      pFile->fab.fab$v_put = 1;
      pFile->fab.fab$v_upd = 1;
    }

    if( flags & SQLITE_OPEN_EXCLUSIVE ){
      status = sys$create(&pFile->fab);
    }else if( flags & SQLITE_OPEN_CREATE ){
      pFile->fab.fab$v_cif = 1;
      status = sys$create(&pFile->fab);
    }else{
      status = sys$open(&pFile->fab);
    }
  }

  if( $VMS_STATUS_SUCCESS(status) ){
    pFile->chan = pFile->fab.fab$l_stv;

    pFile->esa[pFile->nam.nam$b_esl] = 0;

    if( pOutFlags ){
      if( flags & SQLITE_OPEN_READWRITE ){
        *pOutFlags = SQLITE_OPEN_READWRITE;
      }else{
        *pOutFlags = SQLITE_OPEN_READONLY;
      }
    }

    sprintf(pFile->lock_name, "SQLITE3_%04X%04X%04X",
        pFile->nam.nam$w_fid[0], pFile->nam.nam$w_fid[1],
        pFile->nam.nam$w_fid[2]);

    resnam.dsc$w_length = strlen(pFile->lock_name);
    resnam.dsc$b_dtype = DSC$K_DTYPE_T;
    resnam.dsc$b_class = DSC$K_CLASS_S;
    resnam.dsc$a_pointer = pFile->lock_name;

    status = sys$enqw(EFN$C_ENF, LCK$K_NLMODE, &pFile->lksb,
        LCK$M_NOQUEUE | LCK$M_SYSTEM | LCK$M_EXPEDITE,
        &resnam, 0, NULL, NULL, NULL, PSL$C_USER, NULL, NULL);
    if( $VMS_STATUS_SUCCESS(status)
        && $VMS_STATUS_SUCCESS(pFile->lksb.status) ){
      return SQLITE_OK;
    }

    sys$close(&pFile->fab);
  }

  return SQLITE_CANTOPEN;
}

static int vmsDelete(
  sqlite3_vfs *pVfs,        /* VFS record */
  const char *zFilename,    /* Name of file to delete */
  int syncDit               /* Not used on OpenVMS */
){
  struct FAB fab;
  int status;

  fab = cc$rms_fab;
  fab.fab$l_fna = (char *)zFilename;
  fab.fab$b_fns = strlen(zFilename);

  status = sys$erase(&fab);
  if( $VMS_STATUS_SUCCESS(status)
    || (status == RMS$_FNF) ){
    return SQLITE_OK;
  }

  return SQLITE_IOERR_DELETE;
}

static int vmsAccess(
  sqlite3_vfs *pVfs,        /* VFS record */
  const char *zFilename,    /* Name of file to check */
  int flags,                /* Type of test to make on this file */
  int *pResOut              /* Result */
){
  static struct dsc$descriptor username = { 0 };

  int access;
  int seflags = CHP$M_OBSERVE;
  struct dsc$descriptor file;
  struct ile3 itmlst[3];
  int result;
  int status;

  static $DESCRIPTOR(class, "FILE");

  if( !username.dsc$a_pointer ){
    long jpicod = JPI$_USERNAME;

    username.dsc$b_dtype = DSC$K_DTYPE_T;
    username.dsc$b_class = DSC$K_CLASS_D;

    status = lib$getjpi(&jpicod, 0, 0, 0, &username, 0);
    if( !$VMS_STATUS_SUCCESS(status) ){
      return SQLITE_IOERR_ACCESS;
    }
  }

  file.dsc$w_length = strlen(zFilename);
  file.dsc$a_pointer = (char *)zFilename;

  access = ARM$M_READ;
  if( flags == SQLITE_ACCESS_READWRITE ){
    access |= ARM$M_WRITE;
  }

  itmlst[0].itmcod = CHP$_ACCESS;
  itmlst[0].buflen = sizeof(access);
  itmlst[0].bufadr = &access;
  itmlst[0].retlen = NULL;
  itmlst[1].itmcod = CHP$_FLAGS;
  itmlst[1].buflen = sizeof(seflags);
  itmlst[1].bufadr = &seflags;
  itmlst[1].retlen = NULL;
  itmlst[2].itmcod = CHP$_END;
  itmlst[2].buflen = 0;

  status = sys$check_access(0, &file, &username, itmlst, 0, &class, 0, 0);
  if( $VMS_STATUS_SUCCESS(status) ){
    if( pResOut ){
      *pResOut = 1;
    }

    return SQLITE_OK;
  }else{
    if( pResOut ){
      *pResOut = 0;
    }

    if( flags == SQLITE_ACCESS_EXISTS ){
      if( status == RMS$_FNF ){
        return SQLITE_OK;
      }
    }else if( status == SS$_NOPRIV || status == SS$_NOCALLPRIV ){
      return SQLITE_OK;
    }
  }
  return SQLITE_IOERR_ACCESS;
}

static int vmsFullPathname(
  sqlite3_vfs *pVfs,        /* VFS record */
  const char *zRelative,    /* Input path */
  int nFull,                /* Size of output buffer in bytes */
  char *zFull               /* Output buffer */
){
  struct FAB fab;
  struct NAM nam;
  int status;

  nam = cc$rms_nam;
  nam.nam$v_synchk = 1;
  nam.nam$l_esa = zFull;
  nam.nam$b_ess = nFull - 1; /* subtract 1 for terminating NULL */

  fab = cc$rms_fab;
  fab.fab$l_nam = &nam;
  fab.fab$l_fna = (char *)zRelative;
  fab.fab$b_fns = strlen(zRelative);

  status = sys$parse(&fab);
  if( $VMS_STATUS_SUCCESS(status) ){
    *nam.nam$l_ver = '\0';
    return SQLITE_OK;
  }

  return SQLITE_ERROR;
}

#ifndef SQLITE_OMIT_LOAD_EXTENSION
/*
** -- possibly allocate a structure of a pair of descriptors
**    that contains the default part and the name part, after
**    parsing the incoming name.  Need to check what we have...
*/
static void *vmsDlOpen(
  sqlite3_vfs *notUsed,
  const char *zName
){
  struct dsc$descriptor *dImagename = 0;
  struct dsc$descriptor dName;
  int status;

  static $DESCRIPTOR(prefix, "SQLITE3_EXTENSION_");

  dImagename = sqlite3_malloc(sizeof(*dImagename));
  if( dImagename ){
    dImagename->dsc$w_length = 0;
    dImagename->dsc$b_dtype = DSC$K_DTYPE_T;
    dImagename->dsc$b_class = DSC$K_CLASS_D;
    dImagename->dsc$a_pointer = 0;

    dName.dsc$w_length = strlen(zName);
    dName.dsc$b_dtype = DSC$K_DTYPE_T;
    dName.dsc$b_class = DSC$K_CLASS_S;
    dName.dsc$a_pointer = (char *)zName;

    status = str$concat(dImagename, &prefix, &dName);
    if( $VMS_STATUS_SUCCESS(status) ){
      return dImagename;
    }else{
      sqlite3_free(dImagename);
    }
  }

  return 0;
}

static void (*vmsDlSym(
  sqlite3_vfs *pVfs,
  void *pHandle,
  const char *zSymbol
))(void){
  struct dsc$descriptor *dName = pHandle;
  struct dsc$descriptor dSymbol;
  int flags = LIB$M_FIS_MIXEDCASE;
  void (*result)(void) = 0;

  dSymbol.dsc$w_length = strlen(zSymbol);
  dSymbol.dsc$b_dtype = DSC$K_DTYPE_T;
  dSymbol.dsc$b_class = DSC$K_CLASS_S;
  dSymbol.dsc$a_pointer = (char *)zSymbol;

  lib$find_image_symbol(dName, &dSymbol, &result, 0, &flags);

  return result;
}

# define vmsDlError 0

static void vmsDlClose(
  sqlite3_vfs *notUsed,
  void *pHandle
){
  struct dsc$descriptor *dName = pHandle;

  if( dName ){
    str$free1_dx(dName);
    sqlite3_free(dName);
  }
}
#else
# define vmsDlOpen 0
# define vmsDlError 0
# define vmsDlSym 0
# define vmsDlClose 0
#endif

static int vmsRandomness(
  sqlite3_vfs *pVfs,        /* VFS record */
  int nBuf,                 /* Length of output buffer */
  char *zBuf                /* Buffer to write random data to */
){
  static int seed = 0;

  char *pBuf = zBuf;
  float rval;
  int bcnt;

  if( !seed ){
    unsigned int time[2];

    sys$gettim(time);
    seed = time[0] ^ time[1] ^ getpid();
  }

  while( pBuf < (zBuf + nBuf) ){
    rval = mth$random(&seed);
    bcnt = min(pBuf - (zBuf + nBuf), sizeof(rval));
    memcpy(pBuf, &rval, bcnt);
    pBuf += bcnt;
  }

  return SQLITE_OK;
}

static int vmsSleep(
  sqlite3_vfs *pVfs,        /* VFS record */
  int usec                  /* Time to wait for */
){
  unsigned long long begin;
  unsigned long long delta;
  unsigned long long end;
  int status;

  delta = abs(usec) * -1000;

  sys$gettim(&begin);

  status = sys$setimr(EFN$C_ENF, &delta, 0, 0, 0);
  if( $VMS_STATUS_SUCCESS(status) ){
    sys$hiber();
  }

  sys$gettim(&end);

  return (end - begin) / 1000;
}

/*
** Find the current time (in Universal Coordinated Time).  Write into *piNow
** the current time and date as a Julian Day number times 86_400_000.  In
** other words, write into *piNow the number of milliseconds since the Julian
** epoch of noon in Greenwich on November 24, 4714 B.C according to the
** proleptic Gregorian calendar.
**
** On success, return 0.  Return 1 if the time and date cannot be found.
*/
static int vmsCurrentTimeInt64(
  sqlite3_vfs *pVfs,        /* VFS record */
  sqlite3_int64 *piNow
){
  static const sqlite3_int64 utcEpoch = (sqlite3_int64)198647467200000;
  sqlite3_int64 t[2];

  sys$getutc(t);
  *piNow = utcEpoch + (t[0] / 10000);
  return 0;
}

/*
** Find the current time (in Universal Coordinated Time).  Write the
** current time and date as a Julian Day number into *prNow and
** return 0.  Return 1 if the time and date cannot be found.
*/
static int vmsCurrentTime(sqlite3_vfs *pVfs,
                          double *pNow){
  sqlite3_int64 t = 0;

  vmsCurrentTimeInt64(pVfs, &t);
  *pNow = t / 86400000.0;
  return 0;
}

static int vmsGetLastError(
  sqlite3_vfs *pVfs,        /* VFS record */
  int mxBuf,
  char *zBuf
){
  return 0;
}

int sqlite3_os_init(void){
  static sqlite3_vfs vmsVfs = {
    2,                   /* fVersion */
    sizeof(vmsFile),     /* szOsFile */
#if defined(NAML$C_MAXRSS)
    NAML$C_MAXRSS,       /* mxPathname */
#else
    NAM$C_MAXRSS,        /* mxPathname */
#endif
    0,                   /* pNext */
    "vms",               /* zName */
    0,                   /* pAppData */
    vmsOpen,             /* xOpen */
    vmsDelete,           /* xDelete */
    vmsAccess,           /* xAccess */
    vmsFullPathname,     /* xFullPathname */
    vmsDlOpen,           /* xDlOpen */
    vmsDlError,          /* xDlError */
    vmsDlSym,            /* xDlSym */
    vmsDlClose,          /* xDlClose */
    vmsRandomness,       /* xRandomness */
    vmsSleep,            /* xSleep */
    vmsCurrentTime,      /* xCurrentTime */
    vmsGetLastError,     /* xGetLastError */
    vmsCurrentTimeInt64, /* xCurrentTimeInt64 */
  };

  sqlite3_vfs_register(&vmsVfs, 1);
  return SQLITE_OK;
}

int sqlite3_os_end(void){
  return SQLITE_OK;
}

#endif /* SQLITE_OS_VMS */
