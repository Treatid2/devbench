#include "test_framework.h"
#include "ImmutableCallbackSnapshot.h"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace
{
	using Callbacks = std::map<std::string, std::function<void()>>;
	struct FallibleCopy
	{
		std::shared_ptr<bool> fail;
		explicit FallibleCopy(std::shared_ptr<bool> value) : fail(std::move(value)) {}
		FallibleCopy(const FallibleCopy& other) : fail(other.fail)
		{
			if (*fail) throw std::runtime_error("copy failed");
		}
		void operator()() const {}
	};
}

TEST_CASE("cleanup callback snapshots retain earlier registration after replacement")
{
	std::shared_ptr<const Callbacks> current;
	int observed = 0;
	dvb::UpdateCallbackSnapshot(current, "owner", std::function<void()>([&] { observed = 1; }));
	auto retained = current;
	dvb::UpdateCallbackSnapshot(current, "owner", std::function<void()>([&] { observed = 2; }));
	retained->at("owner")();
	CHECK(observed == 1);
	current->at("owner")();
	CHECK(observed == 2);
}

TEST_CASE("failed cleanup callable copy never publishes a partial snapshot")
{
	std::shared_ptr<const Callbacks> current;
	auto fail = std::make_shared<bool>(false);
	dvb::UpdateCallbackSnapshot(current, "first", std::function<void()>(FallibleCopy(fail)));
	auto retained = current;
	*fail = true;
	CHECK_THROWS(dvb::UpdateCallbackSnapshot(current, "second", std::function<void()>([] {})));
	CHECK(current == retained);
	CHECK(current->size() == 1);
	CHECK(current->count("second") == 0);
}

TEST_CASE("retirement snapshots copy ownership without copying fallible callbacks")
{
	std::shared_ptr<const Callbacks> current;
	auto fail = std::make_shared<bool>(false);
	dvb::UpdateCallbackSnapshot(current, "owner", std::function<void()>(FallibleCopy(fail)));
	*fail = true;
	auto retained = current;
	current.reset();
	CHECK(retained->size() == 1);
	retained->at("owner")();
}
