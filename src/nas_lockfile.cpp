
#include "nas_lockfile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

#include <sys/types.h>
#include <sys/stat.h>

namespace fs = std::filesystem;
using namespace std;

using namespace NetworkedFileIO;



const char* NASLockFileErrCategory::name() const noexcept
{
	return "NAS.FileLock";
}



using E = NASLockFileErrorCode;

std::string NASLockFileErrCategory::message(int ev) const
{
	switch (static_cast<NASLockFileErrorCode>(ev))
	{
	case NASLockFileErrorCode::OK:
		return "no error / all hunky dory";

	case NASLockFileErrorCode::ELockOwnedByOthers:
		return "Lock is already owned by another party";

	case NASLockFileErrorCode::EWeHaveNoLock:
		return "we have not acquired a lock previously, while the API requires us to already";

	case NASLockFileErrorCode::ELostLockWatchdogFile:
		return "the lock's watchdog file is lost due to spurious external circumstances";

	case NASLockFileErrorCode::ELostLock:
		return "the lock has been lost due to spurious external circumstances";

	case NASLockFileErrorCode::EBasePathIsNotADirectory:
		return "the base path for the NAS LockFile System is not a directory";

	case NASLockFileErrorCode::EWeAlreadyAcquiredTheLock:
		return "we have already acquired the lock previously";

	case NASLockFileErrorCode::ESpuriousStalenessRecoveryError:
		return "a spurious non-recoverable error occurred during lock staleness recovery. (Advice. DANGEROUS! You may need to check your remote storage and manually clear locks AFTER HAVING MADE SURE ALL NODES ARE INACTIVE!)";

	case NASLockFileErrorCode::WOptimizedOut:
		return "WARNING/INFO: API call has been optimized out";

	default:
		return "(unrecognized error)";
	}
}



const NASLockFileErrCategory theNASLockFileErrCategory{};

std::error_code make_error_code(NASLockFileErrorCode e)
{
	return { static_cast<int>(e), theNASLockFileErrCategory };
}





NASLockFile::NASLockFile(const std::string remote_directory_path)
	: m_remote_directory_path(std::move(remote_directory_path)),
	m_lock_obtained(false),
	m_lockfile_path(""),
	m_stale_recovery_lockfile_path(""),
	m_watchdogfile_path(""),
	m_next_write_time_kick(),
	m_local_clock_at_lock_creation(),
	m_file_time_at_lock_creation(),
	m_clock_vs_filetime_diff(0),
	m_monitoring_active(false),
	m_monitoring_watchdog_file_exists(false),
	m_lost_lock_due_to_external_circumstances(false),
	m_monitoring_last_watchdog_kick_time(),
	m_monitoring_time_of_last_observed_change(),
	m_clock_beat_override(0)
{
	m_lockfile_path                = m_remote_directory_path / "__lock.lockdir";
	m_stale_recovery_lockfile_path = m_remote_directory_path / "_stale.lockdir";

	m_watchdogfile_path = m_lockfile_path / "lock.watchdog";
}



NASLockFile::~NASLockFile()
{
}



