/*
 * WARNING NOTICE FOR DEVELOPERS:
 *
 * You must be VERY AWARE that these tests will behave COMPLETELY DIFFERENTLY when you place breakpoints
 * in the test code and start debugging the code: as "lock staleness" detection is dependent on the CLOCK,
 * you WILL observe very different behaviour in places where the expected run-time flow behaviour is
 * being interrupted by your breakpoints, resulting in different (faulty) test results.
 *
 * Therefor it is paramount to read both the nas_filelock sourcecode, the comments there and this test code,
 * including the comments here, before you jump to any conclusions and/or are surprised by sudden unexpected
 * test errors.
 *
 * A good debugging procedure would be to place abreakpoint at the call where you suspect things start
 * going wrong and then step through, assuming anything that comes after the breakpoint, including the code
 * you will be stepping through in the debugger, will "misbehave" re timings and time calculations.
 * When you review and reason through the **expected behaviour** then, observing what data you need,
 * go and place a breakpoint AFTER the call you wish to inspect: that way, you won't be influencing the
 * timeline/behaviour up to and INCLUDING the call under investigation and the returned values should be
 * trustworthy. Analyze them, go through the code of the call to review & reason about the behaviour you just
 * observed and adjust where you deem applicable. Rinse & repeat. Always use only a single breakpoint as
 * any subsequent breakpoints will be pretty useless for the code flow will already be quite different thanks
 * to the interruption caused by your first breakpoint.
 *
 * When you are sure you've fixed/tweaked what you needed to fix, remove/disable all breakpoints and re-run
 * ALL the tests in here to make sure nothing got out of whack due to wrong assumptions and/or corrupted
 * timeline/flow thanks to breakpoint interruptions and human 'slowness': the tests use a special API
 * override_chrono_clock_beat_multiplier(NNNN) to 'speed up timeouts' and thus help the tests complete in
 * a very short time while mimicking the real world (and the real world timeouts and delays we have encoded
 * in the library) very closely.
 * If you do not trust this 'speeding up' tweak of the library codebase, make sure to edit those calls in the
 * test code and set the speedup factor to 1, or delete the calls entirely (same result) AND THEN RE-RUN
 * ALL TESTS AGAIN: even when these run at 'realtime speed', timeouts/delays like 10 seconds are way too
 * short to NOT be impacted by your human actions while stepping through code, triggering breakpoints and the like.
 *
 * I canot repeat this often enough for this piece of code: ALWAYS REPEAT THE TESTS WITH ALL BREAKPOINTS REMOVED.
 * THEN, ALL TESTS MUST PASS.
 *
 * P.S.: I sometimes allow myself to play loose & fast with unit tests. Not so here. Not with this library.
 * These tests have been specifically designed to hit (almost) all corners of the nas_locking code and still be
 * a minimal set. Augment the nas_locking library, change it behaviour somehow and you very probably
 * will observe multiple tests fail. Fixing that can only be done one breakpoint, one step at a time.
 * 
 * Or you qualify for a big genius award and are way out of my league. Who knows...
 * 
 * P.P.S.: For the rest of us: y'all have been warned. Take it step by step. And unit tests are very valuable,
 * particularly in scenarios such as these, where we concern ourselves with a system where TIME is a
 * CRUCIAL component: in `nas_locking`, it's the clock and the filesystem which together the possible
 * STALENESS of a lock. And Staleness Detection is very important to give the locking system an opportunity
 * to self-recover from grave 'force majeure' situations. Which happen in actual "production" practice
 * more often that you & me would like.
 *
 * P.P.P.S.: The speed-up factors specified as argument for the various override_chrono_clock_beat_multiplier(NNNN)
 * API calls in this test code have been fiddled with a bit to ensure the tests pass on a reasonably fast development
 * machine WHERE EVEN THE SMALL INTERRUPTIONS IN THE TEST RUN DUE TO RUNNING THE TESTS IN THE VISUAL STUDIO TEST EXPLORER
 * DO SIGNIFICANTLY IMPACT THE (EXTREMELY SPED UP) TIMING-DEPENDENT BEHAVIOURS IN THE CODE.
 * When the tests don't pan out or act weird on your machine, the first thing I suggest you do is dial those speed up
 * factors down by a factor of 10 or 100 and rerun the tests: they will take more time but their behaviour should
 * then be completely consistent, if this is your problem. OR run the tests clean on the commandline and observe the
 * GoogleTest color-coded output there: that's much less overhead and should be fine, irrespective of speed-up
 * factor used.
 * 
 * Handle with care.
 * Thank you.
 *                                                                                          Ger Hobbelt
 *                                                                                          June 2nd, 2021
 */

