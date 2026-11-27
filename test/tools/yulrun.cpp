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
/**
 * Yul interpreter.
 */

#include <test/tools/yulInterpreter/Interpreter.h>
#include <test/tools/yulInterpreter/Inspector.h>

#include <libyul/AsmAnalysisInfo.h>
#include <libyul/AsmAnalysis.h>
#include <libyul/Dialect.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/YulStack.h>

#include <liblangutil/DebugInfoSelection.h>
#include <liblangutil/Exceptions.h>
#include <liblangutil/EVMVersion.h>
#include <liblangutil/SourceReferenceFormatter.h>

#include <libsolutil/CommonIO.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/Exceptions.h>

#include <rfl.hpp>
#include <rfl/cli.hpp>

#include <string>
#include <memory>
#include <iostream>

using namespace solidity;
using namespace solidity::util;
using namespace solidity::langutil;
using namespace solidity::yul;
using namespace solidity::yul::test;

namespace
{

std::pair<std::shared_ptr<AST const>, std::shared_ptr<AsmAnalysisInfo>> parse(std::string const& _source)
{
	YulStack stack(
		langutil::EVMVersion(),
		solidity::frontend::OptimiserSettings::none(),
		DebugInfoSelection::Default()
	);
	if (stack.parseAndAnalyze("--INPUT--", _source))
	{
		yulAssert(!Error::hasErrorsWarningsOrInfos(stack.errors()), "Parsed successfully but had errors.");
		return make_pair(stack.parserResult()->code(), stack.parserResult()->analysisInfo);
	}
	else
	{
		SourceReferenceFormatter(std::cout, stack, true, false).printErrorInformation(stack.errors());
		return {};
	}
}

void interpret(std::string const& _source, bool _inspect, bool _disableExternalCalls, bytes const& _calldata)
{
	std::shared_ptr<AST const> ast;
	std::shared_ptr<AsmAnalysisInfo> analysisInfo;
	tie(ast, analysisInfo) = parse(_source);
	if (!ast || !analysisInfo)
		return;

	InterpreterState state;
	state.maxTraceSize = 10000;
	state.calldata = _calldata;
	try
	{
		if (_inspect)
			InspectedInterpreter::run(std::make_shared<Inspector>(_source, state), state, *ast, _disableExternalCalls, /*disableMemoryTracing=*/false);
		else
			Interpreter::run(state, *ast, _disableExternalCalls, /*disableMemoryTracing=*/false);
	}
	catch (InterpreterTerminatedGeneric const&)
	{
	}

	state.dumpTraceAndState(std::cout, /*disableMemoryTracing=*/false);
}

}

int main(int argc, char** argv)
{
	struct YulRunArgs
	{
		rfl::cli::Flag<rfl::cli::attr::Desc<"Enable external calls">> enable_external_calls = false;
		rfl::cli::Flag<rfl::cli::attr::Desc<"Run interactive">> interactive = false;
		rfl::cli::Arg<std::optional<std::string>, rfl::cli::attr::Desc<"Calldata as hex string">> calldata;
		rfl::cli::Arg<std::optional<std::vector<std::string>>, rfl::cli::attr::Positional, rfl::cli::attr::Desc<"input file">> input_file;
	};
	using YulRunCli = rfl::Processors<
		rfl::cli::App<"", "", "", rfl::cli::BuiltinHelp::long_only>,
		rfl::cli::GnuStyle,
		rfl::cli::ListDelimiter<"">
	>;
	rfl::cli::UsageOptions const usageOpts{
		.detailed = true,
		.header = "yulrun, the Yul interpreter.",
		.synopsis = "yulrun [Options] < input",
		.description = "Reads a single source from stdin, runs it and prints a trace of all side-effects.",
	};

	auto parsed = rfl::cli::read_app<YulRunArgs, YulRunCli>(argc, argv, usageOpts);
	if (!parsed)
	{
		std::cerr << parsed.error().what() << std::endl;
		return 1;
	}
	if (auto const* info = std::get_if<rfl::cli::HelpOrVersion>(&*parsed))
	{
		std::cout << info->text;
		return 0;
	}

	auto const& args = std::get<YulRunArgs>(*parsed);
	std::string input;
	if (auto const& files = rfl::unwrap(args.input_file))
		for (std::string const& path: *files)
		{
			try
			{
				input += readFileAsString(path);
			}
			catch (FileNotFound const&)
			{
				std::cerr << "File not found: " << path << std::endl;
				return 1;
			}
			catch (NotAFile const&)
			{
				std::cerr << "Not a regular file: " << path << std::endl;
				return 1;
			}
		}
	else
		input = readUntilEnd(std::cin);

	bytes calldata;
	if (auto const& calldataStr = rfl::unwrap(args.calldata))
		try
		{
			calldata = fromHex(*calldataStr, WhenError::Throw);
		}
		catch (BadHexCharacter const&)
		{
			std::cerr << "Calldata is not a hex string." << std::endl;
			return 1;
		}

	interpret(input, rfl::unwrap(args.interactive), !rfl::unwrap(args.enable_external_calls), calldata);
	return 0;
}
