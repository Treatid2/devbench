#include "test_framework.h"
#include "ContainedThreadEntry.h"

#include <stdexcept>

TEST_CASE("contained production entry returns success without recording failure")
{
	dvb::ContainedThreadEntry entry;
	int calls = 0;
	CHECK(entry.Run([&] { ++calls; }));
	CHECK(calls == 1);
	CHECK(entry.Failures() == 0);
	CHECK(!entry.Failed());
}

TEST_CASE("contained entry records standard and unknown failures without losing history")
{
	dvb::ContainedThreadEntry entry;
	CHECK(!entry.Run([] { throw std::runtime_error("listener/maintenance failed"); }));
	CHECK(entry.Failures() == 1);
	CHECK(entry.Failed());
	CHECK(!entry.Run([] { throw 42; }));
	CHECK(entry.Failures() == 2);
	CHECK(entry.Run([] {}));
	CHECK(entry.Failures() == 2);
	// A later normal exit is not owner-confirmed retirement or failure recovery.
	CHECK(entry.Failed());
}