#include "nas_lockfile.h"

#include "gtest/gtest.h"

#include <string.h>
#include <filesystem>
#include <boost/algorithm/string/predicate.hpp>

#if !defined(_MSC_VER)
#include <unistd.h>
#else
#include <windows.h>
#endif

static void sleep_N_milliseconds(int sleepMs)
{
#if !defined(_MSC_VER)
	usleep(sleepMs * 1000);   // usleep takes sleep time in us (1 millionth of a second)
#else
	Sleep(sleepMs);
#endif
}


namespace fs = std::filesystem;
using namespace std;
using namespace NetworkedFileIO;

using E = NASLockFileErrorCode;

#define EXPECT_STR_EQ(s1, s2)    EXPECT_EQ(strcmp(s1, s2), 0)



static void expect_no_error(const error_code& ec)
{
	EXPECT_EQ(ec.value(), 0);
	const char* name = ec.category().name();
	EXPECT_STR_EQ(name, "system");
}

/**
 * Test lock file operation.
 */
TEST(NASLockFile, BasicTest)
{
	// These all will acquire the same lock, without knowing so about one another.
	// We use this to fake test multiprocessing in these very basic tests...
	NASLockFile lock_1 = NASLockFile("a/b/");
	NASLockFile neighbour_1 = NASLockFile("a/b/");
	NASLockFile neighbour_2 = NASLockFile("a/b/");

	error_code ec;

	// clean up before we start the test:
	lock_1.nuke_stale_lock(ec);

	ec.clear();
	expect_no_error(ec);

	bool rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// when we have a lock, there's no opportunity to acquire the lock:
	rv = neighbour_1.monitor_for_lock_availability(ec);
	EXPECT_FALSE(rv);
	expect_no_error(ec);

	// when we have a lock, it's not sane to check for locking opportunity ourself:
	rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::EWeAlreadyAcquiredTheLock));
	EXPECT_STR_EQ(ec.category().name(), "NAS.FileLock");

	// when we have a lock, there's a lockfile:
	auto lfn = lock_1.active_lockfile();
	EXPECT_TRUE(boost::algorithm::ends_with(lfn, "__lock.lockdir"));
	EXPECT_TRUE(fs::is_directory(lfn, ec));
	expect_no_error(ec);

	// when we have a lock, we can release it when we're "done":
	rv = lock_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// and then the monitors should tell us there's an opportunity once again
	rv = neighbour_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	ec.clear();
}

/**
 * Test lock file collision: second process must fail to obtain the lock.
 */
TEST(NASLockFile, BasicCollisionTest)
{
	// These all will acquire the same lock, without knowing so about one another.
	// We use this to fake test multiprocessing in these very basic tests...
	NASLockFile lock_1 = NASLockFile("a/b/");
	NASLockFile neighbour_1 = NASLockFile("a/b/");
	NASLockFile neighbour_2 = NASLockFile("a/b/");

	error_code ec;

	// clean up before we start the test:
	lock_1.nuke_stale_lock(ec);

	ec.clear();
	expect_no_error(ec);

	bool rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	rv = neighbour_1.create_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::ELockOwnedByOthers));

	// neighbour also can't release OUR lock:
	rv = neighbour_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::EWeHaveNoLock));

	// when we have a lock, we can release it when we're "done":
	rv = lock_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// ---------------------------------------
	// and only then the neighbour can grab it
	rv = neighbour_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// while this time around WE cannot grab the lock as it's already taken
	rv = lock_1.create_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::ELockOwnedByOthers));

	// neither can we release our neighbour's lock:
	rv = lock_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::EWeHaveNoLock));

	// neighbour can release it when she's "done":
	rv = neighbour_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// and things should be full of opportunity again after all that
	rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	ec.clear();
}



