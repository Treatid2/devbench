#include "test_framework.h"
#include "TimeScaleControl.h"

#include <limits>
#include <vector>

using namespace dvb::TimeScaleControl;

TEST_CASE("time-scale receipt does not acknowledge expiry before the first engine write")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "short", 1, 1.0F, 0);
	int calls = 0;
	CHECK(!r.Reconcile(1, [&](float) { ++calls; return true; }));
	const auto receipt = r.Receipt(request, 1, 1.0F);
	CHECK(calls == 0);
	CHECK(receipt.at("state") == "expired");
	CHECK(receipt.at("accepted") == true);
	CHECK(receipt.at("applied") == false);
	CHECK(receipt.at("reached") == false);
	CHECK(receipt.at("ownsController") == false);
	CHECK(r.Requested() == 1.0F);
}

TEST_CASE("time-scale acknowledgement requires a successfully issued setter")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "timer", 100);
	CHECK(!r.Reconcile(1, [](float) { return false; }));
	CHECK(r.Pending());
	CHECK(r.Effective() == 1.0F);
	CHECK(r.Receipt(request, 1, 3.0F).at("applied") == false);
	CHECK(r.Receipt(request, 1, 3.0F).at("reached") == false);
	CHECK(r.Reconcile(2, [](float value) { return value == 3.0F; }) == 3.0F);
	CHECK(!r.Pending());
	CHECK(r.Receipt(request, 2, 1.2F).at("state") == "written");
	CHECK(r.Receipt(request, 3, 3.0F).at("state") == "reached");
}

TEST_CASE("same-owner time-scale displacement retains exact request identities and baseline")
{
	Reconciler r;
	const auto first = r.Request(3.0F, "same", 100, 0.5F, 0);
	const auto second = r.Request(2.0F, "same", 200, 0.5F, 1);
	CHECK(first->id != second->id);
	CHECK(r.Reconcile(2, [](float value) { return value == 2.0F; }) == 2.0F);
	const auto old = r.Receipt(first, 2, 3.0F);
	CHECK(old.at("state") == "displaced");
	CHECK(old.at("applied") == false);
	CHECK(old.at("atTarget") == true);
	CHECK(old.at("reached") == false);
	CHECK(old.at("requested") == 3.0F);
	CHECK(old.at("currentRequested") == 2.0F);
	CHECK(old.at("currentRequestId") == second->id);
	CHECK(r.Receipt(second, 2, 2.0F).at("reached") == true);
	CHECK(r.Reconcile(200, [](float value) { return value == 0.5F; }) == 0.5F);
	CHECK(r.Receipt(second, 200, 2.0F).at("state") == "expired");
}

TEST_CASE("time-scale historical write acknowledgement is not current ownership")
{
	Reconciler r;
	const auto first = r.Request(3.0F, "first", 100);
	CHECK(r.Reconcile(1, [](float) { return true; }) == 3.0F);
	const auto second = r.Request(2.0F, "second", 100, 3.0F, 2);
	const auto old = r.Receipt(first, 2, 3.0F);
	CHECK(old.at("applied") == true);
	CHECK(old.at("atTarget") == true);
	CHECK(old.at("reached") == false);
	CHECK(old.at("ownsController") == false);
	CHECK(old.at("owner") == "first");
	CHECK(old.at("currentOwner") == "second");
	CHECK(r.Receipt(second, 100, 2.0F).at("state") == "expired");
	CHECK(r.Receipt(second, 100, 2.0F).at("leaseRemainingMs") == 0);
}

TEST_CASE("already-target time scale is unchanged rather than an invented owned write")
{
	Reconciler r;
	const auto request = r.Request(1.0F, {}, 0);
	CHECK(!r.Reconcile(1, [](float) { return true; }));
	const auto receipt = r.Receipt(request, 1, 1.0F);
	CHECK(receipt.at("state") == "unchanged");
	CHECK(receipt.at("atTarget") == true);
	CHECK(receipt.at("applied") == false);
	CHECK(receipt.at("reached") == false);
	CHECK(receipt.at("ownsController") == true);
}

TEST_CASE("time-scale renewal and release keep their existing owner semantics")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "run", 100);
	CHECK(r.Reconcile(1, [](float) { return true; }) == 3.0F);
	r.RenewLease("other", 500);
	CHECK(request->expiresAtWallMs == 100);
	r.RenewLease("run", 500);
	CHECK(r.Receipt(request, 100, 3.0F).at("leaseRemainingMs") == 400);
	r.Release("other");
	CHECK(r.Receipt(request, 101, 3.0F).at("reached") == true);
	r.Release("run");
	const auto receipt = r.Receipt(request, 102, 3.0F);
	CHECK(receipt.at("state") == "released");
	CHECK(receipt.at("applied") == true);
	CHECK(receipt.at("reached") == false);
	CHECK(receipt.at("leaseRemainingMs") == 0);
	CHECK(r.Reconcile(102, [](float value) { return value == 1.0F; }) == 1.0F);
}

TEST_CASE("time-scale replacement after its lease deadline classifies the old receipt as expired")
{
	Reconciler r;
	const auto first = r.Request(3.0F, "first", 100);
	const auto second = r.Request(2.0F, "second", 200, 1.0F, 100);
	CHECK(r.Receipt(first, 100, 1.0F).at("state") == "expired");
	CHECK(r.Receipt(second, 100, 1.0F).at("state") == "accepted");
}

