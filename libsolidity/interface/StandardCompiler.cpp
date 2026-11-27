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
 * @author Alex Beregszaszi
 * @date 2016
 * Standard JSON compiler interface.
 */

#include <libsolidity/interface/StandardCompiler.h>
#include <libsolidity/interface/ImportRemapper.h>

#include <libsolidity/ast/ASTJsonExporter.h>
#include <libyul/YulStack.h>
#include <libyul/Exceptions.h>
#include <libyul/optimiser/Suite.h>

#include <libevmasm/Disassemble.h>
#include <libevmasm/EVMAssemblyStack.h>

#include <libsmtutil/Exceptions.h>

#include <liblangutil/SourceReferenceFormatter.h>

#include <libsolutil/JSON.h>
#include <libsolutil/Keccak256.h>
#include <libsolutil/CommonData.h>

#include <boost/algorithm/string/predicate.hpp>

#include <rfl/error_format.hpp>
#include <rfl/json.hpp>
#include <rfl/json/Raw.hpp>
#include <rfl/json/to_schema.hpp>
#include <rfl/nlohmann.hpp>

#include <algorithm>
#include <optional>

#include <range/v3/algorithm/contains.hpp>

using namespace solidity;
using namespace solidity::yul;
using namespace solidity::frontend;
using namespace solidity::langutil;
using namespace solidity::util;
using namespace std::string_literals;

namespace
{

Json formatError(
	Error::Type _type,
	std::string const& _component,
	std::string const& _message,
	std::string const& _formattedMessage = "",
	Json const& _sourceLocation = Json(),
	Json const& _secondarySourceLocation = Json()
)
{
	Json error;
	error["type"] = Error::formatErrorType(_type);
	error["component"] = _component;
	error["severity"] = Error::formatErrorSeverityLowercase(Error::errorSeverity(_type));
	error["message"] = _message;
	error["formattedMessage"] = (_formattedMessage.length() > 0) ? _formattedMessage : _message;
	if (_sourceLocation.is_object())
		error["sourceLocation"] = _sourceLocation;
	if (_secondarySourceLocation.is_array())
		error["secondarySourceLocations"] = _secondarySourceLocation;
	return error;
}

Json formatFatalError(Error::Type _type, std::string const& _message)
{
	Json output;
	output["errors"] = Json::array();
	output["errors"].emplace_back(formatError(_type, "general", _message));
	return output;
}

Json formatSourceLocation(SourceLocation const* location)
{
	if (!location || !location->sourceName)
		return Json();

	Json sourceLocation;
	sourceLocation["file"] = *location->sourceName;
	sourceLocation["start"] = location->start;
	sourceLocation["end"] = location->end;
	return sourceLocation;
}

Json formatSecondarySourceLocation(SecondarySourceLocation const* _secondaryLocation)
{
	if (!_secondaryLocation)
		return Json();

	Json secondarySourceLocation = Json::array();
	for (auto const& location: _secondaryLocation->infos)
	{
		Json msg = formatSourceLocation(&location.second);
		msg["message"] = location.first;
		secondarySourceLocation.emplace_back(msg);
	}
	return secondarySourceLocation;
}

Json formatErrorWithException(
	CharStreamProvider const& _charStreamProvider,
	util::Exception const& _exception,
	Error::Type _type,
	std::string const& _component,
	std::string const& _message,
	std::optional<ErrorId> _errorId = std::nullopt
)
{
	std::string message;
	// TODO: consider enabling color
	std::string formattedMessage = SourceReferenceFormatter::formatExceptionInformation(
		_exception,
		_type,
		_charStreamProvider,
		false // colored
	);

	if (std::string const* description = _exception.comment())
		message = ((_message.length() > 0) ? (_message + ": ") : "") + *description;
	else
		message = _message;

	Json error = formatError(
		_type,
		_component,
		message,
		formattedMessage,
		formatSourceLocation(boost::get_error_info<errinfo_sourceLocation>(_exception)),
		formatSecondarySourceLocation(boost::get_error_info<errinfo_secondarySourceLocation>(_exception))
	);

	if (_errorId)
		error["errorCode"] = std::to_string(_errorId.value().error);

	return error;
}

/// Returns true iff @a _hash (hex with 0x prefix) is the Keccak256 hash of the binary data in @a _content.
bool hashMatchesContent(std::string const& _hash, std::string const& _content)
{
	try
	{
		return util::h256(_hash) == util::keccak256(_content);
	}
	catch (util::BadHexCharacter const&)
	{
		return false;
	}
}

bool isArtifactRequested(Json const& _outputSelection, std::string const& _artifact, bool _wildcardMatchesExperimental)
{
	static std::set<std::string> experimental{"ir", "irAst", "irOptimized", "irOptimizedAst", "ethdebug"};
	for (auto const& selectedArtifactJson: _outputSelection)
	{
		std::string const& selectedArtifact = selectedArtifactJson.get<std::string>();
		if (
			_artifact == selectedArtifact ||
			boost::algorithm::starts_with(_artifact, selectedArtifact + ".")
		)
		{
			if (_artifact.find("ethdebug") != std::string::npos)
				// only accept exact matches for ethdebug, e.g. evm.bytecode.ethdebug
				return selectedArtifact == _artifact;
			return true;
		}
		else if (selectedArtifact == "*")
		{
			// TODO: everything ethdebug related is only experimental for now, so it should not be matched by "*".
			if (_artifact.find("ethdebug") != std::string::npos)
				return false;
			// "ir", "irOptimized" can only be matched by "*" if activated.
			if (experimental.count(_artifact) == 0 || _wildcardMatchesExperimental)
				return true;
		}
	}
	return false;
}

///
/// @a _outputSelection is a JSON object containing a two-level hashmap, where the first level is the filename,
/// the second level is the contract name and the value is an array of artifact names to be requested for that contract.
/// @a _file is the current file
/// @a _contract is the current contract
/// @a _artifact is the current artifact name
///
/// @returns true if the @a _outputSelection has a match for the requested target in the specific file / contract.
///
/// In @a _outputSelection the use of '*' as a wildcard is permitted.
///
/// @TODO optimise this. Perhaps flatten the structure upfront.
///
bool isArtifactRequested(Json const& _outputSelection, std::string const& _file, std::string const& _contract, std::string const& _artifact, bool _wildcardMatchesExperimental)
{
	if (!_outputSelection.is_object())
		return false;

	for (auto const& file: { _file, std::string("*") })
		if (_outputSelection.contains(file) && _outputSelection[file].is_object())
		{
			/// For SourceUnit-level targets (such as AST) only allow empty name, otherwise
			/// for Contract-level targets try both contract name and wildcard
			std::vector<std::string> contracts{ _contract };
			if (!_contract.empty())
				contracts.emplace_back("*");
			for (auto const& contract: contracts)
				if (
					_outputSelection[file].contains(contract) &&
					_outputSelection[file][contract].is_array() &&
					isArtifactRequested(_outputSelection[file][contract], _artifact, _wildcardMatchesExperimental)
				)
					return true;
		}

	return false;
}

bool isArtifactRequested(Json const& _outputSelection, std::string const& _file, std::string const& _contract, std::vector<std::string> const& _artifacts, bool _wildcardMatchesExperimental)
{
	for (auto const& artifact: _artifacts)
		if (isArtifactRequested(_outputSelection, _file, _contract, artifact, _wildcardMatchesExperimental))
			return true;
	return false;
}

/// @returns boolean indicating whether @_artifacts is contained in @_outputSelection. Helper function.
template<ranges::range Range>
bool areArtifactsRequested(Json const& _outputSelection, Range&& _artifacts)
{
	for (auto const& fileRequests: _outputSelection)
		for (auto const& requests: fileRequests)
			for (auto const& request: requests)
				if (ranges::contains(_artifacts, request.get<std::string>()))
					return true;
	return false;
}

/// @returns all artifact names of the EVM object, either for creation or deploy time.
std::vector<std::string> evmObjectComponents(std::string const& _objectKind)
{
	solAssert(_objectKind == "bytecode" || _objectKind == "deployedBytecode", "");
	std::vector<std::string> components{"", ".object", ".opcodes", ".sourceMap", ".functionDebugData", ".generatedSources", ".linkReferences", ".ethdebug"};
	if (_objectKind == "deployedBytecode")
		components.push_back(".immutableReferences");
	return util::applyMap(components, [&](auto const& _s) { return "evm." + _objectKind + _s; });
}

/// @returns true if any binary was requested, i.e. we actually have to perform compilation.
bool isBinaryRequested(Json const& _outputSelection)
{
	if (!_outputSelection.is_object())
		return false;

	// This does not include "evm.methodIdentifiers" on purpose!
	static std::vector<std::string> const outputsThatRequireBinaries = std::vector<std::string>{
		"*",
		"ir", "irAst", "irOptimized", "irOptimizedAst",
		"evm.gasEstimates", "evm.legacyAssembly", "evm.assembly"
	} + evmObjectComponents("bytecode") + evmObjectComponents("deployedBytecode");

	for (auto const& fileRequests: _outputSelection)
		for (auto const& requests: fileRequests)
			for (auto const& output: outputsThatRequireBinaries)
				if (isArtifactRequested(requests, output, false))
					return true;
	return false;
}

/// @returns true if EVM bytecode was requested, i.e. we have to run the old code generator.
bool isEvmBytecodeRequested(Json const& _outputSelection)
{
	if (!_outputSelection.is_object())
		return false;

	static std::vector<std::string> const outputsThatRequireEvmBinaries = std::vector<std::string>{
		"*",
		"evm.gasEstimates", "evm.legacyAssembly", "evm.assembly"
	} + evmObjectComponents("bytecode") + evmObjectComponents("deployedBytecode");

	for (auto const& fileRequests: _outputSelection)
		for (auto const& requests: fileRequests)
			for (auto const& output: outputsThatRequireEvmBinaries)
				if (isArtifactRequested(requests, output, false))
					return true;
	return false;
}

/// @returns true if any per-contract ethdebug program output was requested.
bool isEthdebugProgramRequested(Json const& _outputSelection)
{
	if (!_outputSelection.is_object())
		return false;

	static std::array constexpr ethdebugArtifacts{"evm.bytecode.ethdebug", "evm.deployedBytecode.ethdebug"};

	return areArtifactsRequested(_outputSelection, ethdebugArtifacts);
}

/// @returns true if any ethdebug output (global or per-contract) was requested.
/// Global outputs are ignored for Yul input.
bool isAnyEthdebugRequested(Json const& _outputSelection, std::string_view _language)
{
	if (_language == "Yul")
		return isEthdebugProgramRequested(_outputSelection);

	if (!_outputSelection.is_object())
		return false;

	static std::array constexpr ethdebugArtifacts{
		"evm.bytecode.ethdebug", "evm.deployedBytecode.ethdebug",
		"ethdebug.resources", "ethdebug.compilation"
	};

	return areArtifactsRequested(_outputSelection, ethdebugArtifacts);
}

/// @returns true if the given global ethdebug output was requested via */*.
bool isEthdebugGlobalOutputRequested(Json const& _outputSelection, std::string const& _artifact)
{
	if (!_outputSelection.is_object())
		return false;

	static std::array constexpr globalEthdebugArtifacts{"ethdebug.resources", "ethdebug.compilation"};
	if (!ranges::contains(globalEthdebugArtifacts, _artifact))
		return false;

	for (auto const& [file, contracts]: _outputSelection.items())
		if (file == "*" && contracts.is_object())
			for (auto const& [contract, requests]: contracts.items())
				if (contract == "*" && requests.is_array())
					for (auto const& request: requests)
						if (request.get<std::string>() == _artifact)
							return true;

	return false;
}

bool isExperimentalArtifactRequested(Json const& _outputSelection, std::string_view _language)
{
	static std::array constexpr experimentalArtifacts{"irAst", "irOptimizedAst"};

	if (isAnyEthdebugRequested(_outputSelection, _language))
		return true;

	return areArtifactsRequested(_outputSelection, experimentalArtifacts);
}

/// @returns The set of selected contracts, along with their compiler pipeline configuration, based
/// on outputs requested in the JSON. Translates wildcards to the ones understood by CompilerStack.
/// Note that as an exception, '*' does not yet match "ir", "irAst", "irOptimized" or "irOptimizedAst".
CompilerStack::ContractSelection pipelineConfig(
	Json const& _jsonOutputSelection
)
{
	if (!_jsonOutputSelection.is_object())
		return {};

	CompilerStack::ContractSelection contractSelection;
	for (auto const& [sourceUnitName, jsonOutputSelectionForSource]: _jsonOutputSelection.items())
	{
		solAssert(jsonOutputSelectionForSource.is_object());
		for (auto const& [contractName, jsonOutputSelectionForContract]: jsonOutputSelectionForSource.items())
		{
			solAssert(jsonOutputSelectionForContract.is_array());
			CompilerStack::PipelineConfig pipelineForContract;
			for (Json const& request: jsonOutputSelectionForContract)
			{
				solAssert(request.is_string());
				pipelineForContract.irOptimization =
					pipelineForContract.irOptimization ||
					request == "irOptimized" ||
					request == "irOptimizedAst";
				pipelineForContract.irCodegen =
					pipelineForContract.irCodegen ||
					pipelineForContract.irOptimization ||
					request == "ir" ||
					request == "irAst";
				pipelineForContract.bytecode = isEvmBytecodeRequested(_jsonOutputSelection);
			}
			std::string key = (sourceUnitName == "*") ? "" : sourceUnitName;
			std::string value = (contractName == "*") ? "" : contractName;
			contractSelection[key][value] = pipelineForContract;
		}
	}
	return contractSelection;
}

Json formatLinkReferences(std::map<size_t, std::string> const& linkReferences)
{
	Json ret = Json::object();

	for (auto const& ref: linkReferences)
	{
		std::string const& fullname = ref.second;

		// If the link reference does not contain a colon, assume that the file name is missing and
		// the whole string represents the library name.
		size_t colon = fullname.rfind(':');
		std::string file = (colon != std::string::npos ? fullname.substr(0, colon) : "");
		std::string name = (colon != std::string::npos ? fullname.substr(colon + 1) : fullname);

		Json fileObject = ret.value(file, Json::object());
		Json libraryArray = fileObject.value(name, Json::array());

		Json entry;
		entry["start"] = Json(ref.first);
		entry["length"] = 20;

		libraryArray.emplace_back(entry);
		fileObject[name] = libraryArray;
		ret[file] = fileObject;
	}

	return ret;
}

Json formatImmutableReferences(std::map<u256, evmasm::LinkerObject::ImmutableRefs> const& _immutableReferences)
{
	Json ret = Json::object();

	for (auto const& immutableReference: _immutableReferences)
	{
		auto const& [identifier, byteOffsets] = immutableReference.second;
		Json array = Json::array();
		for (size_t byteOffset: byteOffsets)
		{
			Json byteRange;
			byteRange["start"] = Json::number_unsigned_t(byteOffset);
			byteRange["length"] = Json::number_unsigned_t(32); // immutable references are currently always 32 bytes wide
			array.emplace_back(byteRange);
		}
		ret[identifier] = array;
	}

	return ret;
}

} // end anonymous namespace

