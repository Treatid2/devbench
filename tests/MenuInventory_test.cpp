#include "test_framework.h"
#include "MenuInventory.h"

TEST_CASE("Menu inventory: late installation seeds an already open Main Menu")
{
	dvb::MenuInventory inventory;
	CHECK(inventory.Seed(inventory.BeginSeed(), { "Main Menu" }));
	const auto snapshot = inventory.Snapshot();
	CHECK(snapshot.initialized);
	CHECK(snapshot.openMenus == std::vector<std::string>{ "Main Menu" });
	CHECK(!snapshot.KnownEmpty());
	CHECK(snapshot.KnownAbsent("MessageBoxMenu"));
	CHECK(snapshot.Describe(dvb::json::array())["messageBoxOpen"] == false);
}

TEST_CASE("Menu inventory: unavailable initial snapshot never proves menu or modal absence")
{
	dvb::MenuInventory inventory;
	const auto snapshot = inventory.Snapshot();
	CHECK(!snapshot.initialized);
	CHECK(!snapshot.KnownEmpty());
	CHECK(!snapshot.KnownAbsent("MessageBoxMenu"));
	const auto result = snapshot.Describe(dvb::json::array({ "devbench.selftest" }));
	CHECK(result["messageBoxOpen"].is_null());
	CHECK(result["inventoryInitialized"] == false);
	CHECK(result["registered"][0] == "devbench.selftest");
}

TEST_CASE("Menu inventory: events close the initial menu and maintain later modals")
{
	dvb::MenuInventory inventory;
	CHECK(inventory.Seed(inventory.BeginSeed(), { "Main Menu" }));
	inventory.OnEvent("Main Menu", false);
	CHECK(inventory.Snapshot().KnownEmpty());
	inventory.OnEvent("MessageBoxMenu", true);
	CHECK(!inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
	CHECK(inventory.Snapshot().Describe(dvb::json::array())["messageBoxOpen"] == true);
	inventory.OnEvent("MessageBoxMenu", false);
	CHECK(inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
}

TEST_CASE("Menu inventory: an event during seed collection cannot be overwritten")
{
	dvb::MenuInventory inventory;
	const auto revision = inventory.BeginSeed();
	inventory.OnEvent("MessageBoxMenu", true);
	CHECK(!inventory.Seed(revision, { "Main Menu" }));
	const auto snapshot = inventory.Snapshot();
	CHECK(!snapshot.initialized);
	CHECK(snapshot.openMenus == std::vector<std::string>{ "MessageBoxMenu" });
	CHECK(snapshot.Describe(dvb::json::array())["messageBoxOpen"] == true);
	CHECK(!snapshot.KnownAbsent("Main Menu"));
}

TEST_CASE("Menu inventory: seeded empty UI is distinguishable from unknown")
{
	dvb::MenuInventory inventory;
	CHECK(inventory.Seed(inventory.BeginSeed(), {}));
	CHECK(inventory.Snapshot().KnownEmpty());
	CHECK(inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
	const auto result = inventory.Snapshot().Describe(dvb::json::array());
	CHECK(result["inventoryInitialized"] == true);
	CHECK(result["messageBoxOpen"] == false);
}

TEST_CASE("Menu inventory: snapshots own sorted unique names and cannot mutate state")
{
	dvb::MenuInventory inventory;
	CHECK(inventory.Seed(inventory.BeginSeed(), { "Main Menu", "HUD Menu", "Main Menu" }));
	auto snapshot = inventory.Snapshot();
	CHECK(snapshot.openMenus == (std::vector<std::string>{ "HUD Menu", "Main Menu" }));
	snapshot.openMenus.clear();
	inventory.OnEvent("Main Menu", true);
	CHECK(inventory.Snapshot().openMenus.size() == 2);
}
