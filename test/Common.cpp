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

#include <test/Common.h>
#include <test/CommonCli.h>

#include <test/EVMHost.h>
#include <test/libsolidity/util/SoltestErrors.h>

#include <libyul/backends/evm/EVMDialect.h>

#include <libsolutil/Assertions.h>
#include <libsolutil/StringUtils.h>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include <range/v3/all.hpp>

#include <iostream>
#include <stdexcept>

namespace fs = boost::filesystem;

namespace solidity::test
{

namespace
{

/// If non-empty returns the value of the env. variable ETH_TEST_PATH, otherwise
/// it tries to find a path that contains the directories "libsolidity/syntaxTests"
/// and returns it if found.
/// The routine searches in the current directory, and inside the "test" directory
/// starting from the current directory and up to three levels up.
/// @returns the path of the first match or an empty path if not found.
boost::filesystem::path testPath()
{
	if (auto path = getenv("ETH_TEST_PATH"))
		return path;

	auto const searchPath =
	{
		fs::current_path() / ".." / ".." / ".." / "test",
		fs::current_path() / ".." / ".." / "test",
		fs::current_path() / ".." / "test",
		fs::current_path() / "test",
		fs::current_path()
	};
	for (auto const& basePath: searchPath)
	{
		fs::path syntaxTestPath = basePath / "libsolidity" / "syntaxTests";
		if (fs::exists(syntaxTestPath) && fs::is_directory(syntaxTestPath))
			return basePath;
	}
	return {};
}

std::optional<fs::path> findInDefaultPath(std::string const& lib_name)
{
	auto const searchPath =
	{
		fs::current_path() / "deps",
		fs::current_path() / "deps" / "lib",
		fs::current_path() / ".." / "deps",
		fs::current_path() / ".." / "deps" / "lib",
		fs::current_path() / ".." / ".." / "deps",
		fs::current_path() / ".." / ".." / "deps" / "lib",
		fs::current_path(),
#ifdef __APPLE__
		fs::current_path().root_path() / fs::path("usr") / "local" / "lib",
#endif
	};
	for (auto const& basePath: searchPath)
	{
		fs::path p = basePath / lib_name;
		if (fs::exists(p))
			return p;
	}
	return std::nullopt;
}

}

CommonOptions::CommonOptions(std::string _caption):
	caption(std::move(_caption))
{
}

void CommonOptions::applyCommonCliArgs(
	std::optional<std::string> const& _evmVersion,
	std::optional<std::string> const& _testPath,
	std::optional<std::vector<std::string>> const& _vmPaths,
	size_t _batches,
	size_t _selectedBatch,
	bool _noSemanticTests,
	bool _noSmt,
	bool _optimize,
	bool _enforceGasCost,
	std::optional<std::string> const& _enforceGasCostMinValue,
	bool _abiEncoderV1,
	bool _showMessages,
	bool _showMetadata,
	std::optional<std::vector<std::string>> const& _positionals
)
{
	if (_positionals)
		for (auto const& token: *_positionals)
			if (!token.empty())
				BOOST_THROW_EXCEPTION(std::runtime_error("Unrecognized option: " + token));

	if (_evmVersion)
		evmVersionString = *_evmVersion;
	testPath = _testPath ? fs::path(*_testPath) : solidity::test::testPath();
	vmPaths.clear();
	if (_vmPaths)
		for (auto const& p: *_vmPaths)
			vmPaths.emplace_back(p);
	batches = _batches;
	selectedBatch = _selectedBatch;
	disableSemanticTests = _noSemanticTests;
	disableSMT = _noSmt;
	optimize = _optimize;
	enforceGasTest = _enforceGasCost;
	if (_enforceGasCostMinValue)
	{
		try
		{
			enforceGasTestMinValue = u256(*_enforceGasCostMinValue);
		}
		catch (std::exception const& _e)
		{
			solThrow(ConfigException, std::string("Invalid value for --enforce-gas-cost-min-value: ") + _e.what());
		}
	}
	useABIEncoderV1 = _abiEncoderV1;
	showMessages = _showMessages;
	showMetadata = _showMetadata;

	if (vmPaths.empty())
	{
		if (auto envPath = getenv("ETH_EVMONE"))
			vmPaths.emplace_back(envPath);
		else if (auto repoPath = findInDefaultPath(evmoneFilename))
			vmPaths.emplace_back(*repoPath);
		else
			vmPaths.emplace_back(evmoneFilename);
	}
}

void CommonOptions::validate() const
{
	solRequire(
		!testPath.empty(),
		ConfigException,
		"No test path specified. The --testpath argument must not be empty when given."
	);
	solRequire(
		fs::exists(testPath),
		ConfigException,
		"Invalid test path specified."
	);
	solRequire(
		batches > 0,
		ConfigException,
		"Batches needs to be at least 1."
	);
	solRequire(
		selectedBatch < batches,
		ConfigException,
		"Selected batch has to be less than number of batches."
	);

	if (!enforceGasTest)
		std::cout << std::endl << "WARNING :: Gas cost expectations are not being enforced" << std::endl << std::endl;
	else if (evmVersion() != langutil::EVMVersion{} || useABIEncoderV1)
	{
		std::cout << std::endl << "WARNING :: Enforcing gas cost expectations with non-standard settings:" << std::endl;
		if (evmVersion() != langutil::EVMVersion{})
			std::cout << "- EVM version: " << evmVersion().name() << " (default: " << langutil::EVMVersion{}.name() << ")" << std::endl;
		if (useABIEncoderV1)
			std::cout << "- ABI coder: v1 (default: v2)" << std::endl;
		std::cout << std::endl << "DO NOT COMMIT THE UPDATED EXPECTATIONS." << std::endl << std::endl;
	}
}

bool CommonOptions::parse(int argc, char const* const* argv)
{
	struct FullCommonArgs
	{
		rfl::Flatten<CommonCliArgs> common;
		rfl::Hidden<rfl::Positional<std::optional<std::vector<std::string>>>> positional;
	};

	auto parsed = rfl::cli::read<FullCommonArgs, TestCliProcessors>(argc, argv);
	if (!parsed)
		solThrow(ConfigException, parsed.error().what());

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
	return true;
}

std::string CommonOptions::toString(std::vector<std::string> const& _selectedOptions) const
{
	if (_selectedOptions.empty())
		return "";

	auto boolToString = [](bool _value) -> std::string { return _value ? "true" : "false"; };
	// Using std::map to avoid if-else/switch-case block
	std::map<std::string, std::string> optionValueMap = {
		{"evmVersion", evmVersion().name()},
		{"optimize", boolToString(optimize)},
		{"useABIEncoderV1", boolToString(useABIEncoderV1)},
		{"batch", std::to_string(selectedBatch + 1) + "/" + std::to_string(batches)},
		{"enforceGasTest", boolToString(enforceGasTest)},
		{"enforceGasTestMinValue", enforceGasTestMinValue.str()},
		{"disableSemanticTests", boolToString(disableSemanticTests)},
		{"disableSMT", boolToString(disableSMT)},
		{"showMessages", boolToString(showMessages)},
		{"showMetadata", boolToString(showMetadata)}
	};

	soltestAssert(ranges::all_of(_selectedOptions, [&optionValueMap](std::string const& _option) { return optionValueMap.count(_option) > 0; }));

	std::vector<std::string> optionsWithValues = _selectedOptions |
		ranges::views::transform([&optionValueMap](std::string const& _option) { return _option + "=" + optionValueMap.at(_option); }) |
		ranges::to<std::vector>();

	return solidity::util::joinHumanReadable(optionsWithValues);
}

void CommonOptions::printSelectedOptions(std::ostream& _stream, std::string const& _linePrefix, std::vector<std::string> const& _selectedOptions) const
{
	_stream << _linePrefix << "Run Settings: " << toString(_selectedOptions) << std::endl;
}

langutil::EVMVersion CommonOptions::evmVersion() const
{
	if (!evmVersionString.empty())
	{
		auto version = langutil::EVMVersion::fromString(evmVersionString);
		if (!version)
			BOOST_THROW_EXCEPTION(std::runtime_error("Invalid EVM version: " + evmVersionString));
		return *version;
	}
	else
		return langutil::EVMVersion();
}

yul::EVMDialect const& CommonOptions::evmDialect() const
{
	return yul::EVMDialect::strictAssemblyForEVMObjects(evmVersion());
}


CommonOptions const& CommonOptions::get()
{
	if (!m_singleton)
		BOOST_THROW_EXCEPTION(std::runtime_error("Options not yet constructed!"));

	return *m_singleton;
}

void CommonOptions::setSingleton(std::unique_ptr<CommonOptions const>&& _instance)
{
	m_singleton = std::move(_instance);
}

std::unique_ptr<CommonOptions const> CommonOptions::m_singleton = nullptr;

bool isValidSemanticTestPath(boost::filesystem::path const& _testPath)
{
	bool insideSemanticTests = false;
	fs::path testPathPrefix;
	for (auto const& element: _testPath)
	{
		testPathPrefix /= element;
		if (boost::ends_with(canonical(testPathPrefix).generic_string(), "/test/libsolidity/semanticTests"))
			insideSemanticTests = true;
		if (insideSemanticTests && boost::starts_with(element.string(), "_"))
			return false;
	}
	return true;
}

boost::unit_test::precondition::predicate_t minEVMVersionCheck(langutil::EVMVersion _minEVMVersion)
{
	return [_minEVMVersion](boost::unit_test::test_unit_id) {
		return test::CommonOptions::get().evmVersion() >= _minEVMVersion;
	};
}

bool loadVMs(CommonOptions const& _options)
{
	if (_options.disableSemanticTests)
		return true;

	bool evmSupported = solidity::test::EVMHost::checkVmPaths(_options.vmPaths);
	if (!_options.disableSemanticTests && !evmSupported)
	{
		std::cerr << "Unable to find " << solidity::test::evmoneFilename;
		std::cerr << ". Please disable semantics tests with --no-semantic-tests or provide a path using --vm <path>." << std::endl;
		std::cerr << "You can download it at" << std::endl;
		std::cerr << solidity::test::evmoneDownloadLink << std::endl;
		return false;
	}
	return true;
}

}