namespace solidity::frontend::standard_json
{

using OutputSelectionMap = std::map<std::string, std::map<std::string, std::vector<std::string>>>;
using LanguageLiteral = rfl::Literal<"Solidity", "Yul", "SolidityAST", "EVMAssembly">;
using StopAfterLiteral = rfl::Literal<"parsing">;
using EvmVersionLiteral = rfl::Literal<
	"homestead",
	"tangerineWhistle",
	"spuriousDragon",
	"byzantium",
	"constantinople",
	"petersburg",
	"istanbul",
	"berlin",
	"london",
	"paris",
	"shanghai",
	"cancun",
	"prague",
	"osaka",
	"amsterdam",
	"@future"
>;
using RevertStringsLiteral = rfl::Literal<"default", "strip", "debug", "verboseDebug">;
using BytecodeHashLiteral = rfl::Literal<"ipfs", "bzzr1", "none">;
using ModelCheckerEngineLiteral = rfl::Literal<"all", "bmc", "chc", "none">;
using ModelCheckerExtCallsLiteral = rfl::Literal<"untrusted", "trusted">;
using ModelCheckerInvariantLiteral = rfl::Literal<"contract", "reentrancy">;
using ModelCheckerSolverLiteral = rfl::Literal<"cvc5", "eld", "z3", "smtlib2">;
using ModelCheckerTargetLiteral = rfl::Literal<
	"constantCondition",
	"underflow",
	"overflow",
	"divByZero",
	"balance",
	"assert",
	"popEmptyArray",
	"outOfBounds"
>;

static_assert(StopAfterLiteral::size() == 1 && CompilerStack::StopAfterParsing == "parsing");
static_assert(EvmVersionLiteral::size() == langutil::EVMVersion::allVersions().size());
static_assert(RevertStringsLiteral::size() == c_revertStringsTable.size());
static_assert(ModelCheckerEngineLiteral::size() == ModelCheckerEngine::validNames().size());
static_assert(ModelCheckerExtCallsLiteral::size() == ModelCheckerExtCalls::validNames().size());
static_assert(ModelCheckerInvariantLiteral::size() == ModelCheckerInvariants::validNames().size());
static_assert(ModelCheckerSolverLiteral::size() == smtutil::SMTSolverChoice::validNames().size());
static_assert(ModelCheckerTargetLiteral::size() == ModelCheckerTargets::validNames().size());

struct YulDetailsDisabledInput
{
	std::optional<std::string> optimizerSteps;
};

struct YulDetailsInput
{
	std::optional<bool> stackAllocation;
	std::optional<std::string> optimizerSteps;
};

struct OptimizerDetailsInput
{
	std::optional<bool> peephole;
	std::optional<bool> inliner;
	std::optional<bool> jumpdestRemover;
	std::optional<bool> orderLiterals;
	std::optional<bool> deduplicate;
	std::optional<bool> cse;
	std::optional<bool> constantOptimizer;
	std::optional<bool> yul;
	std::optional<bool> simpleCounterForLoopUncheckedIncrement;
	// #69 Bridge 2: nlohmann::json passthrough for conditional sub-schema (depends on runYulOptimiser).
	std::optional<Json> yulDetails;
};

struct OptimizerInput
{
	std::optional<bool> enabled;
	std::optional<Json::number_unsigned_t> runs;
	std::optional<OptimizerDetailsInput> details;
};

struct MetadataInput
{
	std::optional<bool> appendCBOR;
	std::optional<bool> useLiteralContent;
	std::optional<BytecodeHashLiteral> bytecodeHash;
};

struct DebugInput
{
	std::optional<RevertStringsLiteral> revertStrings;
	std::optional<std::vector<std::string>> debugInfo;
};

struct ModelCheckerInput
{
	std::optional<std::map<std::string, std::vector<std::string>>> contracts;
	std::optional<bool> divModNoSlacks;
	std::optional<ModelCheckerEngineLiteral> engine;
	std::optional<unsigned> bmcLoopIterations;
	std::optional<ModelCheckerExtCallsLiteral> extCalls;
	std::optional<std::vector<ModelCheckerInvariantLiteral>> invariants;
	std::optional<bool> showProvedSafe;
	std::optional<bool> showUnproved;
	std::optional<bool> showUnsupported;
	std::optional<std::vector<ModelCheckerSolverLiteral>> solvers;
	std::optional<bool> printQuery;
	std::optional<std::vector<ModelCheckerTargetLiteral>> targets;
	std::optional<Json::number_unsigned_t> timeout;
};

struct AuxiliaryInput
{
	std::optional<std::map<std::string, std::string>> smtlib2responses;
};

struct SoliditySourceInput
{
	std::optional<std::string> content;
	std::optional<std::string> keccak256;
	std::optional<std::vector<std::string>> urls;
};

struct EvmAssemblySourceInput
{
	Json assemblyJson;
};

struct SettingsInput
{
	std::optional<bool> experimental;
	std::optional<StopAfterLiteral> stopAfter;
	std::optional<bool> viaIR;
	std::optional<bool> viaSSACFG;
	std::optional<EvmVersionLiteral> evmVersion;
	std::optional<DebugInput> debug;
	std::optional<std::vector<std::string>> remappings;
	std::optional<OptimizerInput> optimizer;
	std::optional<std::map<std::string, std::map<std::string, std::string>>> libraries;
	std::optional<MetadataInput> metadata;
	std::optional<OutputSelectionMap> outputSelection;
	std::optional<ModelCheckerInput> modelChecker;
};

struct StandardInput
{
	std::optional<LanguageLiteral> language;
	// #69 Bridge 2: polymorphic per-language raw JSON subtree (Solidity/Yul, SolidityAST, EVMAssembly).
	std::optional<std::map<std::string, Json>> sources;
	std::optional<AuxiliaryInput> auxiliaryInput;
	std::optional<SettingsInput> settings;
};

// #67 rfl::json::Raw: extracts raw AST subtree via yyjson without deserializing the full AST DOM.
struct AstSourceEnvelope
{
	std::optional<rfl::json::Raw> ast;
	std::optional<rfl::json::Raw> AST;
};

struct AstRootNode
{
	std::string nodeType;
};

} // namespace solidity::frontend::standard_json