// this chunk is invoked when we have just acquired the lock (regular locking)
// OR wish to recover it by acquiring it fully (staleness recovery)
bool NASLockFile::setup_watchdog_file(std::error_code& ec)
{
	m_lock_obtained = true;
	m_lost_lock_due_to_external_circumstances = false;

	// decorate the lock & prep for anti-staleness watchdog activity afterwards

	// when we get here, we've got the lock as the MKDIR operation succeeded.
	//
	// Now we create a little 'watchdog' file inside that lock directory to easily
	// track "freshness": it's easy to track MTIME for a regular file.
	//
	// BEFORE we do THAT though, we attempt to restrict the lock directory access to owner+group
	// for that bit of extra security. :-)
	try
	{
		// security: remove world-write rights: set to '-rwxrwx---'
		// 
		// If we fail to accomplish that, so be it, ignore that. It's not that important anyway.
		fs::permissions(m_lockfile_path, fs::perms::owner_all | fs::perms::group_all, fs::perm_options::replace, ec);

		filesystem::path watchdogfile_path = m_lockfile_path / "lock.watchdog";
		std::ofstream(watchdogfile_path).write("locked", 6);

		// Note: some file system report last write time as local time, 
		// while others (eg. NTFS) report it as UTC.
		// We take care of both any time differences like that AND the bothersome
		// fact that we cannot easily convert a system time to file_time_type in C17.
		// 
		// So we track the local clock and use that to update the file MTIME via
		// tracked difference (see refresh_lock() method further below).
		m_file_time_at_lock_creation = m_monitoring_last_watchdog_kick_time = fs::last_write_time(watchdogfile_path);
		m_local_clock_at_lock_creation = m_monitoring_time_of_last_observed_change = chrono::system_clock::now();
		m_clock_vs_filetime_diff = int64_t(m_local_clock_at_lock_creation.time_since_epoch().count()) - int64_t(m_file_time_at_lock_creation.time_since_epoch().count());

		// set up the watchdog beat frequency expectation:
		using namespace std::literals; // enables the usage of 24h, 1ms, 1s instead of e.g. std::chrono::hours(24), accordingly
		using namespace std::chrono_literals;
		// Note to self: write it with these steps or delta_µs will be zero for any multiplier > 10.  :-(((
		chrono::microseconds delta_µs = 10s;
		delta_µs /= get_chrono_clock_beat_multiplier();
		m_next_write_time_kick = m_local_clock_at_lock_creation + delta_µs;

#if 0
		auto print_last_write_time = [](fs::file_time_type const& ftime) {
			std::time_t cftime = std::chrono::system_clock::to_time_t(
				std::chrono::file_clock::to_sys(ftime));
			std::cout << "File write time is " << std::asctime(std::localtime(&cftime));
		};

		print_last_write_time(ftime);
#endif
	}
	catch (fs::filesystem_error& e)
	{
		std::cout << "Could not set up lock properly: " << e.what() << '\n';

		cleanup_after_ourselves(ec);
	}

	return m_lock_obtained;
}



bool NASLockFile::cleanup_after_ourselves(std::error_code& ec)
{
	m_lock_obtained = false;
	m_monitoring_active = false; // RESET monitoring phase state as monitoring is only sane during those times we DO NOT have a lock

	if (m_lost_lock_due_to_external_circumstances)
	{
		ec = make_error_code(E::ELostLock);
		return false;
	}

	// 2. atomic operation: release lock?

	// fs::remove_all() is expected to have as an atomic operation
	// as the very last command in there should be the remove/unlock
	// system call which deletes the lock directory.
	//
	// we use fs::remove_all() because that one adds simplicity:
	// the lock watchdog file and any other cruft is deleted first,
	// without us having to worry about it.
	if (!fs::remove_all(m_lockfile_path, ec))
	{
		// ignore the error per se...
#if 0
		std::cout << "Could not set up/release the lock properly: " << ec.category().name() << "::" << ec.message() << '\n';
#endif
		return false;
	}
	return true;
}



// create = obtain the lockfile.
// Return failure when lockfile is already locked by others or otherwise cannot be created.
bool NASLockFile::create_lock(std::error_code& ec)
{
	// 1. prep work

	if (m_lost_lock_due_to_external_circumstances)
	{
		ec = make_error_code(E::ELostLock);
		return false;
	}

	if (m_lock_obtained)
	{
		// already locked by us
		ec = make_error_code(E::EWeAlreadyAcquiredTheLock);
		return true;
	}

	// make sure the base directory exists; ignore any errors this may report.
	fs::create_directories(m_remote_directory_path, ec);
	// now check if we're actually looking at a regular directory;
	// the only exception being we specify a drive root as base directory, e.g. "W:\"
	if (!fs::is_directory(m_remote_directory_path, ec))
	{
		ec = make_error_code(E::EBasePathIsNotADirectory);
		return false;
	}

	// 2. atomic operation: acquire lock?

	// guaranteed atomic FS operation (as far as guarantees go...)
	if (!fs::create_directory(m_lockfile_path, ec))
	{
		// diagnose the error and recover
		switch (ec.value())
		{
		case 0: // SUCCESS: that's when fs::create_directory() decides the directory already exists, but considers that to be OK (fstat-->OK) in Win32!
		case EEXIST:
			// locked by someone else out there.
			// Do not touch.
			ec = make_error_code(E::ELockOwnedByOthers);
			break;
		}
		return false;
	}
	else
	{
		m_monitoring_active = false; // RESET monitoring phase state as monitoring is only sane during those times we DO NOT have a lock

		// 3: decorate the lock & prep for anti-staleness watchdog activity afterwards
		return setup_watchdog_file(ec);
	}
}