/**
 * Test lock file acquisition N times by same program: do we keep a 'smart counter' or what happens?
 */
TEST(NASLockFile, GrabLockNTimesTest)
{
	// These all will acquire the same lock, without knowing so about one another.
	// We use this to fake test multiprocessing in these very basic tests...
	NASLockFile lock_1 = NASLockFile("a/b/");
	NASLockFile neighbour_1 = NASLockFile("a/b/");
	NASLockFile neighbour_2 = NASLockFile("a/b/");

	error_code ec;

	// clean up before we start the test:
	lock_1.nuke_stale_lock(ec);

	ec.clear();
	expect_no_error(ec);

	bool rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// first time grab:
	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// second time grab attempt:
	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);                                           // !!! we GET the lock, because we ALREADY HAVE IT!
	EXPECT_EQ(ec.value(), int(E::EWeAlreadyAcquiredTheLock));  // !!! BUT we do get an ERROR report for our 'incorrect use'

	// when we have a lock, we can release it when we're "done":
	rv = lock_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// and once released it STAYS released. No "smart counters" overhead to attempt sub-par userland code. ;-)
	rv = lock_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::EWeHaveNoLock));

	ec.clear();
}



/**
 * Test lock file going 'stale' (in sped up timeframe: we don't want to wait long for this in this test)
 */