namespace
{

using namespace solidity::frontend::standard_json;

std::string const* pathKey(std::vector<rfl::ErrorDetail::PathSegment> const& _path, size_t _idx)
{
	return _idx < _path.size() ? std::get_if<std::string>(&_path[_idx]) : nullptr;
}

/// Maps reflect-cpp's structured ErrorDetail (path + #70 ErrorKind) to Solidity's
/// Standard-JSON error format while preserving existing golden-file error strings.
Json formatRflError(rfl::Error const& _error)
{
	auto const details = _error.details();
	// Prioritize unknown keys (#70 ErrorKind::unknown_field) to match Solidity's checkKeys-first ordering.
	for (auto const& d: details)
		if (d.kind == rfl::ErrorKind::unknown_field && !d.path.empty())
			if (auto const* key = std::get_if<std::string>(&d.path.back()))
				return formatFatalError(Error::Type::JSONError, "Unknown key \"" + *key + "\"");

	if (details.empty())
		return formatFatalError(Error::Type::JSONError, _error.what());

	auto const& d = details.front();
	auto const& p = d.path;
	if (p.empty())
		return formatFatalError(Error::Type::JSONError, "Input is not a JSON object.");

	static std::map<std::string, char const*, std::less<>> const invalidValueMessages{
		{"language", "Only \"Solidity\", \"Yul\", \"SolidityAST\" or \"EVMAssembly\" is supported as a language."},
		{"settings.stopAfter", "Invalid value for \"settings.stopAfter\". Only valid value is \"parsing\"."},
		{"settings.evmVersion", "Invalid EVM version requested."},
		{"settings.debug.revertStrings", "Invalid value for settings.debug.revertStrings."},
		{"settings.metadata.bytecodeHash", "\"settings.metadata.bytecodeHash\" must be \"ipfs\", \"bzzr1\" or \"none\""},
		{"settings.modelChecker.engine", "Invalid model checker engine requested."},
		{"settings.modelChecker.extCalls", "Invalid model checker extCalls requested."},
	};

	static std::map<std::string, char const*, std::less<>> const legacyMessages{
		{"language", "Only \"Solidity\", \"Yul\", \"SolidityAST\" or \"EVMAssembly\" is supported as a language."},
		{"sources", "\"sources\" is not a JSON object."},
		{"auxiliaryInput", "\"auxiliaryInput\" must be an object"},
		{"auxiliaryInput.smtlib2responses", "\"auxiliaryInput.smtlib2responses\" must be an object."},
		{"settings", "\"settings\" must be an object"},
		{"settings.experimental", "'settings.experimental' must be a Boolean."},
		{"settings.stopAfter", "\"settings.stopAfter\" must be a string."},
		{"settings.viaIR", "\"settings.viaIR\" must be a Boolean."},
		{"settings.viaSSACFG", "\"settings.viaSSACFG\" must be a Boolean."},
		{"settings.evmVersion", "evmVersion must be a string."},
		{"settings.remappings", "\"settings.remappings\" must be an array of strings."},
		{"settings.debug", "\"settings.debug\" must be an object"},
		{"settings.debug.revertStrings", "settings.debug.revertStrings must be a string."},
		{"settings.debug.debugInfo", "settings.debug.debugInfo must be an array."},
		{"settings.optimizer", "\"settings.optimizer\" must be an object"},
		{"settings.optimizer.enabled", "The \"enabled\" setting must be a Boolean."},
		{"settings.optimizer.runs", "The \"runs\" setting must be an unsigned number."},
		{"settings.optimizer.details", "\"settings.optimizer.details\" must be an object"},
		{"settings.libraries", "\"libraries\" is not a JSON object."},
		{"settings.metadata", "\"settings.metadata\" must be an object"},
		{"settings.metadata.appendCBOR", "\"settings.metadata.appendCBOR\" must be Boolean"},
		{"settings.metadata.useLiteralContent", "\"settings.metadata.useLiteralContent\" must be Boolean"},
		{"settings.metadata.bytecodeHash", "\"settings.metadata.bytecodeHash\" must be \"ipfs\", \"bzzr1\" or \"none\""},
		{"settings.outputSelection", "\"settings.outputSelection\" must be an object"},
		{"settings.modelChecker", "\"modelChecker\" must be an object"},
		{"settings.modelChecker.contracts", "settings.modelChecker.contracts is not a JSON object."},
		{"settings.modelChecker.divModNoSlacks", "settings.modelChecker.divModNoSlacks must be a Boolean."},
		{"settings.modelChecker.engine", "settings.modelChecker.engine must be a string."},
		{"settings.modelChecker.bmcLoopIterations", "settings.modelChecker.bmcLoopIterations must be an unsigned integer."},
		{"settings.modelChecker.extCalls", "settings.modelChecker.extCalls must be a string."},
		{"settings.modelChecker.invariants", "settings.modelChecker.invariants must be an array."},
		{"settings.modelChecker.showProvedSafe", "settings.modelChecker.showProvedSafe must be a Boolean value."},
		{"settings.modelChecker.showUnproved", "settings.modelChecker.showUnproved must be a Boolean value."},
		{"settings.modelChecker.showUnsupported", "settings.modelChecker.showUnsupported must be a Boolean value."},
		{"settings.modelChecker.solvers", "settings.modelChecker.solvers must be an array."},
		{"settings.modelChecker.printQuery", "settings.modelChecker.printQuery must be a Boolean value."},
		{"settings.modelChecker.targets", "settings.modelChecker.targets must be an array."},
		{"settings.modelChecker.timeout", "settings.modelChecker.timeout must be an unsigned integer."},
	};

	std::string const pathStr = rfl::format_path(p);
	// rfl::Literal::from_string reports ErrorKind::invalid_value on unrecognized string values,
	// whereas rfl::nlohmann::Reader::to_basic_type reports ErrorKind::other on wrong JSON types.
	if (d.kind == rfl::ErrorKind::invalid_value)
		if (auto it = invalidValueMessages.find(pathStr); it != invalidValueMessages.end())
			return formatFatalError(Error::Type::JSONError, it->second);
	if (auto it = legacyMessages.find(pathStr); it != legacyMessages.end())
		return formatFatalError(Error::Type::JSONError, it->second);

	auto const* k0 = pathKey(p, 0);
	auto const* k1 = pathKey(p, 1);
	auto const* k2 = pathKey(p, 2);
	auto const* k3 = pathKey(p, 3);
	if (k0 && *k0 == "auxiliaryInput" && k1 && *k1 == "smtlib2responses" && k2)
		return formatFatalError(Error::Type::JSONError, "\"smtlib2Responses." + *k2 + "\" must be a string.");
	if (k0 && *k0 == "settings" && k1)
	{
		if (*k1 == "remappings")
			return formatFatalError(Error::Type::JSONError, "\"settings.remappings\" must be an array of strings");
		if (*k1 == "debug" && k2 && *k2 == "debugInfo")
			return formatFatalError(Error::Type::JSONError, "settings.debug.debugInfo must be an array.");
		if (*k1 == "optimizer" && k2 && *k2 == "details" && k3)
			return formatFatalError(Error::Type::JSONError, "\"settings.optimizer.details." + *k3 + "\" must be Boolean");
		if (*k1 == "libraries")
			return formatFatalError(Error::Type::JSONError, p.size() == 3 ? "Library entry is not a JSON object." : "Library address must be a string.");
		if (*k1 == "outputSelection" && k2)
			return formatFatalError(
				Error::Type::JSONError,
				p.size() == 3 ? "\"settings.outputSelection." + *k2 + "\" must be an object"
				              : "\"settings.outputSelection." + *k2 + "." + (k3 ? *k3 : "") + "\" must be a string array"
			);
		if (*k1 == "modelChecker" && k2)
		{
			if (*k2 == "contracts")
				return formatFatalError(Error::Type::JSONError, p.size() == 4 ? "Source contracts must be an array." : "Every contract in settings.modelChecker.contracts must be a string.");
			if (*k2 == "invariants")
				return formatFatalError(
					Error::Type::JSONError,
					d.kind == rfl::ErrorKind::invalid_value
						? "Invalid model checker invariants requested."
						: "Every invariant type in settings.modelChecker.invariants must be a string."
				);
			if (*k2 == "solvers" || *k2 == "targets")
				return formatFatalError(
					Error::Type::JSONError,
					d.kind == rfl::ErrorKind::invalid_value
						? "Invalid model checker " + *k2 + " requested."
						: "Every target in settings.modelChecker." + *k2 + " must be a string."
				);
		}
	}

	return formatFatalError(Error::Type::JSONError, "\"" + pathStr + "\": " + d.message);
}

std::optional<Json> applyOptimizerDetailSteps(
	std::string const& _fullSequence,
	std::string& _optimiserSetting,
	std::string& _cleanupSetting,
	bool _runYulOptimizer
)
{
	if (!_runYulOptimizer && !OptimiserSuite::isEmptyOptimizerSequence(_fullSequence))
		return formatFatalError(
			Error::Type::JSONError,
			"If Yul optimizer is disabled, only an empty optimizerSteps sequence is accepted."
			" Note that the empty optimizer sequence is properly denoted by \":\"."
		);

	try
	{
		yul::OptimiserSuite::validateSequence(_fullSequence);
	}
	catch (yul::OptimizerException const& _exception)
	{
		return formatFatalError(
			Error::Type::JSONError,
			"Invalid optimizer step sequence in \"settings.optimizer.details.optimizerSteps\": "s + _exception.what()
		);
	}

	auto const delimiterPos = _fullSequence.find(":");
	_optimiserSetting = _fullSequence.substr(0, delimiterPos);

	if (delimiterPos != std::string::npos)
		_cleanupSetting = _fullSequence.substr(delimiterPos + 1);
	else
		solAssert(_cleanupSetting == OptimiserSettings::DefaultYulOptimiserCleanupSteps);

	return {};
}

/// Validates and applies parsed optimizer settings.
/// On error returns the json-formatted error message.
std::variant<OptimiserSettings, Json> applyOptimizerSettings(
	std::string_view const _language,
	OptimizerInput const& _input
)
{
	OptimiserSettings settings = _language == "EVMAssembly" ? OptimiserSettings::none() : OptimiserSettings::minimal();

	if (_input.enabled.value_or(false))
		settings = OptimiserSettings::standard();

	if (_input.runs)
		settings.expectedExecutionsPerDeployment = *_input.runs;

	if (_input.details)
	{
		auto const& d = *_input.details;
		auto applyBool = [](std::optional<bool> _opt, bool& _target) {
			if (_opt)
				_target = *_opt;
		};
		applyBool(d.peephole, settings.runPeephole);
		applyBool(d.inliner, settings.runInliner);
		applyBool(d.jumpdestRemover, settings.runJumpdestRemover);
		applyBool(d.orderLiterals, settings.runOrderLiterals);
		applyBool(d.deduplicate, settings.runDeduplicate);
		applyBool(d.cse, settings.runCSE);
		applyBool(d.constantOptimizer, settings.runConstantOptimiser);
		applyBool(d.yul, settings.runYulOptimiser);
		applyBool(d.simpleCounterForLoopUncheckedIncrement, settings.simpleCounterForLoopUncheckedIncrement);
		settings.optimizeStackAllocation = settings.runYulOptimiser;

		if (d.yulDetails)
		{
			if (!settings.runYulOptimiser)
			{
				auto parsed = rfl::nlohmann::read<YulDetailsDisabledInput, rfl::NoExtraFields>(*d.yulDetails);
				if (!parsed)
				{
					auto const details = parsed.error().details();
					for (auto const& err: details)
						if (err.path.empty() || err.kind == rfl::ErrorKind::unknown_field)
							return formatFatalError(Error::Type::JSONError, "Only optimizerSteps can be set in yulDetails when Yul optimizer is disabled.");
					return formatFatalError(Error::Type::JSONError, "\"settings.optimizer.details.optimizerSteps\" must be a string");
				}
				if (parsed->optimizerSteps)
					if (auto error = applyOptimizerDetailSteps(*parsed->optimizerSteps, settings.yulOptimiserSteps, settings.yulOptimiserCleanupSteps, settings.runYulOptimiser))
						return *error;
				return {std::move(settings)};
			}

			auto parsed = rfl::nlohmann::read<YulDetailsInput, rfl::NoExtraFields>(*d.yulDetails);
			if (!parsed)
			{
				auto const details = parsed.error().details();
				for (auto const& err: details)
					if (err.kind == rfl::ErrorKind::unknown_field && !err.path.empty())
						if (auto const* key = std::get_if<std::string>(&err.path.back()))
							return formatFatalError(Error::Type::JSONError, "Unknown key \"" + *key + "\"");
				auto const& first = details.front();
				if (first.path.empty())
					return formatFatalError(Error::Type::JSONError, "\"settings.optimizer.details.yulDetails\" must be an object");
				if (auto const* k = pathKey(first.path, 0))
				{
					if (*k == "stackAllocation")
						return formatFatalError(Error::Type::JSONError, "\"settings.optimizer.details.stackAllocation\" must be Boolean");
					if (*k == "optimizerSteps")
						return formatFatalError(Error::Type::JSONError, "\"settings.optimizer.details.optimizerSteps\" must be a string");
				}
				return formatFatalError(Error::Type::JSONError, first.message);
			}
			applyBool(parsed->stackAllocation, settings.optimizeStackAllocation);
			if (parsed->optimizerSteps)
				if (auto error = applyOptimizerDetailSteps(*parsed->optimizerSteps, settings.yulOptimiserSteps, settings.yulOptimiserCleanupSteps, settings.runYulOptimiser))
					return *error;
		}
	}
	return {std::move(settings)};
}

}

