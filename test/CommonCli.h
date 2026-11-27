#pragma once

#include <rfl.hpp>
#include <rfl/cli.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace solidity::test
{

struct CommonCliArgs
{
	rfl::Description<"which EVM version to use", std::optional<std::string>> evm_version;
	rfl::Description<"path to test files", std::optional<std::string>> testpath;
	rfl::Description<"path to evmc library, can be supplied multiple times.", std::optional<std::vector<std::string>>> vm;
	rfl::Description<"set number of batches to split the tests into", rfl::DefaultVal<size_t>> batches = 1;
	rfl::Description<"zero-based number of batch to execute", rfl::DefaultVal<size_t>> selected_batch = 0;
	rfl::Description<"disable semantic tests", rfl::DefaultVal<bool>> no_semantic_tests = false;
	rfl::Description<"disable SMT checker", rfl::DefaultVal<bool>> no_smt = false;
	rfl::Description<"enables optimization", rfl::DefaultVal<bool>> optimize = false;
	rfl::ImplicitValue<rfl::Description<"Enforce checking gas cost in semantic tests.", rfl::DefaultVal<bool>>, "true"> enforce_gas_cost = false;
	rfl::Description<"Threshold value to enforce adding gas checks to a test.", std::optional<std::string>> enforce_gas_cost_min_value;
	rfl::Description<"enables abi encoder v1", rfl::DefaultVal<bool>> abiencoderv1 = false;
	rfl::Description<"enables message output", rfl::DefaultVal<bool>> show_messages = false;
	rfl::Description<"enables metadata output", rfl::DefaultVal<bool>> show_metadata = false;
};

using TestCliProcessors = rfl::Processors<
	rfl::NoExtraFields,
	rfl::cli::SeparateOptionValues,
	rfl::cli::ListDelimiter<"">,
	rfl::cli::PureFlags
>;

} // namespace solidity::test
