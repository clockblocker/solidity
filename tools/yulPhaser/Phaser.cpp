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

#include <tools/yulPhaser/Phaser.h>

#include <tools/yulPhaser/AlgorithmRunner.h>
#include <tools/yulPhaser/Common.h>
#include <tools/yulPhaser/Exceptions.h>
#include <tools/yulPhaser/FitnessMetrics.h>
#include <tools/yulPhaser/GeneticAlgorithms.h>
#include <tools/yulPhaser/Program.h>
#include <tools/yulPhaser/SimulationRNG.h>

#include <liblangutil/CharStream.h>
#include <liblangutil/CharStreamProvider.h>
#include <liblangutil/SourceReferenceFormatter.h>
#include <liblangutil/Scanner.h>

#include <libsolutil/Assertions.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/CommonIO.h>

#include <rfl.hpp>
#include <rfl/cli.hpp>

#include <iostream>

using namespace solidity;
using namespace solidity::langutil;
using namespace solidity::util;
using namespace solidity::yul;
using namespace solidity::phaser;

namespace
{

std::map<PhaserMode, std::string> const PhaserModeToStringMap =
{
	{PhaserMode::RunAlgorithm, "run-algorithm"},
	{PhaserMode::PrintOptimisedPrograms, "print-optimised-programs"},
	{PhaserMode::PrintOptimisedASTs, "print-optimised-asts"},
};
std::map<std::string, PhaserMode> const StringToPhaserModeMap = invertMap(PhaserModeToStringMap);

std::map<Algorithm, std::string> const AlgorithmToStringMap =
{
	{Algorithm::Random, "random"},
	{Algorithm::GEWEP, "GEWEP"},
	{Algorithm::Classic, "classic"},
};
std::map<std::string, Algorithm> const StringToAlgorithmMap = invertMap(AlgorithmToStringMap);

std::map<MetricChoice, std::string> MetricChoiceToStringMap =
{
	{MetricChoice::CodeSize, "code-size"},
	{MetricChoice::RelativeCodeSize, "relative-code-size"},
};
std::map<std::string, MetricChoice> const StringToMetricChoiceMap = invertMap(MetricChoiceToStringMap);

std::map<MetricAggregatorChoice, std::string> const MetricAggregatorChoiceToStringMap =
{
	{MetricAggregatorChoice::Average, "average"},
	{MetricAggregatorChoice::Sum, "sum"},
	{MetricAggregatorChoice::Maximum, "maximum"},
	{MetricAggregatorChoice::Minimum, "minimum"},
};
std::map<std::string, MetricAggregatorChoice> const StringToMetricAggregatorChoiceMap = invertMap(MetricAggregatorChoiceToStringMap);

std::map<CrossoverChoice, std::string> const CrossoverChoiceToStringMap =
{
	{CrossoverChoice::SinglePoint, "single-point"},
	{CrossoverChoice::TwoPoint, "two-point"},
	{CrossoverChoice::Uniform, "uniform"},
};
std::map<std::string, CrossoverChoice> const StringToCrossoverChoiceMap = invertMap(CrossoverChoiceToStringMap);

template <class Enum>
Enum parseChoice(std::string const& _value, std::map<std::string, Enum> const& _map, char const* _optName)
{
	auto const it = _map.find(_value);
	assertThrow(it != _map.end(), BadInput, std::string("Invalid value '") + _value + "' for --" + _optName + ".");
	return it->second;
}

template <rfl::internal::StringLiteral G, rfl::internal::StringLiteral VN, rfl::internal::StringLiteral Desc, class T>
using PhaserOpt = rfl::Group<G, rfl::ValueName<VN, rfl::Description<Desc, T>>>;

template <rfl::internal::StringLiteral G, rfl::internal::StringLiteral Desc>
using PhaserFlag = rfl::Group<G, rfl::Description<Desc, rfl::DefaultVal<bool>>>;

template <rfl::internal::StringLiteral VN>
using WeightOpt = rfl::Group<"METRIC WEIGHTS", rfl::ValueName<VN, rfl::DefaultVal<size_t>>>;

struct GeneralArgs
{
	rfl::Positional<PhaserOpt<"GENERAL", "<PATH>", "Input files.", std::optional<std::vector<std::string>>>> input_files;
	PhaserOpt<"GENERAL", "<CHROMOSOME>",
		"Initial optimisation steps automatically applied to every input program.\n"
		"The result is treated as if it was the actual input, i.e. the steps are not considered "
		"a part of the chromosomes and cannot be mutated. The values of relative metric values "
		"are also relative to the fitness of a program with these steps applied rather than the "
		"fitness of the original program.\n"
		"Note that phaser always adds a 'hgo' prefix to ensure that chromosomes can "
		"contain arbitrary optimisation steps. This implicit prefix cannot be changed or "
		"removed using this option. The value given here is applied after it.",
		rfl::DefaultVal<std::string>> prefix = "";
	PhaserOpt<"GENERAL", "<NUM>", "Seed for the random number generator.", std::optional<uint32_t>> seed;
	PhaserOpt<"GENERAL", "<NUM>", "The number of rounds after which the algorithm should stop. (default=no limit).", std::optional<size_t>> rounds;
	PhaserOpt<"GENERAL", "<NAME>",
		"Mode of operation (run-algorithm, print-optimised-programs, print-optimised-asts).",
		rfl::DefaultVal<std::string>> mode = "run-algorithm";
};

struct AlgorithmArgs
{
	PhaserOpt<"ALGORITHM", "<NAME>", "Algorithm (GEWEP, classic, random).", rfl::DefaultVal<std::string>> algorithm = "GEWEP";
	PhaserFlag<"ALGORITHM",
		"By default, after each round of the algorithm duplicate chromosomes are removed from "
		"the population and replaced with randomly generated ones. "
		"This option disables this postprocessing."> no_randomise_duplicates = false;
	PhaserOpt<"ALGORITHM", "<NUM>", "Minimum length of randomly generated chromosomes.", rfl::DefaultVal<size_t>> min_chromosome_length = 100;
	PhaserOpt<"ALGORITHM", "<NUM>", "Maximum length of randomly generated chromosomes.", rfl::DefaultVal<size_t>> max_chromosome_length = 100;
	PhaserOpt<"ALGORITHM", "<NAME>", "Type of the crossover operator to use (single-point, two-point, uniform).", rfl::DefaultVal<std::string>> crossover = "uniform";
	PhaserOpt<"ALGORITHM", "<PROBABILITY>", "Chance of two genes being swapped between chromosomes in uniform crossover.", rfl::DefaultVal<double>> uniform_crossover_swap_chance = 0.5;
};

struct GewepArgs
{
	PhaserOpt<"GEWEP ALGORITHM", "<FRACTION>", "Percentage of population to regenerate using mutations in each round.", rfl::DefaultVal<double>> gewep_mutation_pool_size = 0.25;
	PhaserOpt<"GEWEP ALGORITHM", "<FRACTION>", "Percentage of population to regenerate using crossover in each round.", rfl::DefaultVal<double>> gewep_crossover_pool_size = 0.25;
	PhaserOpt<"GEWEP ALGORITHM", "<PROBABILITY>", "The chance of choosing gene randomisation as the mutation to perform.", rfl::DefaultVal<double>> gewep_randomisation_chance = 0.9;
	PhaserOpt<"GEWEP ALGORITHM", "<PROBABILITY>", "The chance of choosing gene deletion as the mutation if randomisation was not chosen.", rfl::DefaultVal<double>> gewep_deletion_vs_addition_chance = 0.5;
	PhaserOpt<"GEWEP ALGORITHM", "<PROBABILITY>", "The chance of any given gene being mutated in gene randomisation. (default=1/max-chromosome-length)", std::optional<double>> gewep_genes_to_randomise;
	PhaserOpt<"GEWEP ALGORITHM", "<PROBABILITY>", "The chance of a gene being added (or deleted) in gene addition (or deletion). (default=1/max-chromosome-length)", std::optional<double>> gewep_genes_to_add_or_delete;
};

struct ClassicArgs
{
	PhaserOpt<"CLASSIC GENETIC ALGORITHM", "<FRACTION>", "Percentage of population to regenerate using mutations in each round.", rfl::DefaultVal<double>> classic_elite_pool_size = 0.25;
	PhaserOpt<"CLASSIC GENETIC ALGORITHM", "<FRACTION>", "Chance of a chromosome being selected for crossover.", rfl::DefaultVal<double>> classic_crossover_chance = 0.75;
	PhaserOpt<"CLASSIC GENETIC ALGORITHM", "<FRACTION>", "Chance of a gene being mutated.", rfl::DefaultVal<double>> classic_mutation_chance = 0.01;
	PhaserOpt<"CLASSIC GENETIC ALGORITHM", "<PROBABILITY>", "Chance of a gene being deleted.", rfl::DefaultVal<double>> classic_deletion_chance = 0.01;
	PhaserOpt<"CLASSIC GENETIC ALGORITHM", "<PROBABILITY>", "Chance of a random gene being added.", rfl::DefaultVal<double>> classic_addition_chance = 0.01;
};

struct RandomArgs
{
	PhaserOpt<"RANDOM ALGORITHM", "<FRACTION>",
		"Percentage of the population preserved in each round. (default=one individual, regardless of population size)",
		std::optional<double>> random_elite_pool_size;
};

struct PopulationArgs
{
	rfl::MultiToken<PhaserOpt<"POPULATION", "<CHROMOSOMES>",
		"List of chromosomes to be included in the initial population. "
		"You can specify multiple values separated with spaces or invoke the option multiple times "
		"and all the values will be included.",
		std::optional<std::vector<std::string>>>> population;
	PhaserOpt<"POPULATION", "<SIZE>", "The number of randomly generated chromosomes to be included in the initial population.", std::optional<std::vector<size_t>>> random_population;
	PhaserOpt<"POPULATION", "<FILE>", "A text file with a list of chromosomes (one per line) to be included in the initial population.", std::optional<std::vector<std::string>>> population_from_file;
	PhaserOpt<"POPULATION", "<FILE>", "If specified, the population is saved in the specified file after each round. (default=autosave disabled)", std::optional<std::string>> population_autosave;
};

struct MetricsArgs
{
	PhaserOpt<"METRICS", "<NAME>", "Metric used to evaluate the fitness of a chromosome (code-size, relative-code-size).", rfl::DefaultVal<std::string>> metric = "relative-code-size";
	PhaserOpt<"METRICS", "<NAME>", "Operator used to combine multiple fitness metric values (average, sum, maximum, minimum).", rfl::DefaultVal<std::string>> metric_aggregator = "average";
	PhaserOpt<"METRICS", "<EXPONENT>", "Scaling factor for values produced by relative fitness metrics.", rfl::DefaultVal<size_t>> relative_metric_scale = 3;
	PhaserOpt<"METRICS", "<COUNT>", "Number of times to repeat the sequence optimisation steps represented by a chromosome.", rfl::DefaultVal<size_t>> chromosome_repetitions = 1;
};

struct MetricWeightArgs
{
	WeightOpt<"<COST>"> expression_statement_cost = 1;
	WeightOpt<"<COST>"> assignment_cost = 1;
	WeightOpt<"<COST>"> variable_declaration_cost = 1;
	WeightOpt<"<COST>"> function_definition_cost = 1;
	WeightOpt<"<COST>"> if_cost = 2;
	WeightOpt<"<COST>"> switch_cost = 1;
	WeightOpt<"<COST>"> case_cost = 2;
	WeightOpt<"<COST>"> for_loop_cost = 3;
	WeightOpt<"<COST>"> break_cost = 2;
	WeightOpt<"<COST>"> continue_cost = 2;
	WeightOpt<"<COST>"> leave_cost = 2;
	WeightOpt<"<COST>"> block_cost = 1;
	WeightOpt<"<COST>"> function_call_cost = 1;
	WeightOpt<"<COST>"> identifier_cost = 1;
	WeightOpt<"<COST>"> literal_cost = 1;
};

struct CacheAndOutputArgs
{
	PhaserFlag<"CACHE", "Enables caching of intermediate programs corresponding to chromosome prefixes."> program_cache = false;
	PhaserFlag<"OUTPUT", "Print the state of the population before the algorithm starts."> show_initial_population = false;
	PhaserFlag<"OUTPUT", "Print only the best chromosome found in each round rather than the whole population."> show_only_top_chromosome = false;
	PhaserFlag<"OUTPUT", "Hide information about the current round (round number and elapsed time)."> hide_round = false;
	PhaserFlag<"OUTPUT", "Print information about cache size and effectiveness after each round."> show_cache_stats = false;
	PhaserFlag<"OUTPUT", "Print the selected random seed."> show_seed = false;
};

using PhaserCli = rfl::Processors<
	rfl::cli::App<"", "", "", rfl::cli::BuiltinHelp::long_only>,
	rfl::NoExtraFields,
	rfl::cli::SeparateOptionValues,
	rfl::cli::ListDelimiter<"">,
	rfl::cli::PureFlags
>;

} // namespace

