/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/** @file IsolTestOptions.cpp
* @date 2019
*/

#include <test/tools/IsolTestOptions.h>
#include <test/CommonCli.h>

#include <libsolutil/Assertions.h>

#include <boost/filesystem.hpp>

#include <iostream>
#include <regex>
#include <string>

namespace fs = boost::filesystem;

namespace solidity::test
{

namespace
{

std::string editorPath()
{
	if (getenv("EDITOR"))
		return getenv("EDITOR");
	else if (fs::exists("/usr/bin/editor"))
		return "/usr/bin/editor";

	return std::string{};
}

struct IsolTestCliArgs
{
	rfl::Flatten<CommonCliArgs> common;
	rfl::Description<"Path to editor for opening test files.", std::optional<std::string>> editor;
	rfl::Description<"Show this help screen.", rfl::DefaultVal<bool>> help = false;
	rfl::Description<"Don't use colors.", rfl::DefaultVal<bool>> no_color = false;
	rfl::Description<"Automatically accept expectation updates.", rfl::DefaultVal<bool>> accept_updates = false;
	rfl::Short<"t", rfl::Description<"Filters which test units to include.", rfl::DefaultVal<std::string>>> test = "*/*";
	rfl::Hidden<rfl::Positional<std::optional<std::vector<std::string>>>> positional;
};

}

IsolTestOptions::IsolTestOptions():
	CommonOptions("isoltest, tool for interactively managing test contracts.")
{
}

bool IsolTestOptions::parse(int _argc, char const* const* _argv)
{
	auto parsed = rfl::cli::read<IsolTestCliArgs, TestCliProcessors>(_argc, _argv);
	if (!parsed)
		solThrow(ConfigException, parsed.error().what());

	showHelp = rfl::unwrap(parsed->help);
	if (showHelp)
	{
		rfl::cli::UsageOptions const usageOpts{
			.detailed = true,
			.header = "isoltest, tool for interactively managing test contracts.",
			.synopsis = "isoltest [Options]",
			.description = "Interactively validates test contracts.",
		};
		auto const text = rfl::cli::usage<IsolTestCliArgs, TestCliProcessors>("isoltest", usageOpts);
		solAssert(text.has_value(), text.error().what());
		std::cout << *text << std::endl;
		return false;
	}

	auto const& c = parsed->common.value();
	applyCommonCliArgs(
		rfl::unwrap(c.evm_version),
		rfl::unwrap(c.testpath),
		rfl::unwrap(c.vm),
		rfl::unwrap(c.batches),
		rfl::unwrap(c.selected_batch),
		rfl::unwrap(c.no_semantic_tests),
		rfl::unwrap(c.no_smt),
		rfl::unwrap(c.optimize),
		rfl::unwrap(c.enforce_gas_cost),
		rfl::unwrap(c.enforce_gas_cost_min_value),
		rfl::unwrap(c.abiencoderv1),
		rfl::unwrap(c.show_messages),
		rfl::unwrap(c.show_metadata),
		rfl::unwrap(parsed->positional)
	);

	editor = rfl::unwrap(parsed->editor).value_or(editorPath());
	noColor = rfl::unwrap(parsed->no_color);
	acceptUpdates = rfl::unwrap(parsed->accept_updates);
	testFilter = rfl::unwrap(parsed->test);

	enforceGasTest = enforceGasTest || (evmVersion() == langutil::EVMVersion{} && !useABIEncoderV1);

	return true;
}

void IsolTestOptions::validate() const
{
	CommonOptions::validate();
	static std::string filterString{"[a-zA-Z0-9_/*]*"};
	static std::regex filterExpression{filterString};
	solRequire(
		regex_match(testFilter, filterExpression),
		ConfigException,
		"Invalid test unit filter - can only contain '" + filterString + ": " + testFilter
	);
}

}