std::variant<StandardCompiler::InputsAndSettings, Json> StandardCompiler::parseInput(Json const& _input)
{
	InputsAndSettings ret;

	auto parsedInput = rfl::nlohmann::read<StandardInput, rfl::NoExtraFields>(_input);
	if (!parsedInput)
		return formatRflError(parsedInput.error());

	ret.language = parsedInput->language ? parsedInput->language->str() : "";

	if (!parsedInput->sources || parsedInput->sources->empty())
		return formatFatalError(Error::Type::JSONError, "No input sources specified.");

	auto const& sources = *parsedInput->sources;
	ret.errors = Json::array();

	if (ret.language == "Solidity" || ret.language == "Yul")
	{
		for (auto const& [sourceName, sourceValue]: sources)
		{
			auto parsedSource = rfl::nlohmann::read<SoliditySourceInput, rfl::NoExtraFields>(sourceValue);
			if (!parsedSource)
			{
				auto const details = parsedSource.error().details();
				for (auto const& d: details)
					if (d.kind == rfl::ErrorKind::unknown_field && !d.path.empty())
						if (auto const* key = std::get_if<std::string>(&d.path.back()))
							return formatFatalError(Error::Type::JSONError, "Unknown key \"" + *key + "\"");
				auto const& first = details.front();
				if (first.path.empty())
					return formatFatalError(Error::Type::JSONError, "\"sources." + sourceName + "\" must be an object");
				if (auto const* k0 = pathKey(first.path, 0); k0 && *k0 == "urls" && first.path.size() == 2)
					return formatFatalError(Error::Type::JSONError, "URL must be a string.");
				return formatFatalError(Error::Type::JSONError, "Invalid input source specified.");
			}

			std::string const hash = parsedSource->keccak256.value_or("");

			if (parsedSource->content)
			{
				if (!hash.empty() && !hashMatchesContent(hash, *parsedSource->content))
					ret.errors.emplace_back(formatError(
						Error::Type::IOError,
						"general",
						"Mismatch between content and supplied hash for \"" + sourceName + "\""
					));
				else
					ret.sources[sourceName] = *parsedSource->content;
			}
			else if (parsedSource->urls)
			{
				if (!m_readFile)
					return formatFatalError(
						Error::Type::JSONError, "No import callback supplied, but URL is requested."
					);

				std::vector<std::string> failures;
				bool found = false;

				for (auto const& url: *parsedSource->urls)
				{
					ReadCallback::Result result = m_readFile(ReadCallback::kindString(ReadCallback::Kind::ReadFile), url);
					if (result.success)
					{
						if (!hash.empty() && !hashMatchesContent(hash, result.responseOrErrorMessage))
							ret.errors.emplace_back(formatError(
								Error::Type::IOError,
								"general",
								"Mismatch between content and supplied hash for \"" + sourceName + "\" at \"" + url + "\""
							));
						else
						{
							ret.sources[sourceName] = result.responseOrErrorMessage;
							found = true;
							break;
						}
					}
					else
						failures.push_back(
							"Cannot import url (\"" + url + "\"): " + result.responseOrErrorMessage
						);
				}

				for (auto const& failure: failures)
				{
					/// If the import succeeded, let mark all the others as warnings, otherwise all of them are errors.
					ret.errors.emplace_back(formatError(
						found ? Error::Type::Warning : Error::Type::IOError,
						"general",
						failure
					));
				}
			}
			else
				return formatFatalError(Error::Type::JSONError, "Invalid input source specified.");
		}
	}
	else if (ret.language == "SolidityAST")
	{
		for (auto const& [sourceName, sourceValue]: sources)
			ret.sources[sourceName] = util::jsonCompactPrint(sourceValue);
	}
	else if (ret.language == "EVMAssembly")
	{
		for (auto const& [sourceName, sourceValue]: sources)
		{
			auto parsedAsm = rfl::nlohmann::read<EvmAssemblySourceInput, rfl::NoExtraFields>(sourceValue);
			if (!parsedAsm || !parsedAsm->assemblyJson.is_object())
				return formatFatalError(
					Error::Type::JSONError,
					"Invalid input source specified. Expected exactly one object, named 'assemblyJson', inside $.sources." + sourceName
				);

			ret.jsonSources[sourceName] = std::move(parsedAsm->assemblyJson);
		}
		if (ret.jsonSources.size() != 1)
			return formatFatalError(
				Error::Type::JSONError,
				"EVMAssembly import only supports exactly one input file."
			);
	}

	if (parsedInput->auxiliaryInput && parsedInput->auxiliaryInput->smtlib2responses)
		for (auto const& [hashString, response]: *parsedInput->auxiliaryInput->smtlib2responses)
		{
			util::h256 hash;
			try
			{
				hash = util::h256(hashString);
			}
			catch (util::BadHexCharacter const&)
			{
				return formatFatalError(Error::Type::JSONError, "Invalid hex encoding of SMTLib2 auxiliary input.");
			}
			ret.smtLib2Responses[hash] = response;
		}

	SettingsInput const settings = parsedInput->settings.value_or(SettingsInput{});

	if (settings.experimental)
		ret.experimental = *settings.experimental;

	if (settings.stopAfter)
	{
		auto const stage = CompilerStack::stopAfterFromString(settings.stopAfter->str());
		solAssert(stage.has_value());
		ret.stopAfter = *stage;
	}

	if (settings.viaIR)
		ret.viaIR = *settings.viaIR;

	if (settings.viaSSACFG)
	{
		ret.viaSSACFG = *settings.viaSSACFG;
		if (ret.viaSSACFG)
		{
			if (settings.viaIR && !ret.viaIR)
				return formatFatalError(
					Error::Type::JSONError,
					"\"settings.viaSSACFG\" requires compilation via IR."
				);
			ret.viaIR = true;
		}
	}

	if (settings.evmVersion)
	{
		std::optional<langutil::EVMVersion> version = langutil::EVMVersion::fromString(settings.evmVersion->str());
		solAssert(version.has_value());
		if (version < EVMVersion::london())
			ret.errors.emplace_back(formatError(
				Error::Type::Warning,
				"general",
				"Support for EVM versions older than london is deprecated and will be removed in the future."
			));
		if (version->isExperimental() && !ret.experimental)
			return formatFatalError(
				Error::Type::JSONError,
				fmt::format("EVM version '{}' is experimental and can only be used with the 'settings.experimental' option enabled.", version->name())
			);
		ret.evmVersion = *version;
	}

	if (settings.debug)
	{
		if (settings.debug->revertStrings)
		{
			std::optional<RevertStrings> revertStrings = revertStringsFromString(settings.debug->revertStrings->str());
			solAssert(revertStrings.has_value());
			if (*revertStrings == RevertStrings::VerboseDebug)
				return formatFatalError(
					Error::Type::UnimplementedFeatureError,
					"Only \"default\", \"strip\" and \"debug\" are implemented for settings.debug.revertStrings for now."
				);
			ret.revertStrings = *revertStrings;
		}

		if (settings.debug->debugInfo)
		{
			std::optional<DebugInfoSelection> debugInfoSelection = DebugInfoSelection::fromComponents(
				*settings.debug->debugInfo,
				true /* _acceptWildcards */
			);
			if (!debugInfoSelection.has_value())
				return formatFatalError(Error::Type::JSONError, "Invalid value in settings.debug.debugInfo.");

			if (debugInfoSelection->snippet && !debugInfoSelection->location)
				return formatFatalError(
					Error::Type::JSONError,
					"To use 'snippet' with settings.debug.debugInfo you must select also 'location'."
				);
			if (debugInfoSelection->ethdebug)
			{
				if (!ret.experimental)
					return formatFatalError(Error::Type::FatalError, "Ethdebug annotations are experimental and can only be included in 'settings.debug.debugInfo' by enabling the 'settings.experimental' option.");
				if (!debugInfoSelection->astID)
					return formatFatalError(
						Error::Type::JSONError,
						"To use 'ethdebug' with settings.debug.debugInfo you must select also 'ast-id'."
					);
			}
			ret.debugInfoSelection = debugInfoSelection.value();
		}
	}

	if (settings.remappings)
		for (auto const& remapping: *settings.remappings)
		{
			if (auto r = ImportRemapper::parseRemapping(remapping))
				ret.remappings.emplace_back(std::move(*r));
			else
				return formatFatalError(Error::Type::JSONError, "Invalid remapping: \"" + remapping + "\"");
		}

	if (settings.optimizer)
	{
		auto optimiserSettings = applyOptimizerSettings(ret.language, *settings.optimizer);
		if (std::holds_alternative<Json>(optimiserSettings))
			return std::get<Json>(std::move(optimiserSettings));
		ret.optimiserSettings = std::get<OptimiserSettings>(std::move(optimiserSettings));
	}
	else if (ret.language == "EVMAssembly")
		ret.optimiserSettings = OptimiserSettings::none();
	else
		ret.optimiserSettings = OptimiserSettings::minimal();

	if (settings.libraries)
		for (auto const& [sourceName, libs]: *settings.libraries)
			for (auto const& [library, address]: libs)
			{
				if (!boost::starts_with(address, "0x"))
					return formatFatalError(Error::Type::JSONError, "Library address is not prefixed with \"0x\".");
				if (address.length() != 42)
					return formatFatalError(Error::Type::JSONError, "Library address is of invalid length.");
				try
				{
					ret.libraries[sourceName + ":" + library] = util::h160(address);
				}
				catch (util::BadHexCharacter const&)
				{
					return formatFatalError(Error::Type::JSONError, "Invalid library address (\"" + address + "\") supplied.");
				}
			}

	solAssert(CompilerStack::defaultMetadataFormat() != CompilerStack::MetadataFormat::NoMetadata, "");
	if (settings.metadata)
	{
		ret.metadataFormat =
			settings.metadata->appendCBOR.value_or(true) ?
			CompilerStack::defaultMetadataFormat() :
			CompilerStack::MetadataFormat::NoMetadata;
		ret.metadataLiteralSources = settings.metadata->useLiteralContent.value_or(false);
		if (settings.metadata->bytecodeHash)
		{
			std::string const metadataHash = settings.metadata->bytecodeHash->str();
			ret.metadataHash =
				metadataHash == "ipfs" ?
				CompilerStack::MetadataHash::IPFS :
					metadataHash == "bzzr1" ?
					CompilerStack::MetadataHash::Bzzr1 :
					CompilerStack::MetadataHash::None;
			if (ret.metadataFormat == CompilerStack::MetadataFormat::NoMetadata && ret.metadataHash != CompilerStack::MetadataHash::None)
				return formatFatalError(
					Error::Type::JSONError,
					"When the parameter \"appendCBOR\" is set to false, the parameter \"bytecodeHash\" cannot be set to \"" +
					metadataHash +
					"\". The parameter \"bytecodeHash\" should either be skipped, or set to \"none\"."
				);
		}
	}

	ret.outputSelection = settings.outputSelection ? rfl::nlohmann::write(*settings.outputSelection) : Json::object();

	if (ret.stopAfter != CompilerStack::State::CompilationSuccessful && isBinaryRequested(ret.outputSelection))
		return formatFatalError(
			Error::Type::JSONError,
			"Requested output selection conflicts with \"settings.stopAfter\"."
		);

	if (settings.modelChecker)
	{
		auto const& mc = *settings.modelChecker;

		if (mc.contracts)
		{
			std::map<std::string, std::set<std::string>> sourceContracts;
			for (auto const& [source, contracts]: *mc.contracts)
			{
				if (source.empty())
					return formatFatalError(Error::Type::JSONError, "Source name cannot be empty.");
				for (auto const& contract: contracts)
				{
					if (contract.empty())
						return formatFatalError(Error::Type::JSONError, "Contract name cannot be empty.");
					sourceContracts[source].insert(contract);
				}
				if (sourceContracts[source].empty())
					return formatFatalError(Error::Type::JSONError, "Source contracts must be a non-empty array.");
			}
			ret.modelCheckerSettings.contracts = {std::move(sourceContracts)};
		}

		if (mc.divModNoSlacks)
			ret.modelCheckerSettings.divModNoSlacks = *mc.divModNoSlacks;

		if (mc.engine)
		{
			std::optional<ModelCheckerEngine> engine = ModelCheckerEngine::fromString(mc.engine->str());
			solAssert(engine.has_value());
			ret.modelCheckerSettings.engine = *engine;
		}

		if (mc.bmcLoopIterations)
		{
			if (!ret.modelCheckerSettings.engine.bmc)
				return formatFatalError(Error::Type::JSONError, "settings.modelChecker.bmcLoopIterations requires the BMC engine to be enabled.");
			ret.modelCheckerSettings.bmcLoopIterations = *mc.bmcLoopIterations;
		}

		if (mc.extCalls)
		{
			std::optional<ModelCheckerExtCalls> extCalls = ModelCheckerExtCalls::fromString(mc.extCalls->str());
			solAssert(extCalls.has_value());
			ret.modelCheckerSettings.externalCalls = *extCalls;
		}

		if (mc.invariants)
		{
			ModelCheckerInvariants invariants;
			for (auto const& i: *mc.invariants)
			{
				bool const ok = invariants.setFromString(i.str());
				solAssert(ok);
			}
			if (invariants.invariants.empty())
				return formatFatalError(Error::Type::JSONError, "settings.modelChecker.invariants must be a non-empty array.");
			ret.modelCheckerSettings.invariants = invariants;
		}

		if (mc.showProvedSafe)
			ret.modelCheckerSettings.showProvedSafe = *mc.showProvedSafe;
		if (mc.showUnproved)
			ret.modelCheckerSettings.showUnproved = *mc.showUnproved;
		if (mc.showUnsupported)
			ret.modelCheckerSettings.showUnsupported = *mc.showUnsupported;

		if (mc.solvers)
		{
			smtutil::SMTSolverChoice solvers;
			for (auto const& s: *mc.solvers)
			{
				bool const ok = solvers.setSolver(s.str());
				solAssert(ok);
			}
			ret.modelCheckerSettings.solvers = solvers;
		}

		if (mc.printQuery)
			ret.modelCheckerSettings.printQuery = *mc.printQuery;

		if (mc.targets)
		{
			ModelCheckerTargets targets;
			for (auto const& t: *mc.targets)
			{
				bool const ok = targets.setFromString(t.str());
				solAssert(ok);
			}
			if (targets.targets.empty())
				return formatFatalError(Error::Type::JSONError, "settings.modelChecker.targets must be a non-empty array.");
			ret.modelCheckerSettings.targets = targets;
		}

		if (mc.timeout)
			ret.modelCheckerSettings.timeout = *mc.timeout;
	}

	if ((ret.debugInfoSelection.has_value() && ret.debugInfoSelection->ethdebug) || isAnyEthdebugRequested(ret.outputSelection, ret.language))
	{
		if (ret.language != "Solidity" && ret.language != "Yul")
			return formatFatalError(Error::Type::FatalError, "'settings.debug.debugInfo' 'ethdebug' is only supported for languages 'Solidity' and 'Yul'.");
	}

	if (isEthdebugProgramRequested(ret.outputSelection) && ret.language == "Solidity" && !ret.viaIR)
		return formatFatalError(Error::Type::FatalError, "'evm.bytecode.ethdebug' or 'evm.deployedBytecode.ethdebug' can only be selected as output, if 'viaIR' was set.");

	bool const ethdebugSelected = ret.debugInfoSelection.has_value() && ret.debugInfoSelection->ethdebug;
	if (ethdebugSelected && ret.optimiserSettings.runYulOptimiser)
		solUnimplemented("Optimization is not yet supported with ethdebug.");
	// The SSA CFG code generator attaches no source locations, which the
	// program output consists of, so the output is gated as well.
	if ((ethdebugSelected || isEthdebugProgramRequested(ret.outputSelection)) && ret.viaSSACFG)
		solUnimplemented("SSA CFG codegen does not yet support ethdebug.");

	if (!ret.experimental)
	{
		if (ret.language == "SolidityAST" || ret.language == "EVMAssembly")
			return formatFatalError(Error::Type::FatalError, "'SolidityAST' and 'EVMAssembly' inputs are experimental and can only be used with the 'settings.experimental' option enabled.");

		if (ret.evmVersion.isExperimental())
			// TODO: Cover with test when the Amsterdam version is introduced
			return formatFatalError(Error::Type::FatalError, fmt::format("EVM version '{}' is experimental and can only be used with the 'settings.experimental' option enabled.", ret.evmVersion.name()));

		if (isExperimentalArtifactRequested(ret.outputSelection, ret.language))
			return formatFatalError(Error::Type::FatalError, "'irAst', 'irOptimizedAst', and 'ethdebug' outputs are experimental and can only be used with the 'settings.experimental' option enabled.");

		if (ret.viaSSACFG)
			return formatFatalError(Error::Type::FatalError, "'viaSSACFG' setting is experimental and can only be used with the 'settings.experimental' option enabled.");
	}

	return {std::move(ret)};
}