namespace solidity::phaser
{

struct PhaserCommandLineArgs
{
	rfl::Flatten<GeneralArgs> general;
	rfl::Flatten<AlgorithmArgs> algorithm;
	rfl::Flatten<GewepArgs> gewep;
	rfl::Flatten<ClassicArgs> classic;
	rfl::Flatten<RandomArgs> random;
	rfl::Flatten<PopulationArgs> population;
	rfl::Flatten<MetricsArgs> metrics;
	rfl::Flatten<MetricWeightArgs> weights;
	rfl::Flatten<CacheAndOutputArgs> cache_output;
};

} // namespace solidity::phaser

std::istream& phaser::operator>>(std::istream& _inputStream, PhaserMode& _phaserMode) { return deserializeChoice(_inputStream, _phaserMode, StringToPhaserModeMap); }
std::ostream& phaser::operator<<(std::ostream& _outputStream, PhaserMode _phaserMode) { return serializeChoice(_outputStream, _phaserMode, PhaserModeToStringMap); }
std::istream& phaser::operator>>(std::istream& _inputStream, Algorithm& _algorithm) { return deserializeChoice(_inputStream, _algorithm, StringToAlgorithmMap); }
std::ostream& phaser::operator<<(std::ostream& _outputStream, Algorithm _algorithm) { return serializeChoice(_outputStream, _algorithm, AlgorithmToStringMap); }
std::istream& phaser::operator>>(std::istream& _inputStream, MetricChoice& _metric) { return deserializeChoice(_inputStream, _metric, StringToMetricChoiceMap); }
std::ostream& phaser::operator<<(std::ostream& _outputStream, MetricChoice _metric) { return serializeChoice(_outputStream, _metric, MetricChoiceToStringMap); }
std::istream& phaser::operator>>(std::istream& _inputStream, MetricAggregatorChoice& _aggregator) { return deserializeChoice(_inputStream, _aggregator, StringToMetricAggregatorChoiceMap); }
std::ostream& phaser::operator<<(std::ostream& _outputStream, MetricAggregatorChoice _aggregator) { return serializeChoice(_outputStream, _aggregator, MetricAggregatorChoiceToStringMap); }
std::istream& phaser::operator>>(std::istream& _inputStream, CrossoverChoice& _crossover) { return deserializeChoice(_inputStream, _crossover, StringToCrossoverChoiceMap); }
std::ostream& phaser::operator<<(std::ostream& _outputStream, CrossoverChoice _crossover) { return serializeChoice(_outputStream, _crossover, CrossoverChoiceToStringMap); }