TEST_CASE("zero convergence budget separates stalled admission and samples just once")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "wait", 10000);
	int observations = 0;
	int sleeps = 0;
	const auto receipt = AwaitReceipt(0, 2000, 0, [&] {
		++observations;
		return r.Receipt(request, 2000, 1.0F);
	}, [] { return 2000; }, [&](std::int64_t) { ++sleeps; });
	CHECK(observations == 1);
	CHECK(sleeps == 0);
	CHECK(receipt.at("admissionMs") == 2000);
	CHECK(receipt.at("convergenceWaitedMs") == 0);
	CHECK(receipt.at("waitedMs") == 0);
	CHECK(receipt.at("totalElapsedMs") == 2000);
	CHECK(receipt.at("convergenceBudgetMs") == 0);
	CHECK(receipt.at("convergenceDeadlineExceeded") == false);
}

TEST_CASE("time-scale polling sleeps only the remaining convergence budget")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "wait", 1000);
	std::int64_t now = 10;
	std::vector<std::int64_t> sleeps;
	const auto receipt = AwaitReceipt(0, 10, 7, [&] { return r.Receipt(request, now, 1.0F); },
		[&] { return now; }, [&](std::int64_t ms) { sleeps.push_back(ms); now += ms; });
	CHECK(sleeps.size() == 2);
	CHECK(sleeps == std::vector<std::int64_t>({ 5, 2 }));
	CHECK(receipt.at("convergenceWaitedMs") == 7);
	CHECK(receipt.at("totalElapsedMs") == 17);
	CHECK(receipt.at("convergenceDeadlineExceeded") == false);
}

TEST_CASE("time-scale receipts report scheduler overshoot rather than claiming a hard return deadline")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "wait", 1000);
	std::int64_t now = 10;
	std::int64_t requestedSleep = 0;
	const auto receipt = AwaitReceipt(0, 10, 3, [&] { return r.Receipt(request, now, 1.0F); },
		[&] { return now; }, [&](std::int64_t ms) { requestedSleep = ms; now += ms + 8; });
	CHECK(requestedSleep == 3);
	CHECK(receipt.at("convergenceWaitedMs") == 11);
	CHECK(receipt.at("totalElapsedMs") == 21);
	CHECK(receipt.at("convergenceDeadlineExceeded") == true);
}

TEST_CASE("time-scale wait stops for its expired or displaced request without misattribution")
{
	for (const bool expire : { false, true }) {
		Reconciler r;
		const auto request = r.Request(3.0F, "old", expire ? 5 : 100);
		std::int64_t now = 0;
		int sleeps = 0;
		const auto receipt = AwaitReceipt(0, 0, 30, [&] { return r.Receipt(request, now, 2.0F); },
			[&] { return now; }, [&](std::int64_t ms) {
				++sleeps;
				now += ms;
				if (!expire) r.Request(2.0F, "new", 100, 2.0F, now);
			});
		CHECK(sleeps == 1);
		CHECK(receipt.at("state") == (expire ? "expired" : "displaced"));
		CHECK(receipt.at("requested") == 3.0F);
		CHECK(receipt.at("owner") == "old");
		CHECK(receipt.at("applied") == false);
		CHECK(receipt.at("reached") == false);
	}
}

TEST_CASE("final time-scale receipt and note use the same live sample and request")
{
	Reconciler r;
	const auto request = r.Request(3.0F, "owned", 100);
	CHECK(r.Reconcile(1, [](float) { return true; }) == 3.0F);
	int observations = 0;
	const auto receipt = AwaitReceipt(0, 1, 20, [&] {
		++observations;
		return r.Receipt(request, 1, observations == 1 ? 3.0F : 1.0F);
	}, [] { return 1; }, [](std::int64_t) {});
	CHECK(observations == 1);
	CHECK(receipt.at("reached") == true);
	CHECK(receipt.at("effective") == 3.0F);
	CHECK(receipt.at("note").get<std::string>().find("sampled effective=3, requested=3") != std::string::npos);
	r.Request(2.0F, "later", 100, 3.0F, 2);
	CHECK(receipt.at("owner") == "owned");
	CHECK(receipt.at("ownsController") == true);  // immutable observation, not a future guarantee
	CHECK(receipt.at("state") == "reached");
}

TEST_CASE("unchanged time-scale wait returns immediately without convergence polling")
{
	Reconciler r;
	const auto request = r.Request(1.0F, {}, 0);
	int sleeps = 0;
	const auto receipt = AwaitReceipt(0, 0, 2000, [&] { return r.Receipt(request, 0, 1.0F); },
		[] { return 0; }, [&](std::int64_t) { ++sleeps; });
	CHECK(sleeps == 0);
	CHECK(receipt.at("state") == "unchanged");
	CHECK(receipt.at("applied") == false);
}

TEST_CASE("time-scale wait integer validation checks wide values before narrowing")
{
	CHECK(ParseWaitMs(dvb::json(std::uint64_t{ 0 })) == 0);
	CHECK(ParseWaitMs(dvb::json(std::uint64_t{ 30000 })) == 30000);
	CHECK(ParseWaitMs(dvb::json(std::int64_t{ 0 })) == 0);
	CHECK(ParseWaitMs(dvb::json(std::int64_t{ 30000 })) == 30000);
	CHECK(!ParseWaitMs(dvb::json(std::uint64_t{ 30001 })));
	CHECK(!ParseWaitMs(dvb::json(std::int64_t{ -1 })));
	CHECK(!ParseWaitMs(dvb::json(std::numeric_limits<std::uint64_t>::max())));
	CHECK(!ParseWaitMs(dvb::json(std::numeric_limits<std::int64_t>::max())));
	CHECK(!ParseWaitMs(dvb::json(1.0)));
	CHECK(!ParseWaitMs(dvb::json("1")));
	CHECK(!ParseWaitMs(dvb::json(true)));
	CHECK(!ParseWaitMs(dvb::json(nullptr)));
}
