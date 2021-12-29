# lib_nas_lockfile

API for using lockfiles on local and remote storage in a pluriform network environment / NAS / shared file system. 

Includes staleness checks to recover after a lock-holding node/application crashed or network failure ('force majeure' fatalities).

## When to use this library

When you are not in a position where you and your software can demand a rigorous DeVOps environment,
a network node *monoculture* (i.e. all connected stuff running the same OS and version thereof) or when
you don't have access to a central 'monitor' server which takes care of all the extra-node locking requests
that come with your nodes' demands to access the networked file system.

In practice, candidates for using this library include:
- networked software running on home networks, which include NAS and/or Cloud Storage where files are stored and *shared* among the nodes,
- software designed for multi-user environments, where you cannot **guarantee** every operation is properly protected by a network server or other 'monitor' means, which handles lock requests at a central node (or other specialized network locking facilities are absent).
- software offering remote backup storage and/or data sync facilities, which MAY be synced from multiple locations and/or user nodes: a looser "collaboration" scenario
- anywhere your software cannot depend on properly managed network tokens to provide dependable locking mechanisms and you end up having to design your software using the least common denominator in network sharing: a shared file system where you store your stuff for others to pick up and continue working on.
- anywhere where you havee a shared file system that only "up" part of the time and you have to reckon with a set of nodes which will be "off net" part of the time; you use the shared, networked, file system for synchronizing the nodes and need file locking for that part while doing the syncing. Recoverable locking mechanisms mey be part of your requirements.
- you are running stuff on top of NFS, SMB ("Samba"), what-have-you and you need something that doesn't depend on Linux-only `flock()` calls and other highly non-portable !@#$%^&.

This library (rather: C++ class) provides a solid "old school" Least Common Denominator locking system for shared file systems.
This library includes "Lock Staleness Detection" as a built-in feature to help you recover from spurious 'force majeure' network failures and other types of mishap that can corrupt/break a regular file-system based locking mechanism: once the library code detects a Stale Lock, it is automatically cleaned up and released, so the software may pick up and recover from there without 
needing an administrator with root privs to manually clean the lock or perform other manual actions to allow the system to continue.

**Do** keep in mind that a Stale Lock always signals a grave 'force majeure' situation, e.g. crashed node, locked-up unrecoverable application instance, network failure, etc. and this will be signalled by the library in the error codes for the various APIs: you are **strongly advised to check and log all error response values produced by the library**: while the software **can** be written to auto-recover from these situations, a human should always be able to come in and inspect the proceedings at that point (and some time before and afterwards) to analyze and report on the reliability and soundness of the system as a whole.

While everyone would dearly like to work on other stuff than semaphores and the like, I would advise the developers using any locking primitives, whether this one, pthread or whatever floats your boat, to always code-review such libraries: locking is a critical mechanism is any software design that needs them and some extra RTFC effort at the beginning will save y'all some pain in the future. I know it has been the case for me over the years and this is why I do not accept any closed source libraries critical to a software project any more: those have cost me dearly over time.

## What do you mean: Least Common Denominator in FileSystem Networking?

I hope you enjoy what you find here. While there are various options out there, those are often `flock()`/`fcntl()` based and thus assume a rather monocultural network (Linux machines only and *pray to the gods* they are all synchronized to the same OS version).

*This* library is designed, relying on a single assumption that holds for almost all filesystems out there: **mkdir() is atomic**.

That is, in my experience, the Least Common Denominator in various file sharing and mixxed OS environments. If your `mkdir()` is **not atomic** (e.g. DropBox, Google Drive, etc.), then you are, as the Americans say, SOL. Shit Out \[of] Luck.

> If you know of ways to provide an atomic locking operation on thosee kinds of networks, I'm all ears. File an issue and provide some info in the issue tracker for this repo.
> 
> Meanwhile, I will reamin with my belief communications through such 'networks' require different measures: there, one would probably get away with a paradigm similar to message-passing, where each node has its own transmitter station and you code a simple generic 'station discovery' mechanism.
> 
> That aside, this library is targetting NAS (Network Attached Storage) systems and similar file system networks, where the above assumption will hold.
> 





# API

All locking logic is contained in a single, simple C++ class:

```C++
namespace NetworkedFileIO {
  NASLockFile(const std::string remote_directory_path) {
    ...
  }
}
```

All API methods return boolean state values and will set the referenced `std::error_code` error value,
where applicable.

We do not use exceptions as we consider those only useful for handling spurious or otherwise undesirable
code flow: with locks, (almost) nothing is out of the ordinary and should, IMO, not be dealt with
using exceptions when a perfectly fine state + error checking mechanism can provide code with a clear
code flow for both good (desirable) and undesirable situations, the latter expected to happen often enough
to be part of the 'normal' with locking systems, where you cannot do happy-go-lucky haphazard coding,
which is so often observed in business applications.

Rant aside, how are the APIs organized?

There's the ubiquitous lock+release calls `create_lock()` + `delete_lock()`. Obvious stuff.

