# lib_nas_lockfile

API for using lockfiles on local and remote storage in a pluriform network environment. 

Includes staleness checks to recover after a lock-holding node/application crashed.


# API

- `create_lock()`: create the lockfile. Return failure when lockfile is already locked by others or otherwise cannot be created.
- `delete_lock()`: delete the lockfile we own.
- `refresh_lock()`: update the lockfile to signal *freshness* of the lock to everyone monitoring the lock, i.e. "kicking the watchdog" in embedded dev parlance.
- `monitor_for_lock_availability()`: periodically checks the lockfile to discover *staleness* (the **opposite of freshness**) or *absence* of the lockfile, i.e. *wait for opportunities to create the lockfile*. 
- `nuke_stale_lock()`: delete the lockfile when it has been found to be *stale* (by `monitor_for_lock_availability()`). Return failure when the lockfile could not *legally* be cleaned up. Return warning when the lockfile has already been cleaned (*refreshed* or *nuked*) by another application / network node.

## Requirements

- All nodes / applications sharing the same lockfile MUST use the same refresh interval ± 25% \[*Tolerance includes round-trip network latency*]

- The refresh interval and **staleness factor** (used by `monitor_for_lock_availability()`) MUST be chosen so network latency cannot negatively impact lockfile monitoring, which would lead `monitor_for_lock_availability()` to **prematurely** conclude the lockfile *stale*.

  An example to clarify: say your network server (NAS or other) where the lockfile will be stored has an estimated worst-case round-trip response time of 200ms for filesystem I/O -- this includes multiple network messages and server disk I/O operations! this response time is the *total time* it takes to receive a result for a filesystem operation such as `mkdir` -- and your refresh interval is set to 300ms, then the *staleness factor* must be set to .................FORMULA............................

- Your application MUST have directory and file creation, edit and delete rights for the path specified where the 'lockfile' will be created.



## Notes

- The *lockfile* is constructed from a *lock directory* (created atomically by `mkdir()`, see section **Rationale** below for an elaboration) and a regular file created within that subdirectory for we can easily update the *mtime* of a file, but updating the *mtime* of a directory is a little haphazard across filesystems & operating systems.

- *staleness* is detected by monitoring the *mtime* of the lockfile. Because many filesystem network systems which are not professionally managed and monitored 24/7 can suffer from 'out of sync' clocks, we monitor for *mtime* **change** to observe *freshness* as we cannot rely on the absolute timestamp itself. 

  > Though this particular problem is gradually reduced over time as more systems implicitly use NTP to synchronize their local clock to a master beat without the need for administrative effort. Nevertheless, this issue has been observed on several household and semi-professional NAS installations and MUST NOT cause this locking system to fail with hard-to-diagnose failure modes such as incorrect staleness detection and consequent lock removal action.
  
