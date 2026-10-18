#include "test_framework.h"

// Cases self-register via TEST_CASE across the other TUs; just run them all.
int main(int argc, char** argv)
{
	if (argc == 1) return ::tf::run();
	const std::string option = "--case-prefix=";
	if (argc == 2 && std::string(argv[1]).starts_with(option) &&
		std::string(argv[1]).size() > option.size())
		return ::tf::run(std::string(argv[1]).substr(option.size()));
	std::fprintf(stderr, "usage: devbench-tests [--case-prefix=nonempty-prefix]\n");
	return 2;
}