// delete the lockfile we own.
bool NASLockFile::delete_lock(std::error_code& ec)
{
	// 1. prep work

	m_monitoring_active = false; // RESET monitoring phase state as monitoring is only sane during those times we DO NOT have a lock

	if (m_lost_lock_due_to_external_circumstances)
	{
		m_lost_lock_due_to_external_circumstances = false;
		m_lock_obtained = false;

		ec = make_error_code(E::ELostLock);
		return false;
	}

	if (!m_lock_obtained)
	{
		// that lock isn't even owned by us
		ec = make_error_code(E::EWeHaveNoLock);
		return false;
	}

	// and before we go and release the lock, we do a quick last check to see if our lock
	// somehow got damaged or went "stale", e.g. when the userland code did not kick the watchdog timely.
	// If we don't do this last check here, before releasing the lock, chances are we may never know
	// our lock went stale (for whatever reason), making for very hard to diagnose bugs in RL (Real Life: production code)!
	// Hence this last check before we release the lock: the minimal cost is quite acceptable...
	//
	// CAVE CANEM: when another process has already discovered that this lock was stale and has acquired
	// its own lock, a simple 'exist' check won't suffice as the watchdog lockfile will be present and
	// we will be releasing a lock already held by someone else.
	// Hence the stringent check where we double-check to make sure the watchdog file 'last edit' time
	// matches the one we recall from the last time we kicked the bugger...

	// make sure we have a legal watchdog file waiting for us.
	if (!fs::is_regular_file(m_watchdogfile_path, ec))
	{
		// Note: we are already intent on releasing the lock, so no use setting the state variable.
		// We DO, however, set the appropriate error codes so error checks and subsequent logging
		// in userland code will catch this situation.
		m_lost_lock_due_to_external_circumstances = false;
		m_lock_obtained = false;

		ec = make_error_code(E::ELostLockWatchdogFile);

		// shite. Did we loose the lock as well? That would be REALLY bad!
		if (!fs::exists(m_lockfile_path))
		{
			ec = make_error_code(E::ELostLock);
		}
		return false;
	}

	std::filesystem::file_time_type wd_t = fs::last_write_time(m_watchdogfile_path, ec);
	auto diff = wd_t - m_monitoring_last_watchdog_kick_time;
	// did we observe MTIME change?
	// If so, then SOMEONE ELSE has kicked our watchdog and we clearly have already lost the lock
	// due to staleness -- we haven't kicked the watchdog timely enough, or so it seems.
	// Barf a hairball.
	if (diff.count() != 0)
	{
		ec = make_error_code(E::ELostLock);
		return false;
	}

	// 2. atomic operation: release lock?
	return cleanup_after_ourselves(ec);
}