std::map<std::string, Json> StandardCompiler::parseAstFromInput(StringMap const& _sources)
{
	std::map<std::string, Json> sourceJsons;
	for (auto const& [sourceName, sourceCode]: _sources)
	{
		auto envelope = rfl::json::read<AstSourceEnvelope>(sourceCode, YYJSON_READ_ALLOW_COMMENTS);
		astAssert(envelope.has_value(), "Input file could not be parsed to JSON");
		auto const& rawAst = envelope->ast ? envelope->ast : envelope->AST;
		astAssert(rawAst.has_value(), "astkey is not member");
		auto rootNode = rfl::json::read<AstRootNode>(rawAst->str());
		astAssert(rootNode.has_value() && rootNode->nodeType == "SourceUnit", "Top-level node should be a 'SourceUnit'");
		Json astNode;
		astAssert(util::jsonParseStrict(rawAst->str(), astNode), "Input file could not be parsed to JSON");
		astAssert(sourceJsons.count(sourceName) == 0, "All sources must have unique names");
		sourceJsons.emplace(sourceName, std::move(astNode));
	}
	return sourceJsons;
}

Json StandardCompiler::importEVMAssembly(StandardCompiler::InputsAndSettings _inputsAndSettings)
{
	solAssert(_inputsAndSettings.language == "EVMAssembly");
	solAssert(_inputsAndSettings.sources.empty());
	solAssert(_inputsAndSettings.jsonSources.size() == 1);
	solAssert(_inputsAndSettings.experimental);

	if (!isBinaryRequested(_inputsAndSettings.outputSelection))
		return Json::object();

	evmasm::EVMAssemblyStack stack(
		_inputsAndSettings.evmVersion,
		evmasm::Assembly::OptimiserSettings::translateSettings(
			_inputsAndSettings.optimiserSettings
		)
	);
	std::string const& sourceName = _inputsAndSettings.jsonSources.begin()->first; // result of structured binding can only be used within lambda from C++20 on.
	Json const& sourceJson = _inputsAndSettings.jsonSources.begin()->second;
	try
	{
		stack.analyze(sourceName, sourceJson);
		stack.assemble();
	}
	catch (evmasm::AssemblyImportException const& e)
	{
		return formatFatalError(Error::Type::Exception, "Assembly import error: " + std::string(e.what()));
	}
	catch (...)
	{
		return formatError(
			Error::Type::Exception,
			"general",
			"Unknown exception during assembly import: " + boost::current_exception_diagnostic_information()
		);
	}
	if (!stack.compilationSuccessful())
		return Json::object();

	// EVM
	bool const wildcardMatchesExperimental = false;
	Json evmData;
	if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, "", "evm.assembly", wildcardMatchesExperimental))
		evmData["assembly"] = stack.assemblyString(sourceName, {});
	if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, "", "evm.legacyAssembly", wildcardMatchesExperimental))
		evmData["legacyAssembly"] = stack.assemblyJSON(sourceName);

	if (isArtifactRequested(
		_inputsAndSettings.outputSelection,
		sourceName,
		"",
		evmObjectComponents("bytecode"),
		wildcardMatchesExperimental
	))
	{
		auto const evmCreationArtifactRequested = [&](std::string const& _element) {
			return isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, "", "evm.bytecode." + _element, wildcardMatchesExperimental);
		};

		Json creationJSON;
		if (evmCreationArtifactRequested("object"))
			creationJSON["object"] = stack.object(sourceName).toHex();
		if (evmCreationArtifactRequested("opcodes"))
			creationJSON["opcodes"] = evmasm::disassemble(stack.object(sourceName).bytecode, _inputsAndSettings.evmVersion);
		if (evmCreationArtifactRequested("sourceMap"))
			creationJSON["sourceMap"] = stack.sourceMapping(sourceName) ? *stack.sourceMapping(sourceName) : "";
		if (evmCreationArtifactRequested("functionDebugData"))
			creationJSON["functionDebugData"] = formatFunctionDebugData(stack.object(sourceName).functionDebugData);
		if (evmCreationArtifactRequested("linkReferences"))
			creationJSON["linkReferences"] = formatLinkReferences(stack.object(sourceName).linkReferences);
		if (evmCreationArtifactRequested("ethdebug"))
			creationJSON["ethdebug"] = stack.ethdebug(sourceName);
		evmData["bytecode"] = creationJSON;
	}

	if (isArtifactRequested(
		_inputsAndSettings.outputSelection,
		sourceName,
		"",
		evmObjectComponents("deployedBytecode"),
		wildcardMatchesExperimental
	))
	{
		auto const evmDeployedArtifactRequested = [&](std::string const& _element) {
			return isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, "", "evm.deployedBytecode." + _element, wildcardMatchesExperimental);
		};

		Json deployedJSON;
		if (evmDeployedArtifactRequested("object"))
			deployedJSON["object"] = stack.runtimeObject(sourceName).toHex();
		if (evmDeployedArtifactRequested("opcodes"))
			deployedJSON["opcodes"] = evmasm::disassemble(stack.runtimeObject(sourceName).bytecode, _inputsAndSettings.evmVersion);
		if (evmDeployedArtifactRequested("sourceMap"))
			deployedJSON["sourceMap"] = stack.runtimeSourceMapping(sourceName) ? *stack.runtimeSourceMapping(sourceName) : "";
		if (evmDeployedArtifactRequested("functionDebugData"))
			deployedJSON["functionDebugData"] = formatFunctionDebugData(stack.runtimeObject(sourceName).functionDebugData);
		if (evmDeployedArtifactRequested("linkReferences"))
			deployedJSON["linkReferences"] = formatLinkReferences(stack.runtimeObject(sourceName).linkReferences);
		if (evmDeployedArtifactRequested("immutableReferences"))
			deployedJSON["immutableReferences"] = formatImmutableReferences(stack.runtimeObject(sourceName).immutableReferences);
		if (evmDeployedArtifactRequested("ethdebug"))
			deployedJSON["ethdebug"] = stack.ethdebugRuntime(sourceName);
		evmData["deployedBytecode"] = deployedJSON;
	}

	Json contractData;
	if (!evmData.empty())
		contractData["evm"] = evmData;

	Json contractsOutput;
	contractsOutput[sourceName][""] = contractData;
	Json output;
	output["contracts"] = contractsOutput;
	return util::removeNullMembers(output);
}

