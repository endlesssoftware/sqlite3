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
** This file contains the C functions that implement mutexes for OpenVMS
*/
#include "sqliteInt.h"

/*
** The code in this file is only used if we are compiling threadsafe
** under OpenVMS.  It uses the tis library.
*/
#ifdef SQLITE_MUTEX_VMS

#include <tis.h>

/*
** Each recursive mutex is an instance of the following structure.
*/
struct sqlite3_mutex {
  pthread_mutex_t mutex;     /* Mutex controlling the lock */
};

#define SQLITE3_MUTEX_INITIALIZER(n) { PTHREAD_MUTEX_INITWITHNAME_NP(n, 0) }

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int vmsMutexInit(void){ return SQLITE_OK; }
static int vmsMutexEnd(void){ return SQLITE_OK; }

/*
** The sqlite3_mutex_alloc() routine allocates a new
** mutex and returns a pointer to it.  If it returns NULL
** that means that a mutex could not be allocated.  SQLite
** will unwind its stack and return an error.  The argument
** to sqlite3_mutex_alloc() is one of these integer constants:
**
** <ul>
** <li>  SQLITE_MUTEX_FAST
** <li>  SQLITE_MUTEX_RECURSIVE
** <li>  SQLITE_MUTEX_STATIC_MASTER
** <li>  SQLITE_MUTEX_STATIC_MEM
** <li>  SQLITE_MUTEX_STATIC_MEM2
** <li>  SQLITE_MUTEX_STATIC_PRNG
** <li>  SQLITE_MUTEX_STATIC_LRU
** <li>  SQLITE_MUTEX_STATIC_PMEM
** </ul>
**
** The first two constants cause sqlite3_mutex_alloc() to create
** a new mutex.  The new mutex is recursive when SQLITE_MUTEX_RECURSIVE
** is used but not necessarily so when SQLITE_MUTEX_FAST is used.
** The mutex implementation does not need to make a distinction
** between SQLITE_MUTEX_RECURSIVE and SQLITE_MUTEX_FAST if it does
** not want to.  But SQLite will only request a recursive mutex in
** cases where it really needs one.  If a faster non-recursive mutex
** implementation is available on the host platform, the mutex subsystem
** might return such a mutex in response to SQLITE_MUTEX_FAST.
**
** The other allowed parameters to sqlite3_mutex_alloc() each return
** a pointer to a static preexisting mutex.  Six static mutexes are
** used by the current version of SQLite.  Future versions of SQLite
** may add additional static mutexes.  Static mutexes are for internal
** use by SQLite only.  Applications that use SQLite mutexes should
** use only the dynamic mutexes returned by SQLITE_MUTEX_FAST or
** SQLITE_MUTEX_RECURSIVE.
**
** Note that if one of the dynamic mutex parameters (SQLITE_MUTEX_FAST
** or SQLITE_MUTEX_RECURSIVE) is used then sqlite3_mutex_alloc()
** returns a different mutex on every call.  But for the static
** mutex types, the same mutex is returned on every call that has
** the same type number.
*/
static sqlite3_mutex *vmsMutexAlloc(int iType){
  static sqlite3_mutex staticMutexes[] = {
    SQLITE3_MUTEX_INITIALIZER("SQLITE3_MUTEX_STATIC_MASTER"),
    SQLITE3_MUTEX_INITIALIZER("SQLITE3_MUTEX_STATIC_MEM"),
    SQLITE3_MUTEX_INITIALIZER("SQLITE3_MUTEX_STATIC_MEM2"),
    SQLITE3_MUTEX_INITIALIZER("SQLITE3_MUTEX_STATIC_PRNG"),
    SQLITE3_MUTEX_INITIALIZER("SQLITE3_MUTEX_STATIC_LRU"),
    SQLITE3_MUTEX_INITIALIZER("SQLITE3_MUTEX_STATIC_PMEM")
  };
  sqlite3_mutex *p;
  switch( iType ){
    case SQLITE_MUTEX_RECURSIVE: {
      p = sqlite3MallocZero( sizeof(*p) );
      if( p ){
#ifdef vax
        /*
        ** On OpenVMS VAX there is no way to create a recursive mutex in
        ** the tis interface.  The routine tis_mutex_initwithname is in
        ** the header file and claims to enable this.  However, it does
        ** not actually exist in the CMA$TIS_SHR RTL.  So, while the
        ** jiggery-pokery of the lock field in pthread_mutex_t below is
        ** completely unsupported it does, in fact, seem to work.
        */
        tis_mutex_init(&p->mutex);
        p->mutex.lock &= ~_PTHREAD_MSTATE_TYPE;     /* Clear mutex type */
        p->mutex.lock |= _PTHREAD_MTYPE_RECURS;     /* Set mutex type to recursive */
#else
        /*
        ** Although not documented, this is the routine used by the
        ** CRTL on Alpha and I64 to implement the flock() routine.
        */
        tis_mutex_init_type(&p->mutex, PTHREAD_MUTEX_RECURSIVE, 0);
#endif
      }
      break;
    }
    case SQLITE_MUTEX_FAST: {
      p = sqlite3MallocZero( sizeof(*p) );
      if( p ){
        tis_mutex_init(&p->mutex);
      }
      break;
    }
    default: {
      assert( iType-2 >= 0 );
      assert( iType-2 < ArraySize(staticMutexes) );
      p = &staticMutexes[iType-2];
      break;
    }
  }
  return p;
}


/*
** This routine deallocates a previously
** allocated mutex.  SQLite is careful to deallocate every
** mutex that it allocates.
*/
static void vmsMutexFree(sqlite3_mutex *p){
  tis_mutex_destroy(&p->mutex);
  sqlite3_free(p);
}

/*
** The sqlite3_mutex_enter() and sqlite3_mutex_try() routines attempt
** to enter a mutex.  If another thread is already within the mutex,
** sqlite3_mutex_enter() will block and sqlite3_mutex_try() will return
** SQLITE_BUSY.  The sqlite3_mutex_try() interface returns SQLITE_OK
** upon successful entry.  Mutexes created using SQLITE_MUTEX_RECURSIVE can
** be entered multiple times by the same thread.  In such cases the,
** mutex must be exited an equal number of times before another thread
** can enter.  If the same thread tries to enter any other kind of mutex
** more than once, the behavior is undefined.
*/
static void vmsMutexEnter(sqlite3_mutex *p){
  tis_mutex_lock(&p->mutex);
}

static int vmsMutexTry(sqlite3_mutex *p){
  if( pthread_mutex_trylock(&p->mutex)==0 ){
    return SQLITE_OK;
  }

  return SQLITE_BUSY;
}

/*
** The sqlite3_mutex_leave() routine exits a mutex that was
** previously entered by the same thread.  The behavior
** is undefined if the mutex is not currently entered or
** is not currently allocated.  SQLite will never do either.
*/
static void vmsMutexLeave(sqlite3_mutex *p){
  tis_mutex_unlock(&p->mutex);
}

sqlite3_mutex_methods const *sqlite3DefaultMutex(void){
  static const sqlite3_mutex_methods sMutex = {
    vmsMutexInit,
    vmsMutexEnd,
    vmsMutexAlloc,
    vmsMutexFree,
    vmsMutexEnter,
    vmsMutexTry,
    vmsMutexLeave,
    0,
    0
  };

  return &sMutex;
}

#endif /* SQLITE_MUTEX_VMS */