As we offer implicit Staleness Detection logic, we also need a Watchdog (which you will be **kicking** frequently to ensure everyone around can observe your lock(s) to be **alive** rather than **stale**), plus a monitor API where a node can observe a lock and determine whether the lock has become available and/or has become **stale** -- in which case the monitor routine will automatically clean up the stale lock and release it, so the regular lock-acquisition logic can run its course in all nodes once again: those are `refresh_lock()` for kicking the watchdog for the lock you acquired and `monitor_for_lock_availability()` for checking availability and staleness of a lock.

Root/administrative support APIs have been provided as well, which are expected to be used in very specific, human-controlled, tools which will clean up a lock after a 'force majeure' event which either cannot be automatically recovered or when the system administrator decides all nodes and locks need to be *reset* to their default state, e.g. after a fresh software install across the board and the administrator and software designer involved do not wish to rely on the lock staleness auto-recovery mechanism, but go in and reset everything for all to give everyone a fresh clean start. This is the `nuke_stale_lock()` API.

As an additional diagnostic assistant service for special logging and other purposes, the `active_lockfile()` API is provided to list the path of the currently acquired file lock.

> The public `override_chrono_clock_beat_multiplier()`, `reset_chrono_clock_beat_multiplier_override()` and `get_chrono_clock_beat_multiplier()` APIs have been provided for (unit)test purposes only and SHOULD NOT be used in your software: these merely provide a machanism to "speed up" the "*staleness timeouts*" employed by the library.


## Does this library reckon with race conditions, fringe cases and other "weird shit" one will observe in the wild with systems like these?

Yes, we do.

For example, the Staleness Detection Logic has been engineered to counter the scenario where multiple monitoring nodes decree a observed lock as **stale** all at the same time: an **aditional, yet independent** lock is used to ensure only one of these nodes is allowed to provide the cleanup-after-staleness procedure to ensure everyone is and remains in a proper known state regarding the (stale) lock.

We also have reckoned with the race condition which can occur when you go and clean/release a stale lock, while multiple nodes are monitoring said lock for a chance to request and obtain a lock on it themselves: this is why `monitor_for_lock_availability()` only ever reports **opportunity to request a lock**: since we assume only `mkdir()` is atomic, we MUST separate the Staleness Cleanup (and consequent lock **release** as (final) part of that cleanup) from any lock request activity: while the `monitor_for_lock_availability()` API **MAY** report an **opportunity to request a lock successfully**, it never guarantees such success: `create_lock()` will either succeed or fail and when you need that lock, you will then have to cycle and retry again later.

> Of course you may repeatedly call `create_lock()` without any call to `monitor_for_lock_availability()`; the latter is provided as additional help and we advise you to use it when you do not have the lock yet to ensure your node will actively monitor the lock for staleness and help resolve such a situation iff it occurs.
> 

Another fringe case we have provided for is the **highly undeesirable** situation where a software allows its lock to go stale by not kicking the watchdog or by being extrmely *lazy* about it: when this happens, your `delete_lock()` call will observe this and report an error accordingly. Which, if you otherwise followed proper coding practices, i.e. check your success/fail return and error codes rigorously, are able to at least log and thus notify the user and/or investigator who will have a look at the logfiles of your application after the fact.

## API list

- `create_lock()`: create the lockfile. Return failure when lockfile is already locked by others or otherwise cannot be created.
- `delete_lock()`: delete the lockfile we own.
- `refresh_lock()`: update the lockfile to signal *freshness* of the lock to everyone monitoring the lock, i.e. "kicking the watchdog" in embedded dev parlance.
- `monitor_for_lock_availability()`: periodically checks the lockfile to discover *staleness* (the **opposite of freshness**) or *absence* of the lockfile, i.e. *wait for opportunities to create the lockfile*. 
- `nuke_stale_lock()`: delete the lockfile when it has been found to be *stale* (by `monitor_for_lock_availability()`). Return failure when the lockfile could not *legally* be cleaned up. Return warning when the lockfile has already been cleaned (*refreshed* or *nuked*) by another application / network node.


## Design Requirements of this library

(TBD)

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

### Sideways related material

Places where the non-local file locking problem and its consequences is discussed in practical environments.

- https://docs.microsoft.com/en-us/windows-server/troubleshoot/understanding-the-lack-of-distributed-file-locking-in-dfsr :: MSWindows: Understanding (the Lack of) Distributed File Locking in DFSR
- https://docs.microsoft.com/en-us/windows/win32/fileio/locking-and-unlocking-byte-ranges-in-files :: Locking and Unlocking Byte Ranges in Files
- https://support.centrestack.com/hc/en-us/articles/360041243194-How-the-CentreStack-File-Locking-Mechanism-Works
- https://docs.microsoft.com/en-us/rest/api/storageservices/managing-file-locks
- https://apenwarr.ca/log/20101213 :: Everything you never wanted to know about file locking
- https://en.wikipedia.org/wiki/File_locking
- https://sqlite.org/lockingv3.html :: File Locking And Concurrency In SQLite Version 3







# Rationale

TBD
........................................
the lockfile mechanism uses `mkdir()` as an atomic operation (see section **Rationale** below for an elaboration). Hence y
