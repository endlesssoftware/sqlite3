	SQLite for OpenVMS

This is a native port of the SQLite database package to OpenVMS.  It delivers
the SQLite database to OpenVMS using the following native features:

    * Thread support using the tis library.  This allows support for
      multi-threading without having to link against the pthreads RTL.

    * Direct file access.  All files access is performed using the $QIO
      system services, rather than the C RTL or even RMS.

    * Native locking.  All locking is handled using the OpenVMS distributed
      lock manager, allowing database access to be coordinated across
      cluster nodes (of all architectures).

Despite these OpenVMS-specific improvements the database file maintained
by SQLite is still portable to other SQLite-based applications running on
other systems.