GeneticAlgorithmFactory::Options GeneticAlgorithmFactory::Options::fromCommandLine(PhaserCommandLineArgs const& _arguments)
{
	auto const& alg = _arguments.algorithm.value();
	auto const& gewep = _arguments.gewep.value();
	auto const& classic = _arguments.classic.value();
	auto const& random = _arguments.random.value();
	return {
		parseChoice(rfl::unwrap(alg.algorithm), StringToAlgorithmMap, "algorithm"),
		rfl::unwrap(alg.min_chromosome_length),
		rfl::unwrap(alg.max_chromosome_length),
		parseChoice(rfl::unwrap(alg.crossover), StringToCrossoverChoiceMap, "crossover"),
		rfl::unwrap(alg.uniform_crossover_swap_chance),
		rfl::unwrap(random.random_elite_pool_size),
		rfl::unwrap(gewep.gewep_mutation_pool_size),
		rfl::unwrap(gewep.gewep_crossover_pool_size),
		rfl::unwrap(gewep.gewep_randomisation_chance),
		rfl::unwrap(gewep.gewep_deletion_vs_addition_chance),
		rfl::unwrap(gewep.gewep_genes_to_randomise),
		rfl::unwrap(gewep.gewep_genes_to_add_or_delete),
		rfl::unwrap(classic.classic_elite_pool_size),
		rfl::unwrap(classic.classic_crossover_chance),
		rfl::unwrap(classic.classic_mutation_chance),
		rfl::unwrap(classic.classic_deletion_chance),
		rfl::unwrap(classic.classic_addition_chance),
	};
}