// update the lockfile to signal *freshness* of the lock to everyone monitoring the lock,
// i.e. "kicking the watchdog" in embedded dev parlance.
bool NASLockFile::refresh_lock(std::error_code& ec)
{
	// 1. only kick the watchdog on a lock we own:

	if (m_lost_lock_due_to_external_circumstances)
	{
		ec = make_error_code(E::ELostLock);
		return false;
	}

	if (!m_lock_obtained)
	{
		ec = make_error_code(E::EWeHaveNoLock);
		return false;
	}

	m_monitoring_active = false; // RESET monitoring phase state as monitoring is only sane during those times we DO NOT have a lock

	// optimization: check the current time and only kick the dog when we're AT or PAST the kick alarm time:
	chrono::system_clock::time_point t = chrono::system_clock::now();
	if (t < m_next_write_time_kick)
	{
		// no need to do (relatively costly) filesystem I/O yet to kick the watchdog file
		ec = make_error_code(E::WOptimizedOut);
		return false;
	}
	// when we get here, we know we're at or past our due date and MUST kick the watchdog!

	// extra validation before we do what we came to do:
	// make sure we have a legal watchdog file waiting for us.
	if (!fs::is_regular_file(m_watchdogfile_path, ec))
	{
		m_lost_lock_due_to_external_circumstances = true;
		ec = make_error_code(E::ELostLockWatchdogFile);

		// shite. Did we loose the lock as well? That would be REALLY bad!
		if (!fs::exists(m_lockfile_path))
		{
			ec = make_error_code(E::ELostLock);
		}
		return false;
	}

	// CAVE CANEM: when another process has already discovered that this lock was stale and has acquired
	// its own lock, the simple 'exist' check above won't suffice as the watchdog lockfile will be present and
	// we will be kicking a lock watchdog already owned by someone else.
	// Hence the stringent check below where we double-check to make sure the watchdog file 'last edit' time
	// matches the one we recall from the last time we kicked the bugger...

	std::filesystem::file_time_type wd_t = fs::last_write_time(m_watchdogfile_path, ec);
	auto mtime_diff = wd_t - m_monitoring_last_watchdog_kick_time;
	// did we observe MTIME change?
	// If so, then SOMEONE ELSE has kicked our watchdog and we clearly have already lost the lock
	// due to staleness -- we haven't kicked the watchdog timely enough, or so it seems.
	// Barf a hairball.
	if (mtime_diff.count() != 0)
	{
		ec = make_error_code(E::ELostLock);
		return false;
	}

	// now we've done our sanity checks, it's time to kick that watchdoggo. Let's get nasty...

	using namespace std::literals; // enables the usage of 24h, 1ms, 1s instead of e.g. std::chrono::hours(24), accordingly
	using namespace std::chrono_literals;

	// since we cannot easily convert TO file_time_type in C++17, we calc differences based on our local clock
	// and adjust the touch time accordingly:
	// - since we have the lock, nobody but us should be touching that watchdog file
	//   hence we can "safely" assume the previously registered MTIME is still there.
	// - to prevent us running subtle gradual time skew, we always start our time calculations
	//   from the very moment the lockfile was CREATED!
	// - calculate the time diff between now and then in local clock ticks.
	// - convert that to file_time_type ticks by way of the ratios.
	// - add the diff to the file_time and update MTIME: *KICK*.

	//m_file_time_at_lock_creation = m_monitoring_last_watchdog_kick_time = fs::last_write_time(watchdogfile_path);
	//m_local_clock_at_lock_creation = m_monitoring_time_of_last_observed_change = chrono::system_clock::now();
	//m_clock_vs_filetime_diff = int64_t(m_local_clock_at_lock_creation.time_since_epoch().count()) - int64_t(m_file_time_at_lock_creation.time_since_epoch().count());

	fs::file_time_type ft = m_file_time_at_lock_creation; 
	auto diff_dur = t - m_local_clock_at_lock_creation;
	int64_t diff = diff_dur.count();
	// `diff` is now in local clock ticks and must be converted to file time ticks:
	const auto tick1 = chrono::system_clock::time_point::period();
	const auto fsgranularity = fs::file_time_type::period();
	const double conversion_factor = /* file_time_type::period / chrono::system_clock::time_point */
		(fsgranularity.num * tick1.den) /
		double(fsgranularity.den * tick1.num);
	diff = int64_t(diff * conversion_factor);

	fs::file_time_type::duration d(diff);
	ft += d;
	fs::last_write_time(m_watchdogfile_path, ft, ec);
	m_monitoring_last_watchdog_kick_time = fs::last_write_time(m_watchdogfile_path, ec);

	// again set up the watchdog beat frequency expectation:
	chrono::microseconds delta_µs = 10s;
	delta_µs /= get_chrono_clock_beat_multiplier();
	m_next_write_time_kick += delta_µs;

#if 0
	auto print_last_write_time = [](fs::file_time_type const& ftime) {
		std::time_t cftime = std::chrono::system_clock::to_time_t(
			std::chrono::file_clock::to_sys(ftime));
		std::cout << "File write time is " << std::asctime(std::localtime(&cftime));
	};

	print_last_write_time(ftime);
#endif

	return true;
}