TEST(NASLockFile, ObserveLockGoingStaleTest)
{
	// These all will acquire the same lock, without knowing so about one another.
	// We use this to fake test multiprocessing in these very basic tests...
	NASLockFile lock_1 = NASLockFile("a/b/");
	NASLockFile neighbour_1 = NASLockFile("a/b/");
	NASLockFile neighbour_2 = NASLockFile("a/b/");

	error_code ec;

	// clean up before we start the test:
	lock_1.nuke_stale_lock(ec);

	ec.clear();
	expect_no_error(ec);

	bool rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// grab lock:
	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// set up 'sped up timeframe' for everyone:
	lock_1.override_chrono_clock_beat_multiplier(3000);
	neighbour_1.override_chrono_clock_beat_multiplier(3000);

	// lock_1 is a bad lock keeper: no watchdog kicking for him!
	// So the lock will turn stale and we should be able to observe that...
	rv = neighbour_1.monitor_for_lock_availability(ec);
	EXPECT_FALSE(rv);
	//EXPECT_EQ(ec.value(), int(E::ELockOwnedByOthers));
	expect_no_error(ec);

	int count = 0;
	do
	{
		sleep_N_milliseconds(1);
		count++;

		rv = neighbour_1.monitor_for_lock_availability(ec);
	} while (!rv);
	EXPECT_TRUE(count >= 6);

	// we now have a STALE lock in lock_1,
	// and the neighbour can now grab the lock:
	rv = neighbour_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// ---- BAD STUFF STARTS HERE -------------------------------------------------------------
	// shit happens when the 'dead' program (external party) now goes and kills our(=neighbour!)
	// lock from under us:
	//
	// UPDATE: latest version of our locking lib has now code built in to detect staleness-on-release
	// to help diagnose spurious locking errors in userland code (this revision was triggered by
	// the testcases further below).
	// So this old way of just pretending we don't know anything won't work anymore and we'll get
	// extra nasty in this test by NUKING the lock by force right after this old one:

	rv = lock_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::ELostLock));

	// now blow out the lock by brutal force:
	neighbour_2.nuke_stale_lock(ec);
	ec.clear();
	expect_no_error(ec);

	// What does the lock monitor tell us after this: neighbour TWO, what do YOU see?
	// OOPS! A locking opportunity!
	rv = neighbour_2.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// Let's see if we (the *neighbour #1* with the valid lock!) can kick the watchdog any more:
	// Нет товарищ!
	//
	// (Oh, by the by, neighbour will only discover he's lost the lock *after* the internal optimizations
	// protecting against kicking the watchdog like a maniac (which equals an unneccessary hightened disk I/O + CPU load
	// cost) have decided it's "reasonable" to kick the watchdog once again. That's what the little wait
	// loop is for.)
	rv = neighbour_1.refresh_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::WOptimizedOut));

	count = 0;
	do
	{
		sleep_N_milliseconds(1);
		count++;

		rv = neighbour_1.refresh_lock(ec);
		EXPECT_FALSE(rv);
	} while (ec.value() == int(E::WOptimizedOut));
	EXPECT_TRUE(count >= 1);
	EXPECT_EQ(ec.value(), int(E::ELostLock));

	// AARGH! neighbour_1 thinks he still has the lock (which he doesn't, thanks to outside forces nuking the lock files)
	// while neighbour_2 observes an opportunity!
	// Neighbour_1 hasn't gone stale yet, but all this crap happens because lock_1 allowed his lock to go STALE!
	// 
	// ...
	// 
	// ... SHOULD we, perhaps, for the FRINGE CASE of a very-long-running-uninterruptable lock_1 process (which means
	// its code is totally rotten for it should have kicked the watchdog multiple times already by then!), provide
	// a basic diagnostic call, just so a programmer could plug it in at an easy location and attentively watch/review
	// log files afterwards, just so we can report on that off chance that "hey, your lock has gone stale!"?!?!
	// 
	// ...Hm...
	//
	// UPDATE: the new edition provides this via 'tracking' the lost-lock state until the very end, where you RESET
	// the state by calling delete_lock(). Which will ALSO report the ELostLock error, so you only need to check and log
	// your error codes thoroughly, like you should be doing anyway. SOLVED AFAIAC.

	rv = neighbour_2.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// neighbour #1: during a "lost lock" situation, until it is resolved, all relevant API calls should barf a hairball
	// telling us as much:
	{
		rv = neighbour_1.create_lock(ec);
		EXPECT_FALSE(rv);
		EXPECT_EQ(ec.value(), int(E::ELostLock));

		rv = neighbour_1.refresh_lock(ec);
		EXPECT_FALSE(rv);
		EXPECT_EQ(ec.value(), int(E::ELostLock));

		rv = neighbour_1.monitor_for_lock_availability(ec);
		EXPECT_FALSE(rv);
		EXPECT_EQ(ec.value(), int(E::ELostLock));

		std::string lfp = neighbour_1.active_lockfile();
		EXPECT_TRUE(lfp.empty());
	}

	// when we have a lock, we can release it when we're "done", BUT here we have a known case of 'lost lock', hence we MUST
	// call the release API to reset that state, whilee we'll get an error returned. Any subsequent call to the delete_lock()
	// API will then behave as expected: "you don't own a lock!"
	rv = neighbour_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::ELostLock));

	rv = neighbour_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::EWeHaveNoLock));

	// meanwhile at neighbour #2's: we release the lock we own
	rv = neighbour_2.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// Last bit of checking follows: neighbour #1 has recovered from a "lost lock" situation, hence he should be able to do
	// some locking and work once again...

	rv = neighbour_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	rv = neighbour_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	ec.clear();
}



/**
 * Test lock file watchdog proper reaction to kicking: long running process keeps lock 'alive' and other processes wait,
 * observing the LACK of staleness.
 */