std::unique_ptr<GeneticAlgorithm> GeneticAlgorithmFactory::build(
	Options const& _options,
	size_t _populationSize
)
{
	assert(_populationSize > 0);

	switch (_options.algorithm)
	{
		case Algorithm::Random:
		{
			double elitePoolSize = 1.0 / double(_populationSize);

			if (_options.randomElitePoolSize.has_value())
				elitePoolSize = _options.randomElitePoolSize.value();

			return std::make_unique<RandomAlgorithm>(RandomAlgorithm::Options{
				/* elitePoolSize = */ elitePoolSize,
				/* minChromosomeLength = */ _options.minChromosomeLength,
				/* maxChromosomeLength = */ _options.maxChromosomeLength,
			});
		}
		case Algorithm::GEWEP:
		{
			double percentGenesToRandomise = 1.0 / double(_options.maxChromosomeLength);
			double percentGenesToAddOrDelete = percentGenesToRandomise;

			if (_options.gewepGenesToRandomise.has_value())
				percentGenesToRandomise = _options.gewepGenesToRandomise.value();
			if (_options.gewepGenesToAddOrDelete.has_value())
				percentGenesToAddOrDelete = _options.gewepGenesToAddOrDelete.value();

			return std::make_unique<GenerationalElitistWithExclusivePools>(GenerationalElitistWithExclusivePools::Options{
				/* mutationPoolSize = */ _options.gewepMutationPoolSize,
				/* crossoverPoolSize = */ _options.gewepCrossoverPoolSize,
				/* randomisationChance = */ _options.gewepRandomisationChance,
				/* deletionVsAdditionChance = */ _options.gewepDeletionVsAdditionChance,
				/* percentGenesToRandomise = */ percentGenesToRandomise,
				/* percentGenesToAddOrDelete = */ percentGenesToAddOrDelete,
				/* crossover = */ _options.crossover,
				/* uniformCrossoverSwapChance = */ _options.uniformCrossoverSwapChance,
			});
		}
		case Algorithm::Classic:
		{
			return std::make_unique<ClassicGeneticAlgorithm>(ClassicGeneticAlgorithm::Options{
				/* elitePoolSize = */ _options.classicElitePoolSize,
				/* crossoverChance = */ _options.classicCrossoverChance,
				/* mutationChance = */ _options.classicMutationChance,
				/* deletionChance = */ _options.classicDeletionChance,
				/* additionChance = */ _options.classicAdditionChance,
				/* crossover = */ _options.crossover,
				/* uniformCrossoverSwapChance = */ _options.uniformCrossoverSwapChance,
			});
		}
		default:
			assertThrow(false, solidity::util::Exception, "Invalid Algorithm value.");
	}
}

