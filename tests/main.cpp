
#include "nas_lockfile.h"

#include "gtest/gtest.h"

#include <string.h>

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

	ec.clear();
}


GTEST_API_ int main(int argc, const char** argv)
{
	cout << "Running main() from " << __FILE__ << "\n";
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