TEST(NASLockFile, LockWatchdogKickingKeepsLockAliveTest)
{
	// These all will acquire the same lock, without knowing so about one another.
	// We use this to fake test multiprocessing in these very basic tests...
	NASLockFile lock_1 = NASLockFile("a/b/");
	NASLockFile neighbour_1 = NASLockFile("a/b/");
	NASLockFile neighbour_2 = NASLockFile("a/b/");

	error_code ec;

	// clean up before we start the test:
	lock_1.nuke_stale_lock(ec);

	ec.clear();
	expect_no_error(ec);

	bool rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// grab lock:
	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// set up 'sped up timeframe' for everyone:
	lock_1.override_chrono_clock_beat_multiplier(5000);
	neighbour_1.override_chrono_clock_beat_multiplier(5000);

	// lock_1 is a good lock keeper.
	// HOWEVER, for this test to ENSURE we're observing the behaviour we're looking for,
	// we must first guarantee that the watchdog WORKS: we do this by first measuring the time
	// it takes to observe "staleness".
	// Once we know that time, we reset everything, re-establish the lock and this time
	// around we'll be kicking the watchdog at least 3 times that "staleness period", all the while
	// testing the lock to make sure staleness never sets in: that we **proove** that the watchdog
	// does have impact *and* has the expected effect: preventing staleness of the lock!
	// This is done so we do NOT have to depend on the knowledge about internals timings: by waiting
	// for "staleness" first, we *derive*/*observe* the timeout period and then we can make sure
	// we surpass that timeout period in our actual watching kicking test.
	rv = neighbour_1.monitor_for_lock_availability(ec);
	EXPECT_FALSE(rv);
	expect_no_error(ec);

	int count = 0;
	do
	{
		sleep_N_milliseconds(1);
		count++;

		rv = neighbour_1.monitor_for_lock_availability(ec);
	} while (!rv);
	EXPECT_TRUE(count >= 6);

	// we now have a STALE lock in lock_1,
	// we also know the timeout period: `count`
	// and we can go and recover from the stale lock:
	rv = lock_1.delete_lock(ec);
	EXPECT_FALSE(rv);
	EXPECT_EQ(ec.value(), int(E::ELostLock));

	// everyone should now observe lock opportunity!
	rv = lock_1.monitor_for_lock_availability(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// re-establish the lock:
	rv = lock_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// now fake a "long running process" by running it 3 times the observed timeout `count`,
	// while kicking the watchdog.
	// All the while, the neighbours should observe a "alive/fresh" lock, no staleness ever.
	count *= 3;

	do
	{
		sleep_N_milliseconds(1);
		count--;

		// first check after delay, then kick watchdog:
		// this ensures the watchdog has to carry the freshness across the daly AT LEAST:
		rv = neighbour_1.monitor_for_lock_availability(ec);
		EXPECT_FALSE(rv);
		expect_no_error(ec);

		// By the way: neighbour cannot kick our dog!
		rv = neighbour_1.refresh_lock(ec);
		EXPECT_FALSE(rv);
		EXPECT_EQ(ec.value(), int(E::EWeHaveNoLock));

		// now *we* kick our own watchdog:
		rv = lock_1.refresh_lock(ec);
		if (!rv)
		{
			EXPECT_EQ(ec.value(), int(E::WOptimizedOut));
		}
		else
		{
			expect_no_error(ec);
		}

		// while any lock attempts by neighbours MUST fail
		rv = neighbour_1.create_lock(ec);
		EXPECT_FALSE(rv);
		EXPECT_EQ(ec.value(), int(E::ELockOwnedByOthers));
	} while (count > 0);

	// release the lock
	rv = lock_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// neighbour's lock attempt MUST succeed now the lock has been released:
	rv = neighbour_1.create_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	// release and we're done...
	rv = neighbour_1.delete_lock(ec);
	EXPECT_TRUE(rv);
	expect_no_error(ec);

	ec.clear();
}


GTEST_API_ int main(int argc, const char** argv)
{
	cout << "Running main() from " << __FILE__ << "\n";
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