CodeWeights CodeWeightFactory::buildFromCommandLine(PhaserCommandLineArgs const& _arguments)
{
	auto const& w = _arguments.weights.value();
	return {
		rfl::unwrap(w.expression_statement_cost),
		rfl::unwrap(w.assignment_cost),
		rfl::unwrap(w.variable_declaration_cost),
		rfl::unwrap(w.function_definition_cost),
		rfl::unwrap(w.if_cost),
		rfl::unwrap(w.switch_cost),
		rfl::unwrap(w.case_cost),
		rfl::unwrap(w.for_loop_cost),
		rfl::unwrap(w.break_cost),
		rfl::unwrap(w.continue_cost),
		rfl::unwrap(w.leave_cost),
		rfl::unwrap(w.block_cost),
		rfl::unwrap(w.function_call_cost),
		rfl::unwrap(w.identifier_cost),
		rfl::unwrap(w.literal_cost),
	};
}

FitnessMetricFactory::Options FitnessMetricFactory::Options::fromCommandLine(PhaserCommandLineArgs const& _arguments)
{
	auto const& m = _arguments.metrics.value();
	return {
		parseChoice(rfl::unwrap(m.metric), StringToMetricChoiceMap, "metric"),
		parseChoice(rfl::unwrap(m.metric_aggregator), StringToMetricAggregatorChoiceMap, "metric-aggregator"),
		rfl::unwrap(m.relative_metric_scale),
		rfl::unwrap(m.chromosome_repetitions),
	};
}

