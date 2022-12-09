
#ifndef NAS_LOCKFILE_H
#define NAS_LOCKFILE_H

#ifdef __cplusplus

#include <string>
#include <filesystem>
#include <chrono>

#include <sys/types.h>
#include <sys/stat.h>



namespace NetworkedFileIO {

	enum class NASLockFileErrorCode
	{
		OK = 0,
		ELockOwnedByOthers = 400001,
		EWeHaveNoLock,
		EWeAlreadyAcquiredTheLock,
		ELostLockWatchdogFile,
		ELostLock,
		ESpuriousStalenessRecoveryError,
		EBasePathIsNotADirectory,

		WOptimizedOut = 500001,
	};

	// special error codes produced by this class, next to the regular ones:

	struct NASLockFileErrCategory : std::error_category
	{
		const char* name() const noexcept override;
		std::string message(int ev) const override;
	};



	class NASLockFile
	{
	public:
		NASLockFile(const std::string remote_directory_path);
		~NASLockFile();

		// create = obtain the lockfile.
		// Return failure when lockfile is already locked by others or otherwise cannot be created.
		bool create_lock(std::error_code& ec);

		// delete the lockfile we own.
		bool delete_lock(std::error_code& ec);

		// update the lockfile to signal *freshness* of the lock to everyone monitoring the lock,
		// i.e. "kicking the watchdog" in embedded dev parlance.
		bool refresh_lock(std::error_code& ec);

		// periodically checks the lockfile to discover *staleness* (the **opposite of freshness**)
		// or *absence* of the lockfile, i.e. *wait for opportunities to create the lockfile*.
		bool monitor_for_lock_availability(std::error_code& ec);

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
		bool nuke_stale_lock(std::error_code& ec);

		// Return the path to the current active lockfile.
		// Return string::empty when there's no currently active lockfile.
		std::string active_lockfile();

		// Allow test tools and other special rigs to 'tweak' our chrono beat, i.e.
		// the timebase we use to decide whether locks have become stale *and* whether
		// we should really kick the watchdog again (to prevent staleness).
		//
		// You can only override the basic 'beat' (which in itself is already dangerous
		// enough, because now no need to do this EVERYWHERE!); you do NOT have the option
		// to change the watchdog kick beat vs. the steleness monitor beat frequencies:
		// that is extremely risky!
		//
		// We also DO NOT allow this override to be class-wide (a.k.a. 'static') because
		// then this undesirable fiddling with the beat can become 'sticky' very easily.
		// We Do Not Want That, Ever!
		void override_chrono_clock_beat_multiplier(int64_t one_beat_duration_multiplier)
		{
			m_clock_beat_override = one_beat_duration_multiplier;
		}
		void reset_chrono_clock_beat_multiplier_override(void)
		{
			m_clock_beat_override = 0;
		}
		int64_t get_chrono_clock_beat_multiplier(void)
		{
			return m_clock_beat_override > 0 ? m_clock_beat_override : 1 /* standard beat: 1 second */;
		}

	private:
		const std::filesystem::path m_remote_directory_path;
		std::filesystem::path m_lockfile_path;
		std::filesystem::path m_stale_recovery_lockfile_path;
		std::filesystem::path m_watchdogfile_path;
		bool m_lock_obtained;
		bool m_monitoring_active;
		bool m_monitoring_watchdog_file_exists;
		bool m_lost_lock_due_to_external_circumstances;   // flags error state (which is reset by releasing the (lost) lock).
		std::filesystem::file_time_type m_file_time_at_lock_creation;
		std::filesystem::file_time_type m_monitoring_last_watchdog_kick_time;
		std::chrono::system_clock::time_point m_local_clock_at_lock_creation;
		std::chrono::system_clock::time_point m_next_write_time_kick;
		std::chrono::system_clock::time_point m_monitoring_time_of_last_observed_change;
		int64_t m_clock_vs_filetime_diff;
		int64_t m_clock_beat_override;

	protected:
		bool cleanup_after_ourselves(std::error_code& ec);
		bool setup_watchdog_file(std::error_code& ec);
		bool check_staleness_and_report_age(std::chrono::system_clock::time_point t, uint64_t& seconds_ago, std::error_code& ec);
	};


	inline std::string NASLockFile::active_lockfile()
	{
		if (!m_lock_obtained || m_lost_lock_due_to_external_circumstances)
			return "";

		return std::move(m_lockfile_path.string());
	}

}

extern "C" {
#endif

//...

#ifdef __cplusplus
}
#endif

#endif //NAS_LOCKFILE_H