- The *nuke* operation, which aims to remove a *stale* lock, has several potential race conditions, all of which are accounted for in the library code:   
  - two or more network nodes may detect *staleness* at the same time and take action simultaneously. Hence *nuking* can only be done *after* we have acquired another lock! (a.k.a. the *nuke lockfile*)
  - when the node which acquired the *nuke lock* crashes or otherwise fails to release *that* lockfile, we now have a recursive staleness detection problem!
  - [CWE-367: Time-of-check Time-of-use (TOCTOU) Race Condition](https://cwe.mitre.org/data/definitions/367.html): when you use the API to check for *staleness* (`monitor_for_lock_availability()`) and then call the `nuke_stale_lock()` action, someone else MAY have cleaned up already! *And* acquired a fresh lock already as well! Hence `nuke_stale_lock()` must acquire a *nuke lock* before re-checking the freshness of the filelock that was suspected to be *stale*, and only when it finds thelockfile is *still* stale, *nuke* it.



# Inspiration

- https://github.com/moxystudio/node-proper-lockfile (well thought out; some great ideas besides the regular list of `flock()`, `fcntl()`, etc. file locking story that's been developed since the days of NFS, SAMBA and flaky network filesystems)
- https://github.com/judecumt/node-lockfile
- https://github.com/ccache/ccache (LockFile class)
- https://github.com/miquels/liblockfile
- https://github.com/michael-uman/lockfile


## R&D

- https://github.com/bobvanderlinden/sharpfilesystem/issues/8#issuecomment-237612512 :: Atomic file operations: Replace, AtomicMove
- http://perl.plover.com/yak/flock/ :: presentation File Locking Tricks and Traps
- https://stackoverflow.com/questions/1599459/optimal-lock-file-method/52478000#52478000 :: race conditions on remove(), etc.
- https://keithmaggio.wordpress.com/2011/01/24/code-snippet-file-locking/
- https://stackoverflow.com/questions/14132767/win32-c-c-read-data-from-a-locked-file
- https://cpp.hotexamples.com/examples/-/-/flock/cpp-flock-function-examples.html
- https://stackoverflow.com/questions/472329/how-can-i-synchronize-two-processes-accessing-a-file-on-a-nas
- https://en.wikipedia.org/wiki/Double-checked_locking
- http://nfs.sourceforge.net/ :: NFS FAQ : A9. Why does opening files with O_APPEND on multiple clients cause the files to become corrupted? & other parts
- https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/locking?view=msvc-160
- https://stackoverflow.com/questions/581219/find-out-who-is-locking-a-file-on-a-network-share
- https://www.linuxquestions.org/questions/programming-9/locking-files-with-c-751872/
- https://stackoverflow.com/questions/2057784/locking-files-in-linux-with-c-c
- https://stackoverflow.com/questions/575328/fcntl-lockf-which-is-better-to-use-for-file-locking
- https://stackoverflow.com/questions/307437/moving-a-directory-atomically
- https://rcrowley.org/2010/01/06/things-unix-can-do-atomically.html
- https://www.sqlite.org/lockingv3.html :: section 6.0 How To Corrupt Your Database Files & other spots
- https://ozlabs.org/~rusty/index.cgi/tech/2009-10-20.html :: EXT barrier=1
- https://stackoverflow.com/questions/18585029/which-hdfs-operations-are-atomic
- https://yakking.branchable.com/posts/atomic-file-creation-tmpfile/
- https://yakking.branchable.com/posts/clobbering/
- https://docs.oracle.com/javase/tutorial/essential/io/fileOps.html#atomic :: section "Atomic Operations"
- https://github.com/ansible/ansible/issues/67794 :: The default permissions used by atomic_move can create files that are world readable
- https://sqlite.org/atomiccommit.html
- https://lwn.net/Articles/789600/ :: A way to do atomic writes
- https://www.usenix.org/conference/fast15/technical-sessions/presentation/verma :: Failure-Atomic Updates of Application Data in a Linux File System
- https://en.cppreference.com/w/cpp/filesystem/rename :: std::filesystem::rename
- https://pubs.opengroup.org/onlinepubs/9699919799/functions/rename.html :: rename, renameat - rename file
- https://pubs.opengroup.org/onlinepubs/9699919799/functions/contents.html :: ToC. See mkdir & other stdlib API refs in that list
- https://pubs.opengroup.org/onlinepubs/9699919799/functions/mkdir.html#tag_16_325
- https://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html#tag_16_357
- https://exceptionshub.com/how-do-i-lock-files-using-fopen.html
- https://www.ibm.com/docs/en/i/7.4?topic=ssw_ibm_i_74/apis/fcntl.htm#HDRFCNLOC :: fcntl() -- Perform File Control Command
- https://docs.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex :: LockFileEx function (fileapi.h)
- https://www.gnu.org/software/libc/manual/html_node/File-Locks.html :: section 13.16 File Locks
- https://unix.stackexchange.com/questions/20756/how-do-you-make-a-cross-process-locking-in-linux-c-c
- https://www.cplusplus.com/forum/general/11124/ :: File locking for read and write
- https://docs.microsoft.com/en-us/windows-server/troubleshoot/understanding-the-lack-of-distributed-file-locking-in-dfsr :: Understanding (the Lack of) Distributed File Locking in DFSR
- https://docs.microsoft.com/en-us/windows/win32/fileio/opportunistic-locks
- https://bytes.com/topic/c/answers/212960-file-locking
- https://www.thegeekstuff.com/2012/04/linux-file-locking-types/
- https://www.codeproject.com/articles/49670/inter-computer-read-write-file-lock
- https://www.codeproject.com/questions/568056/mutexplusnotplusworkingplusforpluscross-processplu






# Rationale

TBD
........................................
the lockfile mechanism uses `mkdir()` as an atomic operation (see section **Rationale** below for an elaboration). Hence y