Json StandardCompiler::compileSolidity(StandardCompiler::InputsAndSettings _inputsAndSettings)
{
	solAssert(_inputsAndSettings.jsonSources.empty());

	CompilerStack compilerStack(m_readFile);

	StringMap sourceList = std::move(_inputsAndSettings.sources);
	if (_inputsAndSettings.language == "Solidity")
		compilerStack.setSources(sourceList);
	for (auto const& smtLib2Response: _inputsAndSettings.smtLib2Responses)
		compilerStack.addSMTLib2Response(smtLib2Response.first, smtLib2Response.second);
	compilerStack.setViaIR(_inputsAndSettings.viaIR);
	compilerStack.setViaSSACFG(_inputsAndSettings.viaSSACFG);
	compilerStack.setEVMVersion(_inputsAndSettings.evmVersion);
	compilerStack.setRemappings(std::move(_inputsAndSettings.remappings));
	compilerStack.setOptimiserSettings(std::move(_inputsAndSettings.optimiserSettings));
	compilerStack.setRevertStringBehaviour(_inputsAndSettings.revertStrings);
	if (_inputsAndSettings.debugInfoSelection.has_value())
		compilerStack.selectDebugInfo(_inputsAndSettings.debugInfoSelection.value());
	compilerStack.setLibraries(_inputsAndSettings.libraries);
	compilerStack.useMetadataLiteralSources(_inputsAndSettings.metadataLiteralSources);
	compilerStack.setMetadataFormat(_inputsAndSettings.metadataFormat);
	compilerStack.setMetadataHash(_inputsAndSettings.metadataHash);
	compilerStack.selectContracts(pipelineConfig(_inputsAndSettings.outputSelection));
	compilerStack.setModelCheckerSettings(_inputsAndSettings.modelCheckerSettings);
	compilerStack.setExperimental(_inputsAndSettings.experimental);

	Json errors = std::move(_inputsAndSettings.errors);

	bool const binariesRequested = isBinaryRequested(_inputsAndSettings.outputSelection);

	try
	{
		if (_inputsAndSettings.language == "SolidityAST")
		{
			try
			{
				compilerStack.importASTs(parseAstFromInput(sourceList));
				if (!compilerStack.analyze())
					errors.emplace_back(formatError(Error::Type::FatalError, "general", "Analysis of the AST failed."));
				if (binariesRequested)
					compilerStack.compile();
			}
			catch (util::Exception const& _exc)
			{
				solThrow(util::Exception, "Failed to import AST: "s + _exc.what());
			}
		}
		else
		{
			if (binariesRequested)
				compilerStack.compile();
			else
				compilerStack.parseAndAnalyze(_inputsAndSettings.stopAfter);

			for (auto const& error: compilerStack.errors())
				errors.emplace_back(formatErrorWithException(
					compilerStack,
					*error,
					error->type(),
					"general",
					"",
					error->errorId()
				));
		}
	}
	// NOTE: This includes langutil::StackTooDeepError.
	catch (CompilerError const& _exception)
	{
		errors.emplace_back(formatErrorWithException(
			compilerStack,
			_exception,
			Error::Type::CompilerError,
			"general",
			"Compiler error (" + _exception.lineInfo() + ")"
		));
	}
	catch (yul::StackTooDeepError const& _exception)
	{
		errors.emplace_back(formatErrorWithException(
			compilerStack,
			_exception,
			Error::Type::YulException,
			"general",
			"" // No prefix needed. These messages already say it's a "stack too deep" error.
		));
	}
	catch (InternalCompilerError const&)
	{
		errors.emplace_back(formatError(
			Error::Type::InternalCompilerError,
			"general",
			"Internal compiler error:\n" + boost::current_exception_diagnostic_information()
		));
	}
	catch (UnimplementedFeatureError const& _exception)
	{
		// let StandardCompiler::compile handle this
		throw _exception;
	}
	catch (YulAssertion const&)
	{
		errors.emplace_back(formatError(
			Error::Type::YulException,
			"general",
			"Yul assertion failed:\n" + boost::current_exception_diagnostic_information()
		));
	}
	catch (smtutil::SMTLogicError const&)
	{
		errors.emplace_back(formatError(
			Error::Type::SMTLogicException,
			"general",
			"SMT logic error:\n" + boost::current_exception_diagnostic_information()
		));
	}
	catch (...)
	{
		errors.emplace_back(formatError(
			Error::Type::Exception,
			"general",
			"Unknown exception during compilation: " + boost::current_exception_diagnostic_information()
		));
	}

	bool parsingSuccess = compilerStack.state() >= CompilerStack::State::Parsed;
	bool analysisSuccess = compilerStack.state() >= CompilerStack::State::AnalysisSuccessful;
	bool compilationSuccess = compilerStack.state() == CompilerStack::State::CompilationSuccessful;

	// If analysis fails, the artifacts inside CompilerStack are potentially incomplete and must not be returned.
	// Note that not completing analysis due to stopAfter does not count as a failure. It's neither failure nor success.
	bool analysisFailed = !analysisSuccess && _inputsAndSettings.stopAfter >= CompilerStack::State::AnalysisSuccessful;
	bool compilationFailed = !compilationSuccess && binariesRequested;
	if (compilationFailed || analysisFailed || !parsingSuccess)
		solAssert(!errors.empty(), "No error reported, but compilation failed.");

	Json output;

	if (errors.size() > 0)
		output["errors"] = std::move(errors);

	if (!compilerStack.unhandledSMTLib2Queries().empty())
		for (std::string const& query: compilerStack.unhandledSMTLib2Queries())
			output["auxiliaryInputRequested"]["smtlib2queries"]["0x" + util::keccak256(query).hex()] = query;

	bool const wildcardMatchesExperimental = false;

	output["sources"] = Json::object();
	unsigned sourceIndex = 0;
	// NOTE: A case that will pass `parsingSuccess && !analysisFailed` but not `analysisSuccess` is
	// stopAfter: parsing with no parsing errors.
	if (parsingSuccess && !analysisFailed)
		for (std::string const& sourceName: compilerStack.sourceNames())
		{
			Json sourceResult;
			sourceResult["id"] = sourceIndex++;
			if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, "", "ast", wildcardMatchesExperimental))
				sourceResult["ast"] = ASTJsonExporter(compilerStack.state(), compilerStack.sourceIndices()).toJson(compilerStack.ast(sourceName));
			output["sources"][sourceName] = sourceResult;
		}

	Json contractsOutput;
	for (std::string const& contractName: analysisSuccess ? compilerStack.contractNames() : std::vector<std::string>())
	{
		size_t colon = contractName.rfind(':');
		solAssert(colon != std::string::npos, "");
		std::string file = contractName.substr(0, colon);
		std::string name = contractName.substr(colon + 1);

		// ABI, storage layout, documentation and metadata
		Json contractData;
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "abi", wildcardMatchesExperimental))
			contractData["abi"] = compilerStack.contractABI(contractName);
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "storageLayout", false))
			contractData["storageLayout"] = compilerStack.storageLayout(contractName);
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "transientStorageLayout", false))
			contractData["transientStorageLayout"] = compilerStack.transientStorageLayout(contractName);
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "metadata", wildcardMatchesExperimental))
			contractData["metadata"] = compilerStack.metadata(contractName);
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "userdoc", wildcardMatchesExperimental))
			contractData["userdoc"] = compilerStack.natspecUser(contractName);
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "devdoc", wildcardMatchesExperimental))
			contractData["devdoc"] = compilerStack.natspecDev(contractName);

		// IR
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "ir", wildcardMatchesExperimental))
			contractData["ir"] = compilerStack.yulIR(contractName).value_or("");
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "irAst", wildcardMatchesExperimental))
			contractData["irAst"] = compilerStack.yulIRAst(contractName).value_or(Json{});
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "irOptimized", wildcardMatchesExperimental))
			contractData["irOptimized"] = compilerStack.yulIROptimized(contractName).value_or("");
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "irOptimizedAst", wildcardMatchesExperimental))
			contractData["irOptimizedAst"] = compilerStack.yulIROptimizedAst(contractName).value_or(Json{});

		// EVM
		Json evmData;
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "evm.assembly", wildcardMatchesExperimental))
			evmData["assembly"] = compilerStack.assemblyString(contractName, sourceList);
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "evm.legacyAssembly", wildcardMatchesExperimental))
			evmData["legacyAssembly"] = compilerStack.assemblyJSON(contractName);
		if (isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "evm.methodIdentifiers", wildcardMatchesExperimental))
			evmData["methodIdentifiers"] = compilerStack.interfaceSymbols(contractName)["methods"];
		if (compilationSuccess && isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "evm.gasEstimates", wildcardMatchesExperimental))
			evmData["gasEstimates"] = compilerStack.gasEstimates(contractName);

		if (compilationSuccess && isArtifactRequested(
			_inputsAndSettings.outputSelection,
			file,
			name,
			evmObjectComponents("bytecode"),
			wildcardMatchesExperimental
		))
		{
			auto const evmCreationArtifactRequested = [&](std::string const& _element) {
				return isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "evm.bytecode." + _element, wildcardMatchesExperimental);
			};

			Json creationJSON;
			if (evmCreationArtifactRequested("object"))
				creationJSON["object"] = compilerStack.object(contractName).toHex();
			if (evmCreationArtifactRequested("opcodes"))
				creationJSON["opcodes"] = evmasm::disassemble(compilerStack.object(contractName).bytecode, _inputsAndSettings.evmVersion);
			if (evmCreationArtifactRequested("sourceMap"))
				creationJSON["sourceMap"] = compilerStack.sourceMapping(contractName) ? *compilerStack.sourceMapping(contractName) : "";
			if (evmCreationArtifactRequested("functionDebugData"))
				creationJSON["functionDebugData"] = formatFunctionDebugData(compilerStack.object(contractName).functionDebugData);
			if (evmCreationArtifactRequested("linkReferences"))
				creationJSON["linkReferences"] = formatLinkReferences(compilerStack.object(contractName).linkReferences);
			if (evmCreationArtifactRequested("generatedSources"))
				creationJSON["generatedSources"] = compilerStack.generatedSources(contractName, /* _runtime */ false);
			if (evmCreationArtifactRequested("ethdebug"))
				creationJSON["ethdebug"] = compilerStack.ethdebug(contractName);
			evmData["bytecode"] = creationJSON;
		}

		if (compilationSuccess && isArtifactRequested(
			_inputsAndSettings.outputSelection,
			file,
			name,
			evmObjectComponents("deployedBytecode"),
			wildcardMatchesExperimental
		))
		{
			auto const evmDeployedArtifactRequested = [&](std::string const& _element) {
				return isArtifactRequested(_inputsAndSettings.outputSelection, file, name, "evm.deployedBytecode." + _element, wildcardMatchesExperimental);
			};

			Json deployedJSON;
			if (evmDeployedArtifactRequested("object"))
				deployedJSON["object"] = compilerStack.runtimeObject(contractName).toHex();
			if (evmDeployedArtifactRequested("opcodes"))
				deployedJSON["opcodes"] = evmasm::disassemble(compilerStack.runtimeObject(contractName).bytecode, _inputsAndSettings.evmVersion);
			if (evmDeployedArtifactRequested("sourceMap"))
				deployedJSON["sourceMap"] = compilerStack.runtimeSourceMapping(contractName) ? *compilerStack.runtimeSourceMapping(contractName) : "";
			if (evmDeployedArtifactRequested("functionDebugData"))
				deployedJSON["functionDebugData"] = formatFunctionDebugData(compilerStack.runtimeObject(contractName).functionDebugData);
			if (evmDeployedArtifactRequested("linkReferences"))
				deployedJSON["linkReferences"] = formatLinkReferences(compilerStack.runtimeObject(contractName).linkReferences);
			if (evmDeployedArtifactRequested("immutableReferences"))
				deployedJSON["immutableReferences"] = formatImmutableReferences(compilerStack.runtimeObject(contractName).immutableReferences);
			if (evmDeployedArtifactRequested("generatedSources"))
				deployedJSON["generatedSources"] = compilerStack.generatedSources(contractName, /* _runtime */ true);
			if (evmDeployedArtifactRequested("ethdebug"))
				deployedJSON["ethdebug"] = compilerStack.ethdebugRuntime(contractName);
			evmData["deployedBytecode"] = deployedJSON;
		}

		if (!evmData.empty())
			contractData["evm"] = evmData;

		if (!contractData.empty())
		{
			if (!contractsOutput.contains(file))
				contractsOutput[file] = Json::object();
			contractsOutput[file][name] = contractData;
		}
	}

	if (analysisSuccess)
	{
		if (isEthdebugGlobalOutputRequested(_inputsAndSettings.outputSelection, "ethdebug.resources"))
			output["ethdebug"]["resources"] = compilerStack.ethdebug();
		if (isEthdebugGlobalOutputRequested(_inputsAndSettings.outputSelection, "ethdebug.compilation"))
			output["ethdebug"]["compilation"] = compilerStack.ethdebugCompilation();
	}

	if (!contractsOutput.empty())
		output["contracts"] = contractsOutput;

	return output;
}


Json StandardCompiler::compileYul(InputsAndSettings _inputsAndSettings)
{
	solAssert(_inputsAndSettings.jsonSources.empty());

	Json output;
	output["errors"] = std::move(_inputsAndSettings.errors);

	if (_inputsAndSettings.sources.size() != 1)
	{
		output["errors"].emplace_back(formatError(
			Error::Type::JSONError,
			"general",
			"Yul mode only supports exactly one input file."
		));
		return output;
	}
	if (!_inputsAndSettings.smtLib2Responses.empty())
	{
		output["errors"].emplace_back(formatError(
			Error::Type::JSONError,
			"general",
			"Yul mode does not support smtlib2responses."
		));
		return output;
	}
	if (!_inputsAndSettings.remappings.empty())
	{
		output["errors"].emplace_back(formatError(
			Error::Type::JSONError,
			"general",
			"Field \"settings.remappings\" cannot be used for Yul."
		));
		return output;
	}
	if (_inputsAndSettings.revertStrings != RevertStrings::Default)
	{
		output["errors"].emplace_back(formatError(
			Error::Type::JSONError,
			"general",
			"Field \"settings.debug.revertStrings\" cannot be used for Yul."
		));
		return output;
	}

	YulStack stack(
		_inputsAndSettings.evmVersion,
		_inputsAndSettings.optimiserSettings,
		_inputsAndSettings.debugInfoSelection.has_value() ?
			_inputsAndSettings.debugInfoSelection.value() :
			DebugInfoSelection::Default()
	);
	std::string const& sourceName = _inputsAndSettings.sources.begin()->first;
	std::string const& sourceContents = _inputsAndSettings.sources.begin()->second;

	std::string contractName;
	bool const wildcardMatchesExperimental = true;
	MachineAssemblyObject object;
	MachineAssemblyObject deployedObject;

	bool successful = stack.parseAndAnalyze(sourceName, sourceContents);
	if (!successful)
		// Inconsistent state - stop here to receive error reports from users
		solAssert(stack.hasErrors(), "No error reported, but parsing/analysis failed.");
	else
	{
		contractName = stack.parserResult()->name;
		if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, contractName, "ir", wildcardMatchesExperimental))
			output["contracts"][sourceName][contractName]["ir"] = stack.print();
		if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, contractName, "ast", wildcardMatchesExperimental))
		{
			Json sourceResult;
			sourceResult["id"] = 0;
			sourceResult["ast"] = stack.astJson();
			output["sources"][sourceName] = sourceResult;
		}
		stack.optimize(_inputsAndSettings.viaSSACFG);
		std::tie(object, deployedObject) = stack.assembleWithDeployed({}, _inputsAndSettings.viaSSACFG);
		if (object.bytecode)
			object.bytecode->link(_inputsAndSettings.libraries);
		if (deployedObject.bytecode)
			deployedObject.bytecode->link(_inputsAndSettings.libraries);
	}

	for (auto const& error: stack.errors())
	{
		auto err = std::dynamic_pointer_cast<Error const>(error);

		output["errors"].emplace_back(formatErrorWithException(
			stack,
			*error,
			err->type(),
			"general",
			""
		));
	}
	if (stack.hasErrors())
		return output;

	for (auto&& [kind, isDeployed]: {make_pair("bytecode"s, false), make_pair("deployedBytecode"s, true)})
		if (isArtifactRequested(
			_inputsAndSettings.outputSelection,
			sourceName,
			contractName,
			evmObjectComponents(kind),
			wildcardMatchesExperimental
		))
		{
			auto const evmArtifactRequested = [&](std::string const& _kind, std::string const& _element) {
				return isArtifactRequested(
					_inputsAndSettings.outputSelection,
					sourceName,
					contractName,
					"evm." + _kind + "." + _element,
					wildcardMatchesExperimental
				);
			};

			MachineAssemblyObject const& selectedObject = isDeployed ? deployedObject : object;
			if (selectedObject.bytecode)
			{
				Json bytecodeJSON;
				if (evmArtifactRequested(kind, "object"))
					bytecodeJSON["object"] = selectedObject.bytecode->toHex();
				if (evmArtifactRequested(kind, "opcodes"))
					bytecodeJSON["opcodes"] = evmasm::disassemble(selectedObject.bytecode->bytecode, _inputsAndSettings.evmVersion);
				if (evmArtifactRequested(kind, "sourceMap"))
					bytecodeJSON["sourceMap"] = selectedObject.sourceMappings ? *selectedObject.sourceMappings : "";
				if (evmArtifactRequested(kind, "functionDebugData"))
					bytecodeJSON["functionDebugData"] = formatFunctionDebugData(selectedObject.bytecode->functionDebugData);
				if (evmArtifactRequested(kind, "linkReferences"))
					bytecodeJSON["linkReferences"] = formatLinkReferences(selectedObject.bytecode->linkReferences);
				if (evmArtifactRequested(kind, "ethdebug"))
				{
					solAssert(_inputsAndSettings.experimental);
					bytecodeJSON["ethdebug"] = selectedObject.ethdebug;
				}
				if (isDeployed && evmArtifactRequested(kind, "immutableReferences"))
					bytecodeJSON["immutableReferences"] = formatImmutableReferences(selectedObject.bytecode->immutableReferences);
				output["contracts"][sourceName][contractName]["evm"][kind] = bytecodeJSON;
			}
		}

	if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, contractName, "irOptimized", wildcardMatchesExperimental))
		output["contracts"][sourceName][contractName]["irOptimized"] = stack.print();
	if (isArtifactRequested(_inputsAndSettings.outputSelection, sourceName, contractName, "evm.assembly", wildcardMatchesExperimental))
		output["contracts"][sourceName][contractName]["evm"]["assembly"] = object.assembly->assemblyString(stack.debugInfoSelection());

	return output;
}

Json StandardCompiler::compile(Json const& _input) noexcept
{
	YulStringRepository::reset();

	try
	{
		auto parsed = parseInput(_input);
		if (std::holds_alternative<Json>(parsed))
			return std::get<Json>(std::move(parsed));
		InputsAndSettings settings = std::get<InputsAndSettings>(std::move(parsed));
		if (settings.language == "Solidity")
			return compileSolidity(std::move(settings));
		else if (settings.language == "Yul")
			return compileYul(std::move(settings));
		else if (settings.language == "SolidityAST")
			return compileSolidity(std::move(settings));
		else if (settings.language == "EVMAssembly")
			return importEVMAssembly(std::move(settings));
		else
			return formatFatalError(Error::Type::JSONError, "Only \"Solidity\", \"Yul\", \"SolidityAST\" or \"EVMAssembly\" is supported as a language.");
	}
	catch (UnimplementedFeatureError const& _exception)
	{
		solAssert(_exception.comment(), "Unimplemented feature errors must include a message for the user");
		return formatFatalError(Error::Type::UnimplementedFeatureError, stringOrDefault(_exception.comment()));
	}
	catch (...)
	{
		return formatFatalError(
			Error::Type::InternalCompilerError,
			"Uncaught exception:\n" + boost::current_exception_diagnostic_information()
		);
	}
}

std::string StandardCompiler::compile(std::string const& _input) noexcept
{
	Json input;
	std::string errors;
	try
	{
		if (!util::jsonParseStrict(_input, input, &errors))
			return util::jsonPrint(formatFatalError(Error::Type::JSONError, errors), m_jsonPrintingFormat);
	}
	catch (...)
	{
		if (errors.empty())
			return "{\"errors\":[{\"type\":\"JSONError\",\"component\":\"general\",\"severity\":\"error\",\"message\":\"Error parsing input JSON.\"}]}";
		else
			return "{\"errors\":[{\"type\":\"JSONError\",\"component\":\"general\",\"severity\":\"error\",\"message\":\"Error parsing input JSON: " + errors + "\"}]}";
	}

//	std::cout << "Input: " << solidity::util::jsonPrettyPrint(input) << std::endl;
	Json output = compile(input);
//	std::cout << "Output: " << solidity::util::jsonPrettyPrint(output) << std::endl;

	try
	{
		return util::jsonPrint(output, m_jsonPrintingFormat);
	}
	catch (...)
	{
		return "{\"errors\":[{\"type\":\"JSONError\",\"component\":\"general\",\"severity\":\"error\",\"message\":\"Error writing output JSON.\"}]}";
	}
}

std::string StandardCompiler::inputSchema(util::JsonFormat const& _format)
{
	std::string raw = rfl::json::to_schema<standard_json::StandardInput, rfl::NoExtraFields>(
		0,
		"Solidity Standard JSON compiler input schema (generated from standard_json::StandardInput)"
	);
	if (_format.format == util::JsonFormat::Pretty)
		return util::jsonPrint(Json::parse(raw), _format);
	return raw;
}

Json StandardCompiler::formatFunctionDebugData(
	std::map<std::string, evmasm::LinkerObject::FunctionDebugData> const& _debugInfo
)
{
	static_assert(std::is_same_v<Json::number_unsigned_t, uint64_t>);
	Json ret = Json::object();
	for (auto const& [name, info]: _debugInfo)
	{
		Json fun = Json::object();
		if (info.sourceID)
			fun["id"] = Json::number_unsigned_t(*info.sourceID);
		else
			fun["id"] = Json();
		if (info.bytecodeOffset)
			fun["entryPoint"] = Json::number_unsigned_t(*info.bytecodeOffset);
		else
			fun["entryPoint"] = Json();
		fun["parameterSlots"] = Json::number_unsigned_t(info.params);
		fun["returnSlots"] = Json::number_unsigned_t(info.returns);
		ret[name] = std::move(fun);
	}

	return ret;
}