std::unique_ptr<FitnessMetric> FitnessMetricFactory::build(
	Options const& _options,
	std::vector<Program> _programs,
	std::vector<std::shared_ptr<ProgramCache>> _programCaches,
	CodeWeights const& _weights
)
{
	assert(_programCaches.size() == _programs.size());
	assert(_programs.size() > 0 && "Validations should prevent this from being executed with zero files.");

	std::vector<std::shared_ptr<FitnessMetric>> metrics;
	switch (_options.metric)
	{
		case MetricChoice::CodeSize:
		{
			for (size_t i = 0; i < _programs.size(); ++i)
				metrics.push_back(std::make_unique<ProgramSize>(
					_programCaches[i] != nullptr ? std::optional<Program>{} : std::move(_programs[i]),
					std::move(_programCaches[i]),
					_weights,
					_options.chromosomeRepetitions
				));

			break;
		}
		case MetricChoice::RelativeCodeSize:
		{
			for (size_t i = 0; i < _programs.size(); ++i)
				metrics.push_back(std::make_unique<RelativeProgramSize>(
					_programCaches[i] != nullptr ? std::optional<Program>{} : std::move(_programs[i]),
					std::move(_programCaches[i]),
					_options.relativeMetricScale,
					_weights,
					_options.chromosomeRepetitions
				));
			break;
		}
		default:
			assertThrow(false, solidity::util::Exception, "Invalid MetricChoice value.");
	}

	switch (_options.metricAggregator)
	{
		case MetricAggregatorChoice::Average:
			return std::make_unique<FitnessMetricAverage>(std::move(metrics));
		case MetricAggregatorChoice::Sum:
			return std::make_unique<FitnessMetricSum>(std::move(metrics));
		case MetricAggregatorChoice::Maximum:
			return std::make_unique<FitnessMetricMaximum>(std::move(metrics));
		case MetricAggregatorChoice::Minimum:
			return std::make_unique<FitnessMetricMinimum>(std::move(metrics));
		default:
			assertThrow(false, solidity::util::Exception, "Invalid MetricAggregatorChoice value.");
	}

	// FIXME: Workaround for spurious GCC 12.1 warning (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105794)
	util::unreachable();
}

PopulationFactory::Options PopulationFactory::Options::fromCommandLine(PhaserCommandLineArgs const& _arguments)
{
	auto const& alg = _arguments.algorithm.value();
	auto const& pop = _arguments.population.value();
	return {
		rfl::unwrap(alg.min_chromosome_length),
		rfl::unwrap(alg.max_chromosome_length),
		rfl::unwrap(pop.population).value_or(std::vector<std::string>{}),
		rfl::unwrap(pop.random_population).value_or(std::vector<size_t>{}),
		rfl::unwrap(pop.population_from_file).value_or(std::vector<std::string>{}),
	};
}

Population PopulationFactory::build(
	Options const& _options,
	std::shared_ptr<FitnessMetric> _fitnessMetric
)
{
	Population population = buildFromStrings(_options.population, _fitnessMetric);

	size_t combinedSize = 0;
	for (size_t populationSize: _options.randomPopulation)
		combinedSize += populationSize;

	population = std::move(population) + buildRandom(
		combinedSize,
		_options.minChromosomeLength,
		_options.maxChromosomeLength,
		_fitnessMetric
	);

	for (std::string const& populationFilePath: _options.populationFromFile)
		population = std::move(population) + buildFromFile(populationFilePath, _fitnessMetric);

	return population;
}

Population PopulationFactory::buildFromStrings(
	std::vector<std::string> const& _geneSequences,
	std::shared_ptr<FitnessMetric> _fitnessMetric
)
{
	std::vector<Chromosome> chromosomes;
	for (std::string const& geneSequence: _geneSequences)
		chromosomes.emplace_back(geneSequence);

	return Population(std::move(_fitnessMetric), std::move(chromosomes));
}

Population PopulationFactory::buildRandom(
	size_t _populationSize,
	size_t _minChromosomeLength,
	size_t _maxChromosomeLength,
	std::shared_ptr<FitnessMetric> _fitnessMetric
)
{
	return Population::makeRandom(
		std::move(_fitnessMetric),
		_populationSize,
		_minChromosomeLength,
		_maxChromosomeLength
	);
}