// See monitor_for_lock_availability() for the use of this chunk.
// Also note the story in the comments there about possible RACE CONDITION:
// that's what made this chunk a favorite request at the opera house
// and that merits it its own method/function right here:
bool NASLockFile::check_staleness_and_report_age(chrono::system_clock::time_point t, uint64_t & seconds_ago, std::error_code& ec)
{
	seconds_ago = 0;

	if (m_lost_lock_due_to_external_circumstances)
	{
		ec = make_error_code(E::ELostLock);
		return false;		// do NOT signal immediate opportunity --> FALSE
	}

	// we can do some rough & quick checks here, assuming we DO NOT want to acquire a lock
	// *immediately* upon discovering availability. We can keep those actions separate.
	// make sure we have a legal watchdog file waiting for us.

	// check if the locking directory exists (i.e. the lock is owned by somebody else)
	// Do it rough, so don't check directory attribute: existence is enough.
	if (!fs::exists(m_lockfile_path, ec))
	{
		// opportunity!
		m_monitoring_active = false;		// EXIT the monitoring phase too!
		return true;
	}

	// Okay, so the directory exists.
	// Now go check if the watchdog file exists and gets kicked periodically.
	//
	// WARNING: non-existence of the watchdog file CAN LEGALLY OCCUR shortly after
	// a node acquired the lock or shortly before a node releases the lock it held
	// (via `fs::remove_all()`).
	//
	// We also must reckon with networks where nodes DO NOT have precisely synced timestamps
	// so we CANNOT check against absolute timestamp values: that's what the watchdog
	// activity is for: so everyone out there can see us (the lock holder) *move*.
	//
	// Staleness is thus defined as the *absence of change over a sufficiently long period of time*.
	// That "change" can be a change in the presence/absence status of the watchdog file,
	// or a slowly changing MTIME of said watchdog file.

	// check if we are already monitoring for some time.
	// IFF not, reset the state/counters to something sane: those should be values
	// such that our first monitoring rounds SHOULD assume 'freshness' automatically.
	if (!m_monitoring_active)
	{
		m_monitoring_active = true;
		m_monitoring_time_of_last_observed_change = t;
		m_monitoring_watchdog_file_exists = false;
		m_monitoring_last_watchdog_kick_time = m_monitoring_last_watchdog_kick_time.max() - 666h;
	}

	bool state = fs::exists(m_watchdogfile_path, ec);
	if (state != m_monitoring_watchdog_file_exists)
	{
		// detected change right now:
		m_monitoring_time_of_last_observed_change = t;
		m_monitoring_watchdog_file_exists = state;
		// Doesn't matter if the watchdog file went or came right now: we 'fake' observing
		// a watchdog kick on the next round anyway:
		m_monitoring_last_watchdog_kick_time = m_monitoring_last_watchdog_kick_time.max() - 666h;
	}
	if (state)
	{
		// watchdog file exists: check it's MTIME pulse:
		std::filesystem::file_time_type wd_t = fs::last_write_time(m_watchdogfile_path, ec);
		auto diff = wd_t - m_monitoring_last_watchdog_kick_time;
		// did we observe MTIME change?
		// NOTE: we will also observe (insane) MTIME change when the watchdog file got deleted
		// in between these non-atomic operations: that is fine because we only look for
		// the FACT OF CHANGE; we DO NOT mind HOW MUCH change there is in the timestamp...
		if (diff.count() != 0)
		{
			m_monitoring_time_of_last_observed_change = t;
		}
		m_monitoring_last_watchdog_kick_time = wd_t;
	}

	// now check if the 'staleness' has set in: how long ago did we last see something change?
	auto age = t - m_monitoring_time_of_last_observed_change;
	// convert to seconds:
	// seconds_ago = age / chrono::system_clock::time_point::period
	auto tick1 = chrono::system_clock::time_point::period();
	seconds_ago = (age.count() * get_chrono_clock_beat_multiplier() * tick1.num) / tick1.den;

	return false;
}



