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

#include <tools/yulPhaser/Population.h>

#include <libsolutil/CommonData.h>

#include <algorithm>
#include <cassert>
#include <numeric>

using namespace std;
using namespace solidity;
using namespace solidity::langutil;
using namespace solidity::phaser;

namespace solidity::phaser
{

ostream& operator<<(ostream& _stream, Individual const& _individual);
ostream& operator<<(ostream& _stream, Population const& _population);

}

ostream& phaser::operator<<(ostream& _stream, Individual const& _individual)
{
	_stream << "Fitness: " << _individual.fitness;
	_stream << ", optimisations: " << _individual.chromosome;

	return _stream;
}

Population Population::makeRandom(shared_ptr<FitnessMetric const> _fitnessMetric, size_t _size)
{
	vector<Chromosome> chromosomes;
	for (size_t i = 0; i < _size; ++i)
		chromosomes.push_back(Chromosome::makeRandom(randomChromosomeLength()));

	return Population(move(_fitnessMetric), move(chromosomes));
}

void Population::run(optional<size_t> _numRounds, ostream& _outputStream)
{
	for (size_t round = 0; !_numRounds.has_value() || round < _numRounds.value(); ++round)
	{
		doMutation();
		doSelection();

		_outputStream << "---------- ROUND " << round << " ----------" << endl;
		_outputStream << *this;
	}
}

Population Population::select(Selection const& _selection) const
{
	vector<Individual> selectedIndividuals;
	for (size_t i: _selection.materialize(m_individuals.size()))
		selectedIndividuals.emplace_back(m_individuals[i]);

	return Population(m_fitnessMetric, selectedIndividuals);
}

Population Population::mutate(Selection const& _selection, function<Mutation> _mutation) const
{
	vector<Individual> mutatedIndividuals;
	for (size_t i: _selection.materialize(m_individuals.size()))
		mutatedIndividuals.emplace_back(_mutation(m_individuals[i].chromosome), *m_fitnessMetric);

	return Population(m_fitnessMetric, mutatedIndividuals);
}

Population Population::crossover(PairSelection const& _selection, function<Crossover> _crossover) const
{
	vector<Individual> crossedIndividuals;
	for (auto const& [i, j]: _selection.materialize(m_individuals.size()))
	{
		auto [childChromosome1, childChromosome2] = _crossover(
			m_individuals[i].chromosome,
			m_individuals[j].chromosome
		);
		crossedIndividuals.emplace_back(move(childChromosome1), *m_fitnessMetric);
		crossedIndividuals.emplace_back(move(childChromosome2), *m_fitnessMetric);
	}

	return Population(m_fitnessMetric, crossedIndividuals);
}

Population operator+(Population _a, Population _b)
{
	// This operator is meant to be used only with populations sharing the same metric (and, to make
	// things simple, "the same" here means the same exact object in memory).
	assert(_a.m_fitnessMetric == _b.m_fitnessMetric);

	return Population(_a.m_fitnessMetric, move(_a.m_individuals) + move(_b.m_individuals));
}

ostream& phaser::operator<<(ostream& _stream, Population const& _population)
{
	auto individual = _population.m_individuals.begin();
	for (; individual != _population.m_individuals.end(); ++individual)
		_stream << *individual << endl;

	return _stream;
}

void Population::doMutation()
{
	// TODO: Implement mutation and crossover
}

void Population::doSelection()
{
	randomizeWorstChromosomes(*m_fitnessMetric, m_individuals, m_individuals.size() / 2);
	m_individuals = sortIndividuals(move(m_individuals));
}

void Population::randomizeWorstChromosomes(
	FitnessMetric const& _fitnessMetric,
	vector<Individual>& _individuals,
	size_t _count
)
{
	assert(_individuals.size() >= _count);
	// ASSUMPTION: _individuals is sorted in ascending order

	auto individual = _individuals.begin() + (_individuals.size() - _count);
	for (; individual != _individuals.end(); ++individual)
	{
		auto chromosome = Chromosome::makeRandom(randomChromosomeLength());
		size_t fitness = _fitnessMetric.evaluate(chromosome);
		*individual = {move(chromosome), fitness};
	}
}

vector<Individual> Population::chromosomesToIndividuals(
	FitnessMetric const& _fitnessMetric,
	vector<Chromosome> _chromosomes
)
{
	vector<Individual> individuals;
	for (auto& chromosome: _chromosomes)
		individuals.emplace_back(move(chromosome), _fitnessMetric);

	return individuals;
}

vector<Individual> Population::sortIndividuals(vector<Individual> _individuals)
{
	sort(
		_individuals.begin(),
		_individuals.end(),
		[](auto const& a, auto const& b){ return a.fitness < b.fitness; }
	);

	return _individuals;
}