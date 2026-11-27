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

#include <solc/CommandLineParser.h>

#include <solc/Exceptions.h>

#include <libyul/optimiser/Suite.h>

#include <liblangutil/EVMVersion.h>

#include <boost/algorithm/string.hpp>

#include <range/v3/view/transform.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/range/conversion.hpp>

#include <fmt/format.h>

#include <rfl.hpp>
#include <rfl/cli.hpp>

using namespace solidity::langutil;
using namespace solidity::yul;

namespace solidity::frontend
{

static std::string const g_strAllowPaths = "allow-paths";
static std::string const g_strIncludePath = "include-path";
static std::string const g_strCombinedJson = "combined-json";
static std::string const g_strEVM = "evm";
static std::string const g_strEVMVersion = "evm-version";
static std::string const g_strViaIR = "via-ir";
static std::string const g_strViaSSACFG = "via-ssa-cfg";
static std::string const g_strExperimentalViaIR = "experimental-via-ir";
static std::string const g_strExperimental = "experimental";
static std::string const g_strGas = "gas";
static std::string const g_strHelp = "help";
static std::string const g_strImportAst = "import-ast";
static std::string const g_strImportEvmAssemblerJson = "import-asm-json";
static std::string const g_strInputFile = "input-file";
static std::string const g_strYulDialect = "yul-dialect";
static std::string const g_strDebugInfo = "debug-info";
static std::string const g_strIPFS = "ipfs";
static std::string const g_strLicense = "license";
static std::string const g_strLink = "link";
static std::string const g_strMachine = "machine";
static std::string const g_strNoCBORMetadata = "no-cbor-metadata";
static std::string const g_strMetadataHash = "metadata-hash";
static std::string const g_strMetadataLiteral = "metadata-literal";
static std::string const g_strModelCheckerContracts = "model-checker-contracts";
static std::string const g_strModelCheckerDivModNoSlacks = "model-checker-div-mod-no-slacks";
static std::string const g_strModelCheckerEngine = "model-checker-engine";
static std::string const g_strModelCheckerExtCalls = "model-checker-ext-calls";
static std::string const g_strModelCheckerInvariants = "model-checker-invariants";
static std::string const g_strModelCheckerPrintQuery = "model-checker-print-query";
static std::string const g_strModelCheckerShowProvedSafe = "model-checker-show-proved-safe";
static std::string const g_strModelCheckerShowUnproved = "model-checker-show-unproved";
static std::string const g_strModelCheckerShowUnsupported = "model-checker-show-unsupported";
static std::string const g_strModelCheckerSolvers = "model-checker-solvers";
static std::string const g_strModelCheckerTargets = "model-checker-targets";
static std::string const g_strModelCheckerTimeout = "model-checker-timeout";
static std::string const g_strModelCheckerBMCLoopIterations = "model-checker-bmc-loop-iterations";
static std::string const g_strNone = "none";
static std::string const g_strNoOptimizeYul = "no-optimize-yul";
static std::string const g_strNoImportCallback = "no-import-callback";
static std::string const g_strOptimize = "optimize";
static std::string const g_strOptimizeRuns = "optimize-runs";
static std::string const g_strOptimizeYul = "optimize-yul";
static std::string const g_strYulOptimizations = "yul-optimizations";
static std::string const g_strOutputDir = "output-dir";
static std::string const g_strRevertStrings = "revert-strings";
static std::string const g_strStopAfter = "stop-after";
static std::string const g_strStandardJSON = "standard-json";
static std::string const g_strStrictAssembly = "strict-assembly";
static std::string const g_strSwarm = "swarm";
static std::string const g_strPrettyJson = "pretty-json";
static std::string const g_strJsonIndent = "json-indent";
static std::string const g_strVersion = "version";
static std::string const g_strColor = "color";
static std::string const g_strNoColor = "no-color";

static std::map<InputMode, std::string> const g_inputModeName = {
	{InputMode::Help, "help"},
	{InputMode::License, "license"},
	{InputMode::Version, "version"},
	{InputMode::Compiler, "compiler"},
	{InputMode::CompilerWithASTImport, "compiler (AST import)"},
	{InputMode::Assembler, "assembler"},
	{InputMode::StandardJson, "standard JSON"},
	{InputMode::Linker, "linker"},
	{InputMode::EVMAssemblerJSON, "EVM assembler (JSON format)"},
};

namespace
{

using Flag = rfl::DefaultVal<bool>;
using OptStr = std::optional<std::string>;
using OptStrVec = std::optional<std::vector<std::string>>;

template <rfl::internal::StringLiteral G, rfl::internal::StringLiteral Desc>
using GroupFlag = rfl::cli::Opt<Flag, rfl::cli::attr::Group<G>, rfl::cli::attr::Desc<Desc>>;

template <rfl::internal::StringLiteral G, rfl::internal::StringLiteral VN, rfl::internal::StringLiteral Desc, class T = OptStr>
using GroupOpt = rfl::cli::Opt<T, rfl::cli::attr::Group<G>, rfl::cli::attr::ValueName<VN>, rfl::cli::attr::Desc<Desc>>;

template <rfl::internal::StringLiteral Desc>
using OutCompFlag = GroupFlag<"Output Components", Desc>;

struct GeneralInfoArgs
{
	GroupFlag<"General Information", "Show help message and exit."> help = false;
	GroupFlag<"General Information", "Show version and exit."> version = false;
	GroupFlag<"General Information", "Show licensing information and exit."> license = false;
	rfl::cli::Opt<OptStrVec, rfl::cli::attr::Positional, rfl::cli::attr::Group<"General Information">,
		rfl::cli::attr::ValueName<"arg">, rfl::cli::attr::Desc<"input file">> input_file;
};

struct InputArgs
{
	GroupOpt<"Input Options", "path",
		"Use the given path as the root of the source tree instead of the root of the filesystem."> base_path;
	GroupOpt<"Input Options", "path",
		"Make an additional source directory available to the default import callback. "
		"Use this option if you want to import contracts whose location is not fixed in relation "
		"to your main source tree, e.g. third-party libraries installed using a package manager. "
		"Can be used multiple times. "
		"Can only be used if base path has a non-empty value.",
		OptStrVec> include_path;
	GroupOpt<"Input Options", "path(s)",
		"Allow a given path for imports. A list of paths can be supplied by separating them with a comma."> allow_paths;
	GroupFlag<"Input Options", "Ignore missing files."> ignore_missing = false;
	GroupFlag<"Input Options",
		"Disable the default import callback to prevent the compiler from loading any source "
		"files not listed on the command line or given in the Standard JSON input."> no_import_callback = false;
};

struct OutputArgs
{
	rfl::cli::Opt<OptStr, rfl::cli::attr::Short<"o">, rfl::cli::attr::Group<"Output Options">,
		rfl::cli::attr::ValueName<"path">,
		rfl::cli::attr::Desc<"If given, creates one file per output component and contract/file at the specified directory.">> output_dir;
	GroupFlag<"Output Options", "Overwrite existing files (used together with -o)."> overwrite = false;
	GroupOpt<"Output Options", "version", "Select desired EVM version."> evm_version;
	GroupFlag<"Output Options", "Deprecated synonym of --via-ir."> experimental_via_ir = false;
	GroupFlag<"Output Options", "Turn on compilation via IR."> via_ir = false;
	GroupFlag<"Output Options", "(experimental) Turn on SSA CFG-based code generation. Implies compilation via IR."> via_ssa_cfg = false;
	GroupOpt<"Output Options", "debug,default,strip,verboseDebug",
		"Strip revert (and require) reason strings or add additional debugging information. "
		"Note that 'strip' does not remove custom errors."> revert_strings;
	GroupOpt<"Output Options", "value",
		"Debug info components to be included in the produced EVM assembly and Yul code. "
		"Value can be 'all', 'none' or a comma-separated list of debug info components."> debug_info;
	GroupOpt<"Output Options", "stage",
		"Stop execution after the given compiler stage. Valid options: \"parsing\"."> stop_after;
};

struct AlternativeInputArgs
{
	GroupFlag<"Alternative Input Modes",
		"Switch to Standard JSON input / output mode, ignoring all options. "
		"It reads from standard input, if no input file was given, otherwise it reads from the provided input file. "
		"The result will be written to standard output."> standard_json = false;
	GroupFlag<"Alternative Input Modes",
		"Switch to linker mode, ignoring all options apart from --libraries and modify binaries in place."> link = false;
	GroupFlag<"Alternative Input Modes",
		"Switch to strict assembly mode and assume input is strict assembly."> strict_assembly = false;
	GroupFlag<"Alternative Input Modes",
		"(experimental) Resume compilation from an abstract syntax tree (AST) representing already parsed and analyzed Solidity code."> import_ast = false;
	GroupFlag<"Alternative Input Modes",
		"(experimental) Resume compilation from EVM assembly in JSON format."> import_asm_json = false;
};

struct AssemblyModeArgs
{
	GroupOpt<"Assembly Mode Options", "evm", "Target machine in assembly or Yul mode."> machine;
	GroupOpt<"Assembly Mode Options", "evm", "Input dialect to use in assembly or yul mode."> yul_dialect;
};

struct LinkerModeArgs
{
	GroupOpt<"Linker Mode Options", "libs",
		"Direct string or file containing library addresses. Syntax: "
		"<libraryName>=<address> [, or whitespace] ...\n"
		"Address is interpreted as a hex string prefixed by 0x.",
		OptStrVec> libraries;
};

struct OutputFormattingArgs
{
	GroupFlag<"Output Formatting", "Output JSON in pretty format."> pretty_json = false;
	GroupOpt<"Output Formatting", "N",
		"Indent pretty-printed JSON with N spaces. Enables '--pretty-json' automatically.",
		std::optional<uint32_t>> json_indent;
	GroupFlag<"Output Formatting", "Force colored output."> color = false;
	GroupFlag<"Output Formatting", "Explicitly disable colored output, disabling terminal auto-detection."> no_color = false;
	GroupFlag<"Output Formatting", "Output error codes."> error_codes = false;
};

struct OutputComponentArgs
{
	OutCompFlag<"AST of all source files in a compact JSON format."> ast_compact_json = false;
	rfl::Rename<"asm", OutCompFlag<"EVM assembly of the contracts.">> asm_ = false;
	OutCompFlag<"EVM assembly of the contracts in JSON format."> asm_json = false;
	OutCompFlag<"Opcodes of the contracts."> opcodes = false;
	OutCompFlag<"Binary of the contracts in hex."> bin = false;
	OutCompFlag<"Binary of the runtime part of the contracts in hex."> bin_runtime = false;
	OutCompFlag<"ABI specification of the contracts."> abi = false;
	OutCompFlag<"Intermediate Representation (IR) of all contracts."> ir = false;
	OutCompFlag<"(experimental) AST of Intermediate Representation (IR) of all contracts in a compact JSON format."> ir_ast_json = false;
	OutCompFlag<"Optimized Intermediate Representation (IR) of all contracts."> ir_optimized = false;
	OutCompFlag<"(experimental) AST of optimized Intermediate Representation (IR) of all contracts in a compact JSON format."> ir_optimized_ast_json = false;
	OutCompFlag<"Function signature hashes of the contracts."> hashes = false;
	OutCompFlag<"Natspec user documentation of all contracts."> userdoc = false;
	OutCompFlag<"Natspec developer documentation of all contracts."> devdoc = false;
	OutCompFlag<"Combined Metadata JSON whose IPFS hash is stored on-chain."> metadata = false;
	OutCompFlag<"Slots, offsets and types of the contract's state variables located in storage."> storage_layout = false;
	OutCompFlag<"Slots, offsets and types of the contract's state variables located in transient storage."> transient_storage_layout = false;
	OutCompFlag<"(experimental) Global ethdebug type and pointer tables (ethdebug/format/info/resources schema)."> ethdebug_resources = false;
	OutCompFlag<"(experimental) Compiler information and source list (ethdebug/format/materials/compilation schema)."> ethdebug_compilation = false;
	OutCompFlag<"(experimental) Debug information in ethdebug format for all contracts (ethdebug/format/program schema for creation bytecode)."> ethdebug_program = false;
	OutCompFlag<"(experimental) Debug information in ethdebug format for the runtime part of all contracts (ethdebug/format/program schema for deployed bytecode)."> ethdebug_program_runtime = false;
};

struct ExtraOutputArgs
{
	GroupFlag<"Extra Output", "Print an estimate of the maximal gas usage for each function."> gas = false;
	GroupOpt<"Extra Output", "abi,asm,ast,bin,bin-runtime,devdoc,function-debug,function-debug-runtime,generated-sources,generated-sources-runtime,hashes,metadata,opcodes,srcmap,srcmap-runtime,storage-layout,transient-storage-layout,userdoc",
		"Output a single json document containing the specified information."> combined_json;
};

struct MetadataArgs
{
	GroupFlag<"Metadata Options", "Do not append CBOR metadata to the end of the bytecode."> no_cbor_metadata = false;
	GroupOpt<"Metadata Options", "ipfs,none,swarm", "Choose hash method for the bytecode metadata or disable it."> metadata_hash;
	GroupFlag<"Metadata Options", "Store referenced sources as literal data in the metadata output."> metadata_literal = false;
};

struct OptimizerArgs
{
	GroupFlag<"Optimizer Options", "Enable optimizer."> optimize = false;
	GroupOpt<"Optimizer Options", "n",
		"The number of runs specifies roughly how often each opcode of the deployed code will be executed across the lifetime of the contract. "
		"Lower values will optimize more for initial deployment cost, higher values will optimize more for high-frequency usage.",
		std::optional<OptimiserSettings::ExecutionCount>> optimize_runs;
	GroupFlag<"Optimizer Options",
		"Enable Yul optimizer (independently of the EVM assembly optimizer). "
		"The general --optimize option automatically enables this unless --no-optimize-yul is specified."> optimize_yul = false;
	GroupFlag<"Optimizer Options", "Disable Yul optimizer (independently of the EVM assembly optimizer)."> no_optimize_yul = false;
	GroupOpt<"Optimizer Options", "steps",
		"Forces Yul optimizer to use the specified sequence of optimization steps instead of the built-in one."> yul_optimizations;
};

struct ModelCheckerArgs
{
	GroupOpt<"Model Checker Options", "default,<source>:<contract>",
		"Select which contracts should be analyzed using the form <source>:<contract>. "
		"Multiple pairs <source>:<contract> can be selected at the same time, separated by a comma and no spaces."> model_checker_contracts;
	GroupFlag<"Model Checker Options",
		"Encode division and modulo operations with their precise operators instead of multiplication with slack variables."> model_checker_div_mod_no_slacks = false;
	GroupOpt<"Model Checker Options", "all,bmc,chc,none", "Select model checker engine."> model_checker_engine;
	GroupOpt<"Model Checker Options", "untrusted,trusted",
		"Select whether to assume (trusted) that external calls always invoke the code given by the type of the contract, if that code is available."> model_checker_ext_calls;
	GroupOpt<"Model Checker Options", "default,all,contract,reentrancy",
		"Select whether to report inferred contract inductive invariants. "
		"Multiple types of invariants can be selected at the same time, separated by a comma and no spaces. "
		"By default no invariants are reported."> model_checker_invariants;
	GroupFlag<"Model Checker Options", "Print the queries created by the SMTChecker in the SMTLIB2 format."> model_checker_print_query = false;
	GroupFlag<"Model Checker Options", "Show all targets that were proved safe separately."> model_checker_show_proved_safe = false;
	GroupFlag<"Model Checker Options", "Show all unproved targets separately."> model_checker_show_unproved = false;
	GroupFlag<"Model Checker Options", "Show all unsupported language features separately."> model_checker_show_unsupported = false;
	GroupOpt<"Model Checker Options", "cvc5,eld,z3,smtlib2", "Select model checker solvers."> model_checker_solvers;
	GroupOpt<"Model Checker Options", "default,all,constantCondition,underflow,overflow,divByZero,balance,assert,popEmptyArray,outOfBounds",
		"Select model checker verification targets. "
		"Multiple targets can be selected at the same time, separated by a comma and no spaces. "
		"By default all targets except underflow and overflow are selected."> model_checker_targets;
	GroupOpt<"Model Checker Options", "ms",
		"Set model checker timeout per query in milliseconds. "
		"The default is a deterministic resource limit. "
		"A timeout of 0 means no resource/time restrictions for any query.",
		std::optional<unsigned>> model_checker_timeout;
	GroupOpt<"Model Checker Options", "value",
		"Set loop unrolling depth for BMC engine. Default is 1.",
		std::optional<unsigned>> model_checker_bmc_loop_iterations;
};

struct ExperimentalArgs
{
	GroupFlag<"Experimental options",
		"Enable experimental mode. "
		"This flag does not activate any experimental features on its own - "
		"it unlocks the ability to use them through the usual flags and pragmas."> experimental = false;
};

struct DisabledArgs
{
	rfl::Hidden<Flag> assemble = false;
	rfl::Hidden<Flag> lsp = false;
	rfl::Hidden<Flag> yul = false;
};

using SolcCli = rfl::Processors<
	rfl::NoExtraFields,
	rfl::cli::SeparateOptionValues,
	rfl::cli::ListDelimiter<"">,
	rfl::cli::PureFlags
>;

} // namespace

struct CommandLineParser::RawArgs
{
	rfl::Flatten<GeneralInfoArgs> general;
	rfl::Flatten<InputArgs> input;
	rfl::Flatten<OutputArgs> output;
	rfl::Flatten<AlternativeInputArgs> alt_input;
	rfl::Flatten<AssemblyModeArgs> assembly;
	rfl::Flatten<LinkerModeArgs> linker;
	rfl::Flatten<OutputFormattingArgs> formatting;
	rfl::Flatten<OutputComponentArgs> outputs;
	rfl::Flatten<ExtraOutputArgs> extra_output;
	rfl::Flatten<MetadataArgs> metadata;
	rfl::Flatten<OptimizerArgs> optimizer;
	rfl::Flatten<ModelCheckerArgs> model_checker;
	rfl::Flatten<ExperimentalArgs> experimental;
	rfl::Flatten<DisabledArgs> disabled;
};

CommandLineParser::CommandLineParser() = default;
CommandLineParser::~CommandLineParser() = default;
CommandLineParser::CommandLineParser(CommandLineParser&&) noexcept = default;
CommandLineParser& CommandLineParser::operator=(CommandLineParser&&) noexcept = default;

void CommandLineParser::printHelp(std::ostream& _out)
{
	rfl::cli::UsageOptions const usageOpts{
		.detailed = true,
		.header =
			"solc, the Solidity commandline compiler.\n\n"
			"This program comes with ABSOLUTELY NO WARRANTY. This is free software, and you\n"
			"are welcome to redistribute it under certain conditions. See 'solc --" + g_strLicense + "'\n"
			"for details.",
		.synopsis = "solc [options] [input_file...]",
		.description =
			"Compiles the given Solidity input files (or the standard input if \"-\" is\n"
			"used as a file name) and outputs the components specified in the options\n"
			"at standard output or in files in the output directory, if specified.\n"
			"Imports are automatically read from the filesystem, but it is also possible to\n"
			"remap paths using the context:prefix=path syntax.\n"
			"Example:\n"
			"solc --" + CompilerOutputs::componentName(&CompilerOutputs::binary) +
			" -o /tmp/solcoutput dapp-bin=/usr/local/lib/dapp-bin contract.sol",
	};
	auto const text = rfl::cli::usage<RawArgs, SolcCli>("solc", usageOpts);
	solAssert(text.has_value(), text.error().what());
	_out << *text;
}

void CommandLineParser::checkMutuallyExclusive(std::vector<std::string> const& _optionNames)
{
	if (countEnabledOptions(_optionNames) > 1)
	{
		solThrow(
			CommandLineValidationError,
			"The following options are mutually exclusive: " + joinOptionNames(_optionNames) + ". " +
			"Select at most one."
		);
	}
}

std::vector<std::string> const& CommandLineParser::experimentalOptionNames()
{
	static std::vector<std::string> const names{
		g_strImportAst,
		g_strImportEvmAssemblerJson,
		"ir-ast-json",
		"ir-optimized-ast-json",
		"ethdebug-resources",
		"ethdebug-compilation",
		"ethdebug-program",
		"ethdebug-program-runtime",
		g_strViaSSACFG,
	};
	return names;
}

void CommandLineParser::checkExperimental(std::vector<std::string> const& _optionNames) const
{
	bool const experimental = rfl::unwrap(m_rawArgs->experimental.value().experimental);
	if (!experimental && countEnabledOptions(_optionNames) > 0)
	{
		solThrow(
			CommandLineValidationError,
			fmt::format(
				"The following options are only available in experimental mode: {}. "
				"To enable experimental mode, use the --{} flag.",
				joinOptionNames(enabledOptions(_optionNames)),
				g_strExperimental
			)
		);
	}

	auto const& evmVersionOpt = rfl::unwrap(m_rawArgs->output.value().evm_version);
	if (!experimental && evmVersionOpt.has_value())
	{
		std::optional<langutil::EVMVersion> versionOption = langutil::EVMVersion::fromString(*evmVersionOpt);

		if (versionOption.has_value() && versionOption->isExperimental())
			solThrow(
				CommandLineValidationError,
				fmt::format(
					"EVM version '{}' is experimental and can only be selected in experimental mode. "
					"To enable experimental mode, use the --{} flag.",
					versionOption->name(),
					g_strExperimental
				)
			);
	}
}

std::ostream& operator<<(std::ostream& _out, CompilerOutputs const& _selection)
{
	std::vector<std::string> serializedSelection;
	for (auto&& [componentName, component]: CompilerOutputs::componentMap())
		if (_selection.*component)
			serializedSelection.push_back(CompilerOutputs::componentName(component));

	return _out << util::joinHumanReadable(serializedSelection, ",");
}

std::string const& CompilerOutputs::componentName(bool CompilerOutputs::* _component)
{
	solAssert(_component, "");

	// NOTE: Linear search is not optimal but it's simpler than getting pointers-to-members to work as map keys.
	for (auto const& [componentName, component]: CompilerOutputs::componentMap())
		if (component == _component)
			return componentName;

	solAssert(false, "");
}

std::ostream& operator<<(std::ostream& _out, CombinedJsonRequests const& _requests)
{
	std::vector<std::string> serializedRequests;
	for (auto&& [componentName, component]: CombinedJsonRequests::componentMap())
		if (_requests.*component)
			serializedRequests.push_back(CombinedJsonRequests::componentName(component));

	return _out << util::joinHumanReadable(serializedRequests, ",");
}

std::string const& CombinedJsonRequests::componentName(bool CombinedJsonRequests::* _component)
{
	solAssert(_component, "");

	for (auto const& [componentName, component]: CombinedJsonRequests::componentMap())
		if (component == _component)
			return componentName;

	solAssert(false, "");
}

OptimiserSettings CommandLineOptions::optimiserSettings() const
{
	OptimiserSettings settings;

	if (optimizer.optimizeEvmasm)
		settings = OptimiserSettings::standard();
	else
		if (input.mode == InputMode::EVMAssemblerJSON)
			settings = OptimiserSettings::none();
		else
			settings = OptimiserSettings::minimal();

	settings.runYulOptimiser = optimizer.optimizeYul;
	if (optimizer.optimizeYul)
		// NOTE: Standard JSON disables optimizeStackAllocation by default when yul optimizer is disabled.
		// --optimize --no-optimize-yul on the CLI does not have that effect.
		settings.optimizeStackAllocation = true;

	if (optimizer.expectedExecutionsPerDeployment.has_value())
		settings.expectedExecutionsPerDeployment = optimizer.expectedExecutionsPerDeployment.value();

	if (optimizer.yulSteps.has_value())
	{
		std::string const fullSequence = optimizer.yulSteps.value();
		auto const delimiterPos = fullSequence.find(":");
		settings.yulOptimiserSteps = fullSequence.substr(0, delimiterPos);

		if (delimiterPos != std::string::npos)
			settings.yulOptimiserCleanupSteps = fullSequence.substr(delimiterPos + 1);
		else
			solAssert(settings.yulOptimiserCleanupSteps == OptimiserSettings::DefaultYulOptimiserCleanupSteps);
	}

	return settings;
}

void CommandLineParser::parse(int _argc, char const* const* _argv)
{
	parseArgs(_argc, _argv);
	processArgs();
}

void CommandLineParser::parseInputPathsAndRemappings()
{
	m_options.input.ignoreMissingFiles = rfl::unwrap(m_rawArgs->input.value().ignore_missing);

	if (auto const& inputFiles = rfl::unwrap(m_rawArgs->general.value().input_file))
		for (std::string const& positionalArg: *inputFiles)
		{
			if (ImportRemapper::isRemapping(positionalArg))
			{
				std::optional<ImportRemapper::Remapping> remapping = ImportRemapper::parseRemapping(positionalArg);
				if (!remapping.has_value())
					solThrow(CommandLineValidationError, "Invalid remapping: \"" + positionalArg + "\".");

				if (m_options.input.mode == InputMode::StandardJson)
					solThrow(
						CommandLineValidationError,
						"Import remappings are not accepted on the command line in Standard JSON mode.\n"
						"Please put them under 'settings.remappings' in the JSON input."
					);

				if (!remapping->target.empty())
				{
					// If the target is a directory, whitelist it. Otherwise whitelist containing dir.
					// NOTE: /a/b/c/ is a directory while /a/b/c is not.
					boost::filesystem::path remappingDir = remapping->target;
					if (remappingDir.filename() != "..")
						// As an exception we'll treat /a/b/c/.. as a directory too. It would be
						// unintuitive to whitelist /a/b/c when the target is equivalent to /a/b/.
						remappingDir.remove_filename();
					m_options.input.allowedDirectories.insert(remappingDir.empty() ? "." : remappingDir);
				}

				m_options.input.remappings.emplace_back(std::move(remapping.value()));
			}
			else if (positionalArg == "-")
				m_options.input.addStdin = true;
			else
				m_options.input.paths.insert(positionalArg);
		}

	if (m_options.input.mode == InputMode::StandardJson)
	{
		if (m_options.input.paths.size() > 1 || (m_options.input.paths.size() == 1 && m_options.input.addStdin))
			solThrow(
				CommandLineValidationError,
				"Too many input files for --" + g_strStandardJSON + ".\n"
				"Please either specify a single file name or provide its content on standard input."
			);
		else if (m_options.input.paths.size() == 0)
			// Standard JSON mode input used to be handled separately and zero files meant "read from stdin".
			// Keep it working that way for backwards-compatibility.
			m_options.input.addStdin = true;
	}
	else if (m_options.input.paths.size() == 0 && !m_options.input.addStdin)
		solThrow(
			CommandLineValidationError,
			"No input files given. If you wish to use the standard input please specify \"-\" explicitly."
		);
}

void CommandLineParser::parseLibraryOption(std::string const& _input)
{
	namespace fs = boost::filesystem;
	std::string data = _input;
	try
	{
		if (fs::is_regular_file(_input))
			data = util::readFileAsString(_input);
	}
	catch (fs::filesystem_error const&)
	{
		// Thrown e.g. if path is too long.
	}
	catch (util::FileNotFound const&)
	{
		// Should not happen if `fs::is_regular_file` is correct.
	}
	catch (util::NotAFile const&)
	{
		// Should not happen if `fs::is_regular_file` is correct.
	}

	std::vector<std::string> libraries;
	boost::split(libraries, data, boost::is_space() || boost::is_any_of(","), boost::token_compress_on);
	for (std::string const& lib: libraries)
		if (!lib.empty())
		{
			//search for equal sign or last colon in string as our binaries output placeholders in the form of file=Name or file:Name
			//so we need to search for `=` or `:` in the string
			auto separator = lib.rfind('=');
			bool isSeparatorEqualSign = true;
			if (separator == std::string::npos)
			{
				separator = lib.rfind(':');
				if (separator == std::string::npos)
					solThrow(
						CommandLineValidationError,
						"Equal sign separator missing in library address specifier \"" + lib + "\""
					);
				else
					isSeparatorEqualSign = false; // separator is colon
			}
			else
				if (lib.rfind('=') != lib.find('='))
					solThrow(
						CommandLineValidationError,
						"Only one equal sign \"=\" is allowed in the address string \"" + lib + "\"."
					);

			std::string libName(lib.begin(), lib.begin() + static_cast<ptrdiff_t>(separator));
			boost::trim(libName);
			if (m_options.linker.libraries.count(libName))
				solThrow(
					CommandLineValidationError,
					"Address specified more than once for library \"" + libName + "\"."
				);

			std::string addrString(lib.begin() + static_cast<ptrdiff_t>(separator) + 1, lib.end());
			boost::trim(addrString);
			if (addrString.empty())
				solThrow(
					CommandLineValidationError,
					"Empty address provided for library \"" + libName + "\".\n"
					"Note that there should not be any whitespace after the " +
					(isSeparatorEqualSign ? "equal sign" : "colon") + "."
				);
			if (!util::isValidHex(addrString))
				solThrow(
					CommandLineValidationError,
					addrString.substr(0, 2) != "0x" ?
					"The address " + addrString + " is not prefixed with \"0x\".\n"
					"Note that the address must be prefixed with \"0x\"." :
					"Invalid hex value for address for library \"" + libName + "\": " + addrString + "."
				);
			addrString = addrString.substr(2);
			if (addrString.length() != 40)
				solThrow(
					CommandLineValidationError,
					"Invalid length for address for library \"" + libName + "\": " +
					std::to_string(addrString.length()) + " instead of 40 characters."
				);
			if (!util::passesAddressChecksum(addrString, false))
				solThrow(
					CommandLineValidationError,
					"Invalid checksum on address for library \"" + libName + "\": " + addrString + "\n"
					"The correct checksum is " + util::getChecksummedAddress(addrString)
				);
			bytes binAddr = util::fromHex(addrString);
			util::h160 address(binAddr, util::h160::AlignRight);
			if (binAddr.size() > 20 || address == util::h160())
				solThrow(
					CommandLineValidationError,
					"Invalid address for library \"" + libName + "\": " + addrString
				);
			m_options.linker.libraries[libName] = address;
		}
}

std::vector<std::string> CommandLineParser::enabledOptions(std::vector<std::string> const& _optionList) const
{
	auto optionEnabled = [&](std::string const& _option) { return m_providedOptions.contains(_option); };
	return _optionList | ranges::views::filter(optionEnabled) | ranges::to_vector;
}

void CommandLineParser::parseOutputSelection()
{
	static auto outputSupported = [](InputMode _mode, std::string_view _outputName)
	{
		static std::set<std::string> const compilerModeOutputs = (
			CompilerOutputs::componentMap() |
			ranges::views::keys |
			ranges::to<std::set>()
		);
		static std::set<std::string> const assemblerModeOutputs = {
			CompilerOutputs::componentName(&CompilerOutputs::asm_),
			CompilerOutputs::componentName(&CompilerOutputs::binary),
			CompilerOutputs::componentName(&CompilerOutputs::irOptimized),
			CompilerOutputs::componentName(&CompilerOutputs::astCompactJson),
			CompilerOutputs::componentName(&CompilerOutputs::asmJson),
			CompilerOutputs::componentName(&CompilerOutputs::ethdebugProgram),
		};
		static std::set<std::string> const evmAssemblyJsonImportModeOutputs = {
			CompilerOutputs::componentName(&CompilerOutputs::asm_),
			CompilerOutputs::componentName(&CompilerOutputs::binary),
			CompilerOutputs::componentName(&CompilerOutputs::binaryRuntime),
			CompilerOutputs::componentName(&CompilerOutputs::opcodes),
			CompilerOutputs::componentName(&CompilerOutputs::asmJson),
		};
		switch (_mode)
		{
		case InputMode::Help:
		case InputMode::License:
		case InputMode::Version:
			solAssert(false);
		case InputMode::Compiler:
		case InputMode::CompilerWithASTImport:
			return util::contains(compilerModeOutputs, _outputName);
		case InputMode::EVMAssemblerJSON:
			return util::contains(evmAssemblyJsonImportModeOutputs, _outputName);
		case InputMode::Assembler:
			return util::contains(assemblerModeOutputs, _outputName);
		case InputMode::StandardJson:
		case InputMode::Linker:
			return false;
		}

		solAssert(false, "");
	};

	for (auto&& [optionName, outputComponent]: CompilerOutputs::componentMap())
		m_options.compiler.outputs.*outputComponent = m_providedOptions.contains(optionName);

	if (m_options.input.mode == InputMode::Assembler && m_options.compiler.outputs == CompilerOutputs{})
	{
		// In assembly mode keep the default outputs enabled for backwards-compatibility.
		// TODO: Remove this (must be done in a breaking release).
		m_options.compiler.outputs.asm_ = true;
		m_options.compiler.outputs.binary = true;
		m_options.compiler.outputs.irOptimized = true;
	}

	std::vector<std::string> unsupportedOutputs;
	for (auto&& [optionName, outputComponent]: CompilerOutputs::componentMap())
		if (m_options.compiler.outputs.*outputComponent && !outputSupported(m_options.input.mode, optionName))
			unsupportedOutputs.push_back(optionName);

	if (!unsupportedOutputs.empty())
		solThrow(
			CommandLineValidationError,
			"The following outputs are not supported in " + g_inputModeName.at(m_options.input.mode) + " mode: " +
			joinOptionNames(unsupportedOutputs) + "."
		);
}

void CommandLineParser::parseArgs(int _argc, char const* const* _argv)
{
	m_options = {};
	m_providedOptions.clear();

	auto parsed = rfl::cli::read_detailed<RawArgs, SolcCli>(_argc, _argv);
	if (!parsed)
		solThrow(CommandLineValidationError, parsed.error().what());

	m_rawArgs = std::make_unique<RawArgs>(std::move(parsed->value));
	m_providedOptions.insert(parsed->provided.begin(), parsed->provided.end());
}

void CommandLineParser::processArgs()
{
	auto const& general = m_rawArgs->general.value();
	auto const& input = m_rawArgs->input.value();
	auto const& output = m_rawArgs->output.value();
	auto const& altInput = m_rawArgs->alt_input.value();
	auto const& assembly = m_rawArgs->assembly.value();
	auto const& linker = m_rawArgs->linker.value();
	auto const& formatting = m_rawArgs->formatting.value();
	auto const& extraOutput = m_rawArgs->extra_output.value();
	auto const& metadata = m_rawArgs->metadata.value();
	auto const& optimizer = m_rawArgs->optimizer.value();
	auto const& modelChecker = m_rawArgs->model_checker.value();
	auto const& disabled = m_rawArgs->disabled.value();

	if (rfl::unwrap(formatting.no_color))
		m_options.formatting.coloredOutput = false;
	else if (rfl::unwrap(formatting.color))
		m_options.formatting.coloredOutput = true;

	if (rfl::unwrap(m_rawArgs->experimental.value().experimental))
		m_options.experimental = true;

	checkMutuallyExclusive({
		g_strHelp,
		g_strLicense,
		g_strVersion,
		g_strStandardJSON,
		g_strLink,
		g_strStrictAssembly,
		g_strImportAst,
		g_strImportEvmAssemblerJson,
	});

	checkExperimental(experimentalOptionNames());

	if (rfl::unwrap(general.help))
		m_options.input.mode = InputMode::Help;
	else if (rfl::unwrap(general.license))
		m_options.input.mode = InputMode::License;
	else if (rfl::unwrap(general.version))
		m_options.input.mode = InputMode::Version;
	else if (rfl::unwrap(altInput.standard_json))
		m_options.input.mode = InputMode::StandardJson;
	else if (rfl::unwrap(altInput.strict_assembly))
		m_options.input.mode = InputMode::Assembler;
	else if (rfl::unwrap(altInput.link))
		m_options.input.mode = InputMode::Linker;
	else if (rfl::unwrap(altInput.import_ast))
		m_options.input.mode = InputMode::CompilerWithASTImport;
	else if (rfl::unwrap(altInput.import_asm_json))
		m_options.input.mode = InputMode::EVMAssemblerJSON;
	else
		m_options.input.mode = InputMode::Compiler;

	if (
		m_options.input.mode == InputMode::Help ||
		m_options.input.mode == InputMode::License ||
		m_options.input.mode == InputMode::Version
	)
		return;

	if (m_options.experimental && m_options.input.mode == InputMode::StandardJson)
		solThrow(
			CommandLineValidationError,
			"Standard JSON input mode is incompatible with the --" + g_strExperimental + " flag. "
			"Instead, please use the 'settings.experimental' setting in your Standard JSON input file to "
			"enable experimental mode."
		);

	if (rfl::unwrap(disabled.yul))
		solThrow(
			CommandLineValidationError,
			"The typed Yul dialect formerly accessible via --yul is no longer supported, "
			"please use --strict-assembly instead."
		);

	if (rfl::unwrap(disabled.assemble))
		solThrow(
			CommandLineValidationError,
			"The assembly input mode formerly accessible via --assemble is no longer supported, "
			"please use --strict-assembly instead."
		);

	if (rfl::unwrap(disabled.lsp))
		solThrow(
			CommandLineValidationError,
			"Language Server Protocol (LSP) support has been removed from solc."
		);

	std::map<std::string, std::set<InputMode>> validOptionInputModeCombinations = {
		// TODO: This should eventually contain all options.
		{g_strExperimentalViaIR, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strViaIR, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strMetadataLiteral, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strNoCBORMetadata, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strMetadataHash, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerContracts, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerDivModNoSlacks, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerEngine, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerExtCalls, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerInvariants, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerPrintQuery, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerShowProvedSafe, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerShowUnproved, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerShowUnsupported, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerSolvers, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerTimeout, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerBMCLoopIterations, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strModelCheckerTargets, {InputMode::Compiler, InputMode::CompilerWithASTImport}},
		{g_strViaSSACFG, {InputMode::Compiler, InputMode::CompilerWithASTImport, InputMode::Assembler}}
	};
	std::vector<std::string> invalidOptionsForCurrentInputMode;
	for (auto const& [optionName, inputModes]: validOptionInputModeCombinations)
		if (m_providedOptions.contains(optionName) && inputModes.count(m_options.input.mode) == 0)
			invalidOptionsForCurrentInputMode.push_back(optionName);

	if (!invalidOptionsForCurrentInputMode.empty())
		solThrow(
			CommandLineValidationError,
			"The following options are not supported in the current input mode: " +
			joinOptionNames(invalidOptionsForCurrentInputMode)
		);

	checkMutuallyExclusive({g_strColor, g_strNoColor});
	checkMutuallyExclusive({g_strStopAfter, g_strGas});

	for (std::string const& option: CompilerOutputs::componentMap() | ranges::views::keys)
		if (option != CompilerOutputs::componentName(&CompilerOutputs::astCompactJson))
			checkMutuallyExclusive({g_strStopAfter, option});

	if (m_options.input.mode == InputMode::EVMAssemblerJSON)
	{
		static std::set<std::string> const supportedByEvmAsmJsonImport{
			g_strImportEvmAssemblerJson,
			CompilerOutputs::componentName(&CompilerOutputs::asm_),
			CompilerOutputs::componentName(&CompilerOutputs::binary),
			CompilerOutputs::componentName(&CompilerOutputs::binaryRuntime),
			CompilerOutputs::componentName(&CompilerOutputs::asmJson),
			CompilerOutputs::componentName(&CompilerOutputs::opcodes),
			g_strCombinedJson,
			g_strInputFile,
			g_strJsonIndent,
			g_strOptimize,
			g_strPrettyJson,
			"srcmap",
			"srcmap-runtime",
			g_strExperimental,
		};

		for (auto const& optionName: m_providedOptions)
			if (!supportedByEvmAsmJsonImport.count(optionName))
				solThrow(
					CommandLineValidationError,
					fmt::format(
						"Option --{} is not supported with --{}.",
						optionName,
						g_strImportEvmAssemblerJson
					)
				);
	}

	if (
		m_options.input.mode != InputMode::Compiler &&
		m_options.input.mode != InputMode::CompilerWithASTImport &&
		m_options.input.mode != InputMode::EVMAssemblerJSON &&
		m_options.input.mode != InputMode::Assembler
	)
	{
		if (rfl::unwrap(optimizer.optimize_runs).has_value())
			solThrow(
				CommandLineValidationError,
				"Option --" + g_strOptimizeRuns + " is only valid in compiler and assembler modes."
			);

		for (std::string const& option: {g_strOptimize, g_strNoOptimizeYul, g_strOptimizeYul, g_strYulOptimizations})
			if (m_providedOptions.contains(option))
				solThrow(
					CommandLineValidationError,
					"Option --" + option + " is only valid in compiler and assembler modes."
				);

		if (rfl::unwrap(output.debug_info).has_value())
			solThrow(
				CommandLineValidationError,
				"Option --" + g_strDebugInfo + " is only valid in compiler and assembler modes."
			);
	}

	m_options.formatting.withErrorIds = rfl::unwrap(formatting.error_codes);

	if (auto const& revertStringsString = rfl::unwrap(output.revert_strings))
	{
		std::optional<RevertStrings> revertStrings = revertStringsFromString(*revertStringsString);
		if (!revertStrings)
			solThrow(
				CommandLineValidationError,
				"Invalid option for --" + g_strRevertStrings + ": " + *revertStringsString
			);
		if (*revertStrings == RevertStrings::VerboseDebug)
			solThrow(
				CommandLineValidationError,
				"Only \"default\", \"strip\" and \"debug\" are implemented for --" + g_strRevertStrings + " for now."
			);
		m_options.output.revertStrings = *revertStrings;
	}

	if (auto const& optionValue = rfl::unwrap(output.debug_info))
	{
		m_options.output.debugInfoSelection = DebugInfoSelection::fromString(*optionValue);
		if (!m_options.output.debugInfoSelection.has_value())
			solThrow(CommandLineValidationError, "Invalid value for --" + g_strDebugInfo + " option: " + *optionValue);

		if (!m_options.experimental && m_options.output.debugInfoSelection->ethdebug)
			solThrow(
				CommandLineValidationError,
				fmt::format(
					"Ethdebug annotations are experimental and can only be included in --{} in experimental mode. To enable experimental mode, use the --{} flag.",
					g_strDebugInfo,
					g_strExperimental
				)
			);

		if (m_options.output.debugInfoSelection->snippet && !m_options.output.debugInfoSelection->location)
			solThrow(CommandLineValidationError, "To use 'snippet' with --" + g_strDebugInfo + " you must select also 'location'.");
		if (m_options.output.debugInfoSelection->ethdebug && !m_options.output.debugInfoSelection->astID)
			solThrow(CommandLineValidationError, "To use 'ethdebug' with --" + g_strDebugInfo + " you must select also 'ast-id'.");
	}

	parseCombinedJsonOption();

	if (auto const& outputDir = rfl::unwrap(output.output_dir))
		m_options.output.dir = *outputDir;

	m_options.output.overwriteFiles = rfl::unwrap(output.overwrite);

	if (rfl::unwrap(formatting.pretty_json))
		m_options.formatting.json.format = util::JsonFormat::Pretty;

	if (auto const& indent = rfl::unwrap(formatting.json_indent))
	{
		m_options.formatting.json.format = util::JsonFormat::Pretty;
		m_options.formatting.json.indent = *indent;
	}

	parseOutputSelection();

	m_options.compiler.estimateGas = rfl::unwrap(extraOutput.gas);

	if (auto const& basePath = rfl::unwrap(input.base_path))
		m_options.input.basePath = *basePath;

	if (auto const& includePaths = rfl::unwrap(input.include_path))
	{
		if (m_options.input.basePath.empty())
			solThrow(CommandLineValidationError, "--" + g_strIncludePath + " option requires a non-empty base path.");

		for (std::string const& includePath: *includePaths)
		{
			if (includePath.empty())
				solThrow(CommandLineValidationError, "Empty values are not allowed in --" + g_strIncludePath + ".");

			m_options.input.includePaths.push_back(includePath);
		}
	}

	checkMutuallyExclusive({g_strNoImportCallback, g_strAllowPaths});

	if (rfl::unwrap(input.no_import_callback))
		m_options.input.noImportCallback = true;

	if (auto const& allowPaths = rfl::unwrap(input.allow_paths))
	{
		std::vector<std::string> paths;
		for (std::string const& allowedPath: boost::split(paths, *allowPaths, boost::is_any_of(",")))
			if (!allowedPath.empty())
				m_options.input.allowedDirectories.insert(allowedPath);
	}

	if (auto const& stopAfter = rfl::unwrap(output.stop_after))
	{
		if (*stopAfter != "parsing")
			solThrow(CommandLineValidationError, "Valid options for --" + g_strStopAfter + " are: \"parsing\".\n");
		else
			m_options.output.stopAfter = CompilerStack::State::Parsed;
	}

	parseInputPathsAndRemappings();

	if (m_options.input.mode == InputMode::StandardJson)
		return;

	if (auto const& libraries = rfl::unwrap(linker.libraries))
		for (std::string const& library: *libraries)
			parseLibraryOption(library);

	if (m_options.input.mode == InputMode::Linker)
		return;

	if (auto const& versionOptionStr = rfl::unwrap(output.evm_version))
	{
		std::optional<langutil::EVMVersion> versionOption = langutil::EVMVersion::fromString(*versionOptionStr);
		if (!versionOption)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strEVMVersion + ": " + *versionOptionStr);
		m_options.output.evmVersion = *versionOption;
	}

	bool const optEvmasm = rfl::unwrap(optimizer.optimize);
	bool const optYul = rfl::unwrap(optimizer.optimize_yul);
	bool const noOptYul = rfl::unwrap(optimizer.no_optimize_yul);
	if (noOptYul && optYul)
		solThrow(
			CommandLineValidationError,
			"Options --" + g_strOptimizeYul + " and --" + g_strNoOptimizeYul + " cannot be used together."
		);

	m_options.optimizer.optimizeEvmasm = optEvmasm;
	m_options.optimizer.optimizeYul = ((optEvmasm && !noOptYul) || optYul);
	if (auto const& runs = rfl::unwrap(optimizer.optimize_runs))
		m_options.optimizer.expectedExecutionsPerDeployment = *runs;

	if (auto const& yulSteps = rfl::unwrap(optimizer.yul_optimizations))
	{
		OptimiserSettings optimiserSettings = m_options.optimiserSettings();
		if (
			!optimiserSettings.runYulOptimiser &&
			!OptimiserSuite::isEmptyOptimizerSequence(*yulSteps)
		)
			solThrow(
				CommandLineValidationError,
				"--" + g_strYulOptimizations + " is invalid with a non-empty sequence if Yul optimizer is disabled."
				" Note that the empty optimizer sequence is properly denoted by \":\"."
			);

		try
		{
			yul::OptimiserSuite::validateSequence(*yulSteps);
		}
		catch (yul::OptimizerException const& _exception)
		{
			solThrow(
				CommandLineValidationError,
				"Invalid optimizer step sequence in --" + g_strYulOptimizations + ": " + _exception.what()
			);
		}

		m_options.optimizer.yulSteps = *yulSteps;
	}

	if (m_options.input.mode == InputMode::Assembler)
	{
		std::vector<std::string> const nonAssemblyModeOptions = {
			// TODO: The list is not complete. Add more.
			g_strOutputDir,
			g_strGas,
			g_strCombinedJson,
		};
		if (countEnabledOptions(nonAssemblyModeOptions) >= 1)
		{
			auto enabled = enabledOptions(nonAssemblyModeOptions);
			std::string message = "The following options are invalid in assembly mode: " + joinOptionNames(enabled) + ".";
			solThrow(CommandLineValidationError, message);
		}

		// switch to assembly mode
		using Machine = yul::YulStack::Machine;

		if (auto const& machine = rfl::unwrap(assembly.machine))
		{
			if (*machine == g_strEVM)
				m_options.assembly.targetMachine = Machine::EVM;
			else
				solThrow(CommandLineValidationError, "Invalid option for --" + g_strMachine + ": " + *machine);
		}
		if (auto const& dialect = rfl::unwrap(assembly.yul_dialect))
			if (*dialect != g_strEVM)
				solThrow(CommandLineValidationError, "Invalid option for --" + g_strYulDialect + ": " + *dialect);

		m_options.output.viaSSACFG = rfl::unwrap(output.via_ssa_cfg);

		// Selecting an ethdebug output does not modify the debug info selection,
		// just as --ir-optimized does not imply --optimize: the selection must
		// contain ethdebug explicitly.
		bool const ethdebugSelected =
			m_options.output.debugInfoSelection.has_value() && m_options.output.debugInfoSelection->ethdebug;
		bool const ethdebugProgramRequested =
			m_options.compiler.outputs.ethdebugProgram || m_options.compiler.outputs.ethdebugProgramRuntime;
		if ((ethdebugSelected || ethdebugProgramRequested) && m_options.output.viaSSACFG)
			solUnimplemented("ethdebug is not yet supported with --" + g_strViaSSACFG + ".");
		if (ethdebugSelected && m_options.optimiserSettings().runYulOptimiser)
			solUnimplemented(
				"Optimization (using --" + g_strOptimize + ") is not yet supported with ethdebug."
			);
		return;
	}
	else if (countEnabledOptions({g_strYulDialect, g_strMachine}) >= 1)
		solThrow(
			CommandLineValidationError,
			"--" + g_strYulDialect + " and --" + g_strMachine + " are only valid in assembly mode."
		);

	if (auto const& hashStr = rfl::unwrap(metadata.metadata_hash))
	{
		if (*hashStr == g_strIPFS)
			m_options.metadata.hash = CompilerStack::MetadataHash::IPFS;
		else if (*hashStr == g_strSwarm)
			m_options.metadata.hash = CompilerStack::MetadataHash::Bzzr1;
		else if (*hashStr == g_strNone)
			m_options.metadata.hash = CompilerStack::MetadataHash::None;
		else
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strMetadataHash + ": " + *hashStr);
	}

	if (rfl::unwrap(metadata.no_cbor_metadata))
	{
		if (
			rfl::unwrap(metadata.metadata_hash).has_value() &&
			m_options.metadata.hash != CompilerStack::MetadataHash::None
		)
			solThrow(
				CommandLineValidationError,
				"Cannot specify a metadata hashing method when --" +
				g_strNoCBORMetadata + " is set."
			);

		m_options.metadata.format = CompilerStack::MetadataFormat::NoMetadata;
	}

	if (auto const& contractsStr = rfl::unwrap(modelChecker.model_checker_contracts))
	{
		std::optional<ModelCheckerContracts> contracts = ModelCheckerContracts::fromString(*contractsStr);
		if (!contracts)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strModelCheckerContracts + ": " + *contractsStr);
		m_options.modelChecker.settings.contracts = std::move(*contracts);
	}

	if (rfl::unwrap(modelChecker.model_checker_div_mod_no_slacks))
		m_options.modelChecker.settings.divModNoSlacks = true;

	if (auto const& engineStr = rfl::unwrap(modelChecker.model_checker_engine))
	{
		std::optional<ModelCheckerEngine> engine = ModelCheckerEngine::fromString(*engineStr);
		if (!engine)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strModelCheckerEngine + ": " + *engineStr);
		m_options.modelChecker.settings.engine = *engine;
	}

	if (auto const& mode = rfl::unwrap(modelChecker.model_checker_ext_calls))
	{
		std::optional<ModelCheckerExtCalls> extCallsMode = ModelCheckerExtCalls::fromString(*mode);
		if (!extCallsMode)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strModelCheckerExtCalls + ": " + *mode);
		m_options.modelChecker.settings.externalCalls = *extCallsMode;
	}

	if (auto const& invsStr = rfl::unwrap(modelChecker.model_checker_invariants))
	{
		std::optional<ModelCheckerInvariants> invs = ModelCheckerInvariants::fromString(*invsStr);
		if (!invs)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strModelCheckerInvariants + ": " + *invsStr);
		m_options.modelChecker.settings.invariants = *invs;
	}

	if (rfl::unwrap(modelChecker.model_checker_show_proved_safe))
		m_options.modelChecker.settings.showProvedSafe = true;

	if (rfl::unwrap(modelChecker.model_checker_show_unproved))
		m_options.modelChecker.settings.showUnproved = true;

	if (rfl::unwrap(modelChecker.model_checker_show_unsupported))
		m_options.modelChecker.settings.showUnsupported = true;

	if (auto const& solversStr = rfl::unwrap(modelChecker.model_checker_solvers))
	{
		std::optional<smtutil::SMTSolverChoice> solvers = smtutil::SMTSolverChoice::fromString(*solversStr);
		if (!solvers)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strModelCheckerSolvers + ": " + *solversStr);
		m_options.modelChecker.settings.solvers = *solvers;
	}

	if (rfl::unwrap(modelChecker.model_checker_print_query))
		m_options.modelChecker.settings.printQuery = true;

	if (auto const& targetsStr = rfl::unwrap(modelChecker.model_checker_targets))
	{
		std::optional<ModelCheckerTargets> targets = ModelCheckerTargets::fromString(*targetsStr);
		if (!targets)
			solThrow(CommandLineValidationError, "Invalid option for --" + g_strModelCheckerTargets + ": " + *targetsStr);
		m_options.modelChecker.settings.targets = *targets;
	}

	if (auto const& timeout = rfl::unwrap(modelChecker.model_checker_timeout))
		m_options.modelChecker.settings.timeout = *timeout;

	if (auto const& loopIterations = rfl::unwrap(modelChecker.model_checker_bmc_loop_iterations))
	{
		if (!m_options.modelChecker.settings.engine.bmc)
			solThrow(CommandLineValidationError, "BMC loop unrolling requires the BMC engine to be enabled");
		m_options.modelChecker.settings.bmcLoopIterations = *loopIterations;
	}

	m_options.metadata.literalSources = rfl::unwrap(metadata.metadata_literal);
	// In compiler / AST import / EVM assembler JSON modes, modelChecker defaults are always active.
	m_options.modelChecker.initialize = true;
	m_options.output.viaIR = (rfl::unwrap(output.experimental_via_ir) || rfl::unwrap(output.via_ir));
	m_options.output.viaSSACFG = rfl::unwrap(output.via_ssa_cfg);
	if (m_options.output.viaSSACFG)
		m_options.output.viaIR = true;

	solAssert(
		m_options.input.mode == InputMode::Compiler ||
		m_options.input.mode == InputMode::CompilerWithASTImport ||
		m_options.input.mode == InputMode::EVMAssemblerJSON
	);

	bool ethdebugProgramRequested = m_options.compiler.outputs.ethdebugProgram || m_options.compiler.outputs.ethdebugProgramRuntime;

	std::string const enableEthdebugProgramMessage = fmt::format(
		"--{} / --{}",
		CompilerOutputs::componentName(&CompilerOutputs::ethdebugProgram),
		CompilerOutputs::componentName(&CompilerOutputs::ethdebugProgramRuntime)
	);

	if (ethdebugProgramRequested && !m_options.output.viaIR)
		solThrow(
			CommandLineValidationError,
			enableEthdebugProgramMessage + " output can only be selected, if --via-ir was specified."
		);

	if (
		m_options.output.debugInfoSelection.has_value() && m_options.output.debugInfoSelection->ethdebug &&
		m_options.input.mode != InputMode::Compiler
	)
		solThrow(
			CommandLineValidationError,
			"Invalid input mode for --debug-info ethdebug."
		);

	bool const ethdebugSelected =
		m_options.output.debugInfoSelection.has_value() && m_options.output.debugInfoSelection->ethdebug;
	// The SSA CFG code generator attaches no source locations, which the
	// program output consists of, so the output is gated as well.
	if ((ethdebugSelected || ethdebugProgramRequested) && m_options.output.viaSSACFG)
		solUnimplemented("ethdebug is not yet supported with --" + g_strViaSSACFG + ".");
	if (ethdebugSelected && (m_options.optimizer.optimizeYul || m_options.optimizer.optimizeEvmasm))
		solUnimplemented(
			"Optimization (using --" + g_strOptimize + ") is not yet supported with ethdebug."
		);
}

void CommandLineParser::parseCombinedJsonOption()
{
	auto const& combinedJson = rfl::unwrap(m_rawArgs->extra_output.value().combined_json);
	if (!combinedJson.has_value())
		return;

	std::set<std::string> requests;
	for (std::string const& item: boost::split(requests, *combinedJson, boost::is_any_of(",")))
		if (CombinedJsonRequests::componentMap().count(item) == 0)
			solThrow(CommandLineValidationError, "Invalid option to --" + g_strCombinedJson + ": " + item);

	m_options.compiler.combinedJsonRequests = CombinedJsonRequests{};
	for (auto&& [componentName, component]: CombinedJsonRequests::componentMap())
		m_options.compiler.combinedJsonRequests.value().*component = (requests.count(componentName) > 0);

	if (m_options.input.mode == InputMode::EVMAssemblerJSON && m_options.compiler.combinedJsonRequests.has_value())
	{
		static bool CombinedJsonRequests::* invalidOptions[]{
			&CombinedJsonRequests::abi,
			&CombinedJsonRequests::ast,
			&CombinedJsonRequests::funDebug,
			&CombinedJsonRequests::funDebugRuntime,
			&CombinedJsonRequests::generatedSources,
			&CombinedJsonRequests::generatedSourcesRuntime,
			&CombinedJsonRequests::metadata,
			&CombinedJsonRequests::natspecDev,
			&CombinedJsonRequests::natspecUser,
			&CombinedJsonRequests::signatureHashes,
			&CombinedJsonRequests::storageLayout,
			&CombinedJsonRequests::transientStorageLayout
		};

		for (auto const invalidOption: invalidOptions)
			if (m_options.compiler.combinedJsonRequests.value().*invalidOption)
				solThrow(
					CommandLineValidationError,
					fmt::format(
						"The --{} {} output is not available in EVM assembly import mode.",
						g_strCombinedJson,
						CombinedJsonRequests::componentName(invalidOption)
					)
				);
	}
}

size_t CommandLineParser::countEnabledOptions(std::vector<std::string> const& _optionNames) const
{
	size_t count = 0;
	for (std::string const& _option: _optionNames)
		count += m_providedOptions.count(_option);

	return count;
}

std::string CommandLineParser::joinOptionNames(std::vector<std::string> const& _optionNames, std::string _separator)
{
	return util::joinHumanReadable(
		_optionNames | ranges::views::transform([](std::string const& _option){ return "--" + _option; }),
		_separator
	);
}

} // namespace solidity::frontend
