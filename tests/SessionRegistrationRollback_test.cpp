#include "test_framework.h"
#include "SessionRegistrationRollback.h"

#include <map>
#include <memory>
#include <string>

namespace
{
	using Dispatchers = std::map<std::string, std::shared_ptr<int>>;
	using Initialized = std::map<std::string, bool>;
}

TEST_CASE("failed initialization rollback removes only its exact registration and flags")
{
	auto allocated = std::make_shared<int>(1);
	auto foreign = std::make_shared<int>(2);
	Dispatchers dispatchers{ { "new", allocated }, { "foreign", foreign } };
	Initialized initialized{ { "new", true }, { "foreign", true } };
	std::mutex mutex;
	CHECK(dvb::RollbackSessionRegistration(dispatchers, initialized, mutex, "new", allocated));
	CHECK(dispatchers.size() == 1);
	CHECK(dispatchers.at("foreign") == foreign);
	CHECK(initialized.size() == 1);
	CHECK(initialized.at("foreign"));
}

TEST_CASE("failed initialization rollback refuses a replacement or missing registration")
{
	auto allocated = std::make_shared<int>(1);
	auto replacement = std::make_shared<int>(2);
	Dispatchers dispatchers{ { "reused", replacement } };
	Initialized initialized{ { "reused", true } };
	std::mutex mutex;
	CHECK(!dvb::RollbackSessionRegistration(dispatchers, initialized, mutex, "reused", allocated));
	CHECK(!dvb::RollbackSessionRegistration(dispatchers, initialized, mutex, "missing", allocated));
	CHECK(dispatchers.at("reused") == replacement);
	CHECK(initialized.at("reused"));
}

TEST_CASE("failed initialization rollback is idempotent and refuses null ownership")
{
	auto allocated = std::make_shared<int>(1);
	Dispatchers dispatchers{ { "new", allocated } };
	Initialized initialized;
	std::mutex mutex;
	CHECK(!dvb::RollbackSessionRegistration(dispatchers, initialized, mutex, "new", {}));
	CHECK(dispatchers.at("new") == allocated);
	CHECK(dvb::RollbackSessionRegistration(dispatchers, initialized, mutex, "new", allocated));
	CHECK(!dvb::RollbackSessionRegistration(dispatchers, initialized, mutex, "new", allocated));
	CHECK(dispatchers.empty());
	CHECK(initialized.empty());
}