Population PopulationFactory::buildFromFile(
	std::string const& _filePath,
	std::shared_ptr<FitnessMetric> _fitnessMetric
)
{
	return buildFromStrings(readLinesFromFile(_filePath), std::move(_fitnessMetric));
}

ProgramCacheFactory::Options ProgramCacheFactory::Options::fromCommandLine(PhaserCommandLineArgs const& _arguments)
{
	return {
		rfl::unwrap(_arguments.cache_output.value().program_cache),
	};
}

std::vector<std::shared_ptr<ProgramCache>> ProgramCacheFactory::build(
	Options const& _options,
	std::vector<Program> _programs
)
{
	std::vector<std::shared_ptr<ProgramCache>> programCaches;
	for (Program& program: _programs)
		programCaches.push_back(_options.programCacheEnabled ? std::make_shared<ProgramCache>(std::move(program)) : nullptr);

	return programCaches;
}

ProgramFactory::Options ProgramFactory::Options::fromCommandLine(PhaserCommandLineArgs const& _arguments)
{
	auto const& gen = _arguments.general.value();
	return {
		rfl::unwrap(gen.input_files).value_or(std::vector<std::string>{}),
		rfl::unwrap(gen.prefix),
	};
}

std::vector<Program> ProgramFactory::build(Options const& _options)
{
	std::vector<Program> inputPrograms;
	for (auto& path: _options.inputFiles)
	{
		CharStream sourceCode = loadSource(path);
		std::variant<Program, ErrorList> programOrErrors = Program::load(sourceCode);
		if (std::holds_alternative<ErrorList>(programOrErrors))
		{
			SourceReferenceFormatter{std::cerr, SingletonCharStreamProvider(sourceCode), true, false}
				.printErrorInformation(std::get<ErrorList>(programOrErrors));
			std::cerr << std::endl;
			assertThrow(false, InvalidProgram, "Failed to load program " + path);
		}

		std::get<Program>(programOrErrors).optimise(Chromosome(_options.prefix).optimisationSteps());
		inputPrograms.push_back(std::move(std::get<Program>(programOrErrors)));
	}

	return inputPrograms;
}

CharStream ProgramFactory::loadSource(boost::filesystem::path const& _sourcePath)
{
	assertThrow(boost::filesystem::exists(_sourcePath), MissingFile, "Source file does not exist: " + _sourcePath.string());

	std::string sourceCode = readFileAsString(_sourcePath);
	return CharStream(sourceCode, _sourcePath.string());
}

void Phaser::main(int _argc, char** _argv)
{
	auto arguments = parseCommandLine(_argc, _argv);
	if (!arguments)
		return;

	initialiseRNG(*arguments);
	runPhaser(*arguments);
}

std::unique_ptr<PhaserCommandLineArgs> Phaser::parseCommandLine(int _argc, char** _argv)
{
	rfl::cli::UsageOptions const usageOpts{
		.detailed = true,
		.header = "yul-phaser, a tool for finding the best sequence of Yul optimisation phases.",
		.synopsis = "yul-phaser [options] <file>",
		.description =
			"Reads <file> as Yul code and tries to find the best order in which to run optimisation "
			"phases using a genetic algorithm.\n"
			"Example:\n"
			"yul-phaser program.yul",
		.help_group = "GENERAL",
	};

	auto parsed = rfl::cli::read_app<PhaserCommandLineArgs, PhaserCli>(_argc, _argv, usageOpts);
	assertThrow(parsed.has_value(), BadInput, parsed.error().what());

	if (auto const* info = std::get_if<rfl::cli::HelpOrVersion>(&*parsed))
	{
		std::cout << info->text << std::endl;
		return nullptr;
	}

	auto args = std::make_unique<PhaserCommandLineArgs>(std::move(std::get<PhaserCommandLineArgs>(*parsed)));
	auto const& inputFiles = rfl::unwrap(args->general.value().input_files);
	assertThrow(inputFiles.has_value() && !inputFiles->empty(), NoInputFiles, "Missing argument: input-files.");

	return args;
}

void Phaser::initialiseRNG(PhaserCommandLineArgs const& _arguments)
{
	uint32_t seed = rfl::unwrap(_arguments.general.value().seed).value_or(SimulationRNG::generateSeed());
	SimulationRNG::reset(seed);
	if (rfl::unwrap(_arguments.cache_output.value().show_seed))
		std::cout << "Random seed: " << seed << std::endl;
}

