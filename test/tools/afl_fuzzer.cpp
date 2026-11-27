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
 * Executable for use with AFL <http://lcamtuf.coredump.cx/afl>.
 */

#include <test/tools/fuzzer_common.h>

#include <libsolutil/CommonIO.h>

#include <rfl.hpp>
#include <rfl/cli.hpp>

#include <string>
#include <iostream>

using namespace solidity;
using namespace solidity::util;

int main(int argc, char** argv)
{
	struct SolFuzzerArgs
	{
		rfl::Description<"Only output errors.", rfl::DefaultVal<bool>> quiet = false;
		rfl::Description<
			"Test via the standard-json interface, i.e. "
			"input is expected to be JSON-encoded instead of "
			"plain source file.",
			rfl::DefaultVal<bool>> standard_json = false;
		rfl::Description<
			"Run the constant optimizer instead of compiling. "
			"Expects a binary string of up to 32 bytes on stdin.",
			rfl::DefaultVal<bool>> const_opt = false;
		rfl::Positional<rfl::Description<"input file", std::optional<std::string>>> input_file;
		rfl::MultiToken<rfl::Description<"input files", std::optional<std::vector<std::string>>>> input_files;
		rfl::Description<
			"Run without optimizations. Cannot be used together with standard-json.",
			rfl::DefaultVal<bool>> without_optimizer = false;
	};
	using SolFuzzerCli = rfl::Processors<
		rfl::cli::App<"", "", "", rfl::cli::BuiltinHelp::long_only>,
		rfl::cli::GnuStyle,
		rfl::cli::ListDelimiter<"">
	>;
	rfl::cli::UsageOptions const usageOpts{
		.detailed = true,
		.header = "solfuzzer, fuzz-testing binary for use with AFL.",
		.synopsis = "solfuzzer [Options] < input",
		.description = "Reads a single source from stdin, compiles it and signals a failure for internal errors.",
	};

	auto parsed = rfl::cli::read_app<SolFuzzerArgs, SolFuzzerCli>(argc, argv, usageOpts);
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

	auto const& args = std::get<SolFuzzerArgs>(*parsed);
	bool const quiet = rfl::unwrap(args.quiet);

	std::vector<std::string> inputs;
	if (auto const& inputFile = rfl::unwrap(args.input_file))
		inputs.push_back(*inputFile);
	else if (auto const& inputFiles = rfl::unwrap(args.input_files))
		inputs = *inputFiles;
	else
		inputs.emplace_back("");

	bool const optimize = !rfl::unwrap(args.without_optimizer);
	int retResult = 0;

	for (std::string const& inputFile: inputs)
	{
		std::string input;
		if (inputFile.size() == 0)
			input = readUntilEnd(std::cin);
		else
			input = readFileAsString(inputFile);

		try
		{
			if (rfl::unwrap(args.const_opt))
				FuzzerUtil::testConstantOptimizer(input, quiet);
			else if (rfl::unwrap(args.standard_json))
				FuzzerUtil::testStandardCompiler(input, quiet);
			else
				FuzzerUtil::testCompilerJsonInterface(input, optimize, quiet);
		}
		catch (...)
		{
			retResult = 1;

			if (inputFile.size() == 0)
				throw;

			std::cerr << "Fuzzer "
				<< (optimize ? "" : "(without optimizer) ")
				<< "failed on "
				<< inputFile;
		}
	}

	return retResult;
}
