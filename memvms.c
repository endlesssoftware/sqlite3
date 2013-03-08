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
*************************************************************************
**
** This file contains the memory allocation drivers for use when running
** under OpenVMS.
*/
#include "sqliteInt.h"

/*
** This version of the memory allocator is the only one supported on
** OpenVMS, although the others do work.
*/
#ifdef VMS

#include <descrip.h>
#include <lib$routines.h>
#include <libvmdef.h>
#include <ssdef.h>
#include <starlet.h>
#include <stdlib.h>
#include <stsdef.h>

static int _sqliteZone_ = 0;

static void *sqlite3MemMalloc(int nByte){
  int *pNew, szNew = nByte + sizeof(int);
  if( $VMS_STATUS_SUCCESS(lib$get_vm(&szNew, &pNew, &_sqliteZone_)) ){
    *pNew = nByte;
    return ++pNew;
  }
  return 0;
}

static void sqlite3MemFree(void *pPrior){
  int *pOld = ((int *)pPrior) - 1, szOld = *pOld + sizeof(int);

  lib$free_vm(&szOld, &pOld, &_sqliteZone_);
}

static void *sqlite3MemRealloc(void *pPrior, int nByte){
  int *pOld = ((int *)pPrior) - 1, szOld = *pOld;
  void *pNew;

  if( nByte <= szOld ){
    return pPrior;
  } else {
    pNew = sqlite3MemMalloc(nByte);
    if (!pNew) return 0;
    memcpy(pNew, pPrior, szOld);
    sqlite3MemFree(pPrior);
    return pNew;
  }
}

static int sqlite3MemSize(void *pPrior){
  if (!pPrior) return 0;
  return *((int *)pPrior - 1);
}

static int sqlite3MemRoundup(int n){
  return ROUND8(n);
}

static int sqlite3MemInit(void *NotUsed){
  const int alg = LIB$K_VM_QUICK_FIT, arg = 16, extent = 128;
  const int blocksize = 512, smallest = 8;
  const int flags = LIB$M_VM_BOUNDARY_TAGS | LIB$M_VM_EXTEND_AREA
                  | LIB$M_VM_TAIL_LARGE;
  int rc = SQLITE_OK, status;

  $DESCRIPTOR(name, "Sqlite_heap");

  if( _sqliteZone_ == 0){
    status = lib$create_vm_zone(&_sqliteZone_, &alg, &arg, &flags, &extent,
                                &extent, &blocksize, 0, 0, &smallest, &name);
    if( !$VMS_STATUS_SUCCESS(status) ){
      rc = SQLITE_ERROR;
    }
  }

  return rc;
}

static void sqlite3MemShutdown(void *NotUsed){
  lib$delete_vm_zone(&_sqliteZone_);
}

/*
** This routine is the only routine in this file with external linkage.
**
** Populate the low-level memory allocation function pointers in
** sqlite3GlobalConfig.m with pointers to the routines in this file.
*/
void sqlite3MemSetDefault(void){
  static const sqlite3_mem_methods defaultMethods = {
     sqlite3MemMalloc,
     sqlite3MemFree,
     sqlite3MemRealloc,
     sqlite3MemSize,
     sqlite3MemRoundup,
     sqlite3MemInit,
     sqlite3MemShutdown,
     0
  };
  sqlite3_config(SQLITE_CONFIG_MALLOC, &defaultMethods);
}

#endif /* VMS */