AlgorithmRunner::Options Phaser::buildAlgorithmRunnerOptions(PhaserCommandLineArgs const& _arguments)
{
	auto const& gen = _arguments.general.value();
	auto const& alg = _arguments.algorithm.value();
	auto const& pop = _arguments.population.value();
	auto const& out = _arguments.cache_output.value();
	return {
		rfl::unwrap(gen.rounds),
		rfl::unwrap(pop.population_autosave),
		!rfl::unwrap(alg.no_randomise_duplicates),
		rfl::unwrap(alg.min_chromosome_length),
		rfl::unwrap(alg.max_chromosome_length),
		rfl::unwrap(out.show_initial_population),
		rfl::unwrap(out.show_only_top_chromosome),
		!rfl::unwrap(out.hide_round),
		rfl::unwrap(out.show_cache_stats),
	};
}

void Phaser::runPhaser(PhaserCommandLineArgs const& _arguments)
{
	auto programOptions = ProgramFactory::Options::fromCommandLine(_arguments);
	auto cacheOptions = ProgramCacheFactory::Options::fromCommandLine(_arguments);
	auto metricOptions = FitnessMetricFactory::Options::fromCommandLine(_arguments);
	auto populationOptions = PopulationFactory::Options::fromCommandLine(_arguments);

	std::vector<Program> programs = ProgramFactory::build(programOptions);
	std::vector<std::shared_ptr<ProgramCache>> programCaches = ProgramCacheFactory::build(cacheOptions, programs);
	CodeWeights codeWeights = CodeWeightFactory::buildFromCommandLine(_arguments);
	std::unique_ptr<FitnessMetric> fitnessMetric = FitnessMetricFactory::build(
		metricOptions,
		programs,
		programCaches,
		codeWeights
	);
	Population population = PopulationFactory::build(populationOptions, std::move(fitnessMetric));

	PhaserMode const mode = parseChoice(rfl::unwrap(_arguments.general.value().mode), StringToPhaserModeMap, "mode");
	if (mode == PhaserMode::RunAlgorithm)
		runAlgorithm(_arguments, std::move(population), std::move(programCaches));
	else
		printOptimisedProgramsOrASTs(_arguments, population, std::move(programs), mode);
}

void Phaser::runAlgorithm(
	PhaserCommandLineArgs const& _arguments,
	Population _population,
	std::vector<std::shared_ptr<ProgramCache>> _programCaches
)
{
	auto algorithmOptions = GeneticAlgorithmFactory::Options::fromCommandLine(_arguments);

	std::unique_ptr<GeneticAlgorithm> geneticAlgorithm = GeneticAlgorithmFactory::build(
		algorithmOptions,
		_population.individuals().size()
	);

	AlgorithmRunner algorithmRunner(std::move(_population), std::move(_programCaches), buildAlgorithmRunnerOptions(_arguments), std::cout);
	algorithmRunner.run(*geneticAlgorithm);
}

void Phaser::printOptimisedProgramsOrASTs(
	PhaserCommandLineArgs const& _arguments,
	Population const& _population,
	std::vector<Program> _programs,
	PhaserMode phaserMode
)
{
	auto const& paths = *rfl::unwrap(_arguments.general.value().input_files);
	assert(phaserMode == PhaserMode::PrintOptimisedPrograms || phaserMode == PhaserMode::PrintOptimisedASTs);
	assert(_programs.size() == paths.size());

	if (_population.individuals().size() == 0)
	{
		std::cout << "<EMPTY POPULATION>" << std::endl;
		return;
	}

	size_t const repetitions = rfl::unwrap(_arguments.metrics.value().chromosome_repetitions);
	for (auto& individual: _population.individuals())
	{
		std::cout << "Chromosome: " << individual.chromosome << std::endl;

		for (size_t i = 0; i < _programs.size(); ++i)
		{
			for (size_t j = 0; j < repetitions; ++j)
				_programs[i].optimise(individual.chromosome.optimisationSteps());

			std::cout << "Program: " << paths[i] << std::endl;
			if (phaserMode == PhaserMode::PrintOptimisedPrograms)
				std::cout << _programs[i] << std::endl;
			else
				std::cout << _programs[i].toJson() << std::endl;
		}
	}
}