// periodically checks the lockfile to discover *staleness* (the **opposite of freshness**)
// or *absence* of the lockfile, i.e. *wait for opportunities to create the lockfile*.
bool NASLockFile::monitor_for_lock_availability(std::error_code& ec)
{
	if (m_lost_lock_due_to_external_circumstances)
	{
		ec = make_error_code(E::ELostLock);
		return false;
	}

	if (m_lock_obtained)
	{
		ec = make_error_code(E::EWeAlreadyAcquiredTheLock);
		return false;
	}

	chrono::system_clock::time_point t = chrono::system_clock::now();

	uint64_t seconds_ago;
	if (check_staleness_and_report_age(t, seconds_ago, ec))
	{
		return true;
	}

	// the refresh suggested interval is 10 seconds.
	// 'staleness' should account for that on disparate & distant networked nodes:
	// networked filesystems aren't always the fastest updating systems out there: DropBox, Google Drive, etc.
	// do not just suffer from network latency but are far more severely impacted by their own update frequencies.
	//
	// TODO: make this user-configurable so we can test & twist that dial as we get this code out there
	// in the wild and are expecting curious trouble to rear its ugly head. (von Moltke re: networking)

	// note: seconds_ago CAN be negative: that's us 'faking' a watchdog kick during state change above.
	// Don't mind that and only consider *positive* distances, i.e. true *age*.
	if (seconds_ago >= 60 * 2)
	{
		// 2 minutes of no-movement is considered stale for now: TODO: configurable setting!

		// nuke the lock.
		// Do it in such a way that we'll have the lock 'acquired' first, before we clean out,
		// so our fellow nodes DO NOT attempt to do the same cleanup and release in parallel
		// with us.
		// For that reason, we acquire ANOTHER lock: the monitor lock.
		// After that, we kick the watchdog and clean up.

		// mkdir() is atomic:
		// guaranteed atomic FS operation (as far as guarantees go...)
		if (!fs::create_directory(m_stale_recovery_lockfile_path, ec))
		{
			// diagnose the error and recover
			switch (ec.value())
			{
			case 0: // SUCCESS: that's when fs::create_directory() decides the directory already exists, but considers that to be OK (fstat-->OK) in Win32!
			case EEXIST:
				// locked by someone else out there.
				// Let them clean up.
				ec = make_error_code(E::ELockOwnedByOthers);
				m_monitoring_active = false;		// EXIT the monitoring phase now, allow it to restart on the next cycle!
				return false;
			}
			m_monitoring_active = false;		// EXIT the monitoring phase now, allow it to restart on the next cycle!
			return false;
		}
		else
		{
			// STEP 0 (ZERO):: see the longer RACE CONDITION scenario discussion comment further below:
			// BEFORE WE DO ANYTHING AFTER HAVING ACQUIRED THE STALENESS LOCK, we MUST recheck the staleness state
			// and check if anything has changed there, thanks to very swift action from a fast neighbour:
			//
			// As we check against the exact same local clock time as before (`t`) all results, including
			// the calculated `seconds_ago` value, MUST MATCH EXACTLY or we can be assured a foreign party
			// is ahead of us and we should quietly leave the scene...
			uint64_t seconds_ago_recheck;
			bool locking_opportunity = check_staleness_and_report_age(t, seconds_ago_recheck, ec);

			if (locking_opportunity || seconds_ago_recheck != seconds_ago)
			{
				// whoops. LOCK has already been acquired and (possibly) released by the fast neighbour node.

				// Time to release the staleness lock:
				// We KNOW we didn't add anything into the staleness lockdir yet,
				// so don't try to call remove_all() on that one: the sole remove()
				// SHOULD at least catch the hair of that devil dog!
				if (!fs::remove(m_stale_recovery_lockfile_path, ec))
				{
					// failure to release!
					// unknown error.
					// fail HARD -- we DO NOT release our regular lock but let it fall back into 'staleness',
					// where we got it. We're in UNPREDICTABLE COUNTRY here, so best not to assume things
					// that may damage or kill us.
					ec = make_error_code(E::ESpuriousStalenessRecoveryError);
				}
				m_lock_obtained = false;
				m_monitoring_active = false;		// EXIT the monitoring phase now, allow it to restart on the next cycle!
				return locking_opportunity;
			}

			// we now own the staleness lock, so we can go and clean up by FIRST
			// acquiring the regular lock:
			if (!setup_watchdog_file(ec))
			{
				// failure to acquire!
				// unknown error.
				// fail HARD -- after we've released our staleness lock!
				// We KNOW we didn't add anything into the staleness lockdir yet,
				// so don't try to call remove_all() on that one: the sole remove()
				// SHOULD at least catch the hair of that devil dog!
				m_lock_obtained = false;
				fs::remove(m_stale_recovery_lockfile_path, ec);
				ec = make_error_code(E::ESpuriousStalenessRecoveryError);
				m_monitoring_active = false;		// EXIT the monitoring phase now, allow it to restart on the next cycle!
				return false;
			}

			// When we get here, we've just kicked the watchdog (by REWRITING the watchdog file, etc.)
			// and we are the proud owner of a LOCK.
			//
			// Time to release the staleness lock... HOLD IT, BUSTER! CHECK THE RACE CONDITION comment further below!
			//
			// Here we MUST RELEASE THE REGULAR LOCK WHILE WE STILL HAVE THE STALENESS LOCK!
			// Only AFTER we have released the regular lock can we release the staleness lock, or slower
			// brothers will cause havoc as described in the scenario discussion further below.
			//
			// So... RELEASE THE REGULAR LOCK NOW:
			// (Incidentally, this is why the staleness lockdir sits NEXT to the regular lock dir and NOT INSIDE it! ;-) )
			// 
			//if (!delete_lock(ec)) -->
			// 2. atomic operation: release lock?
			if (!cleanup_after_ourselves(ec) /* this is core of delete_lock */ )
			{
				// failure to release!
				// unknown error.
				// fail HARD -- after we've released our staleness lock!
				// We KNOW we didn't add anything into the staleness lockdir yet,
				// so don't try to call remove_all() on that one: the sole remove()
				// SHOULD at least catch the hair of that devil dog!
				// Allow the regular lock to fall back into 'staleness' mode; retry later that way.
				m_lock_obtained = false;
				fs::remove(m_stale_recovery_lockfile_path, ec);
				ec = make_error_code(E::ESpuriousStalenessRecoveryError);
				m_monitoring_active = false;		// EXIT the monitoring phase now, allow it to restart on the next cycle!
				return false;
			}

			// Okay, so now we have released the regular lock. That also means we've now implicitly spotted
			// a locking opportunity, which is what this call is about, hence we should set our proper return value now.
			locking_opportunity = true;

			// Time to release the staleness lock:
			// We KNOW we didn't add anything into the staleness lockdir yet,
			// so don't try to call remove_all() on that one: the sole remove()
			// SHOULD at least catch the hair of that devil dog!
			if (!fs::remove(m_stale_recovery_lockfile_path, ec))
			{
				// failure to release!
				// unknown error.
				// fail HARD -- we DO NOT release our regular lock but let it fall back into 'staleness',
				// where we got it. We're in UNPREDICTABLE COUNTRY here, so best not to assume things
				// that may damage or kill us.
				m_lock_obtained = false;
				ec = make_error_code(E::ESpuriousStalenessRecoveryError);
				m_monitoring_active = false;		// EXIT the monitoring phase now, allow it to restart on the next cycle!
				return locking_opportunity;
			}

			// -------------- The RACE CONDITION story -------------------------------------------------
			// 
			// Okay, so far, so good.
			// We've just released our staleness lock, acquired a stale lock from unknown (dead) parties,
			// and we're good to go.
			// Yes, we HAVE kicked the watchdog so any parallel observers SHOULD have seen 'movement'
			// by now and thus gone back to monitoring.
			//
			// HOWEVER, they use the same code as us (for it'll be the same library) and there's a bit of a
			// RACE CONDITION above in our initial version:
			// the code flow from LAST MEASUREMENT to the decision of staleness and following attempt to
			// acquire a STALENESS LOCK is NOT ATOMIC [ACROSS THE NETWORK]. Thus any node out there may have traveled our path
			// alongside and is now also living in the belief that they have acquired a good staleness lock above.
			//
			// The only barrier for them will have been the staleness lock acquisition, which was successful for US.
			// Sounds fine, does it?
			// 
			// HOWEVER, what iff THEY were so much faster than us and got that temporary staleness lock, plus did all the above
			// watchdog refreshing and all, releasing THEIR staleness lock afterwards, BEFORE WE even got near the call
			// to acquire the staleness lock -- which was then obviously and legally granted to us afterwards.
			//
			// That is a RACE as we would now have had a contended lock on the regular lockdir.
			//
			// The only sane way out that I can see here is to -- While STILL WITHIN OUR staleness lock section! --
			// acquire the regular lock above AND RELEASE IT!
			// Only THEN do we release our staleness lock!
			//
			// This also means we have to hunt for the regular lock afterwards ourselves, but that is fine, because we
			// got ourselves out of the staleness-based lockup.
			// What will happen then to the 'late party' (which was us in the scenario above, but now that we flipped
			// roles midway through this story, we are the fast one and have RELEASED the regular lock.
			// That will imply that WE (the tardy one) will acquire the staleness lock successfully, but then we MUST
			// discover an absent or FRESH watchdog file, for the regular lock has been either RELEASED and is pending
			// a lock request OR another node has come forward and acquired that lock already.
			//
			// Meanwhile, do we need to worry about our staleness lock? Not really, as that one will only be requested
			// once some node decides the regular lock is stale and that won't happen for quite a while as there's definitely
			// recent 'movement' right now!
			// All the nodes that acquire the staleness lock one after another in this fashion MUST therefor recheck the
			// staleness state BEFORE doing ANYTHING, as all these nodes will share that 'tardiness' scenario with us:
			// near simultaneous staleness decision but fast vs slow staleness lock acquisition as described above.
			//
			// Hence these two MANDATORY ACTIONS WITHIN the staleness lock critical section MUST be done by everyone:
			//
			// - before doing anything to the filesystem, re-check staleness. It MAY have changed. Then release your
			//   staleness lock and exit quietly.
			// - acquire the regular lock from staleness and RELEASE IT IMMEDIATELY.
			// - Only then are you allowed to release the staleness lock.
			//
			// -------------
			//
			// Extra TODO for later: there's also the (spurious) scenario where the application terminates or otherwise
			// b0rks while executing this staleness recovery section: we then have a STALE STALENESS LOCK. That is a recursive
			// problem which needs addressing in the next version. It's rare, but we know what happens when it's got a
			// chance of 1 in a million. If you are unsure, check your Pratchett references. ;-))
			// 
			// ------------
			return locking_opportunity;
		}
	}

	// When we get here, we've observed a 'fresh' lock being alive, not stale. Hence there's little to do but keep monitoring.

	return false;
}



// delete the lockfile when it has been found to be *stale* (by `monitor_for_lock_availability()`).
// Return failure when the lockfile could not *legally* be cleaned up.
// Return warning when the lockfile has already been cleaned (*refreshed* or *nuked*)
// by another application / network node.
//
// WARNING: this API is only provided as a MANUAL OVERRIDE means for human supervision
// empowered to act and clean up after a disasterous situation.
// Normal mode of operation would be to let `monitor_for_lock_availability()` monitor
// for lock staleness due to application abort/crashes/etc. and its built-in auto-recovery
// mechanism.
// Only when `monitor_for_lock_availability()` starts to spit out errors and things look dire
// can a human operator abort all operations, clean up the locks using this API and then
// restart the regular applications.
bool NASLockFile::nuke_stale_lock(std::error_code& ec)
{
	cleanup_after_ourselves(ec);

	fs::remove_all(m_lockfile_path, ec);
	fs::remove(m_stale_recovery_lockfile_path, ec);

	m_lock_obtained = false;
	m_monitoring_active = false;		
	m_monitoring_watchdog_file_exists = false;
	m_lost_lock_due_to_external_circumstances = false;

	return true;
}

