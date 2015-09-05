/*
 * FermionForce.cpp
 *
 *  Created on: Apr 17, 2012
 *      Author: spiem_01
 */

#include "FermionForce.h"
#include "utils/ConvertLattice.h"

namespace Update {

FermionForce::FermionForce(real_t _kappa) : kappa(_kappa) { }

FermionForce::~FermionForce() { }

FermionicForceMatrix FermionForce::tensor(const GaugeVector& x, const GaugeVector& y) const {
	FermionicForceMatrix result;
	set_to_zero(result);
	for (int i = 0; i < diracVectorLength; ++i) {
		for (int j = 0; j < diracVectorLength; ++j) {
			result.at(i,j) = y[i]*conj(x[j]);
		}
	}
	return result;
}

GaugeGroup FermionForce::force(const environment_t& env, const FermionicForceMatrix& derivative, int site, unsigned int mu) {
#ifdef ADJOINT
	GaugeGroup result;
	set_to_zero(result);
	FermionicGroup link;
	ConvertLattice<GaugeGroup,FermionicGroup>::toAdjoint(env.gaugeLinkConfiguration[site][mu],link);
	//For every generator
	for (unsigned int i = 0; i < fermionLieGenerator.numberGenerators(); ++i) {
		result += -I*imag(trace(derivative*fermionLieGenerator.get(i)*link))*gaugeLieGenerator.get(i);
	}
	return result;
#endif
#ifndef ADJOINT
	GaugeGroup result;
	set_to_zero(result);
	//For every generator
	for (unsigned int i = 0; i < fermionLieGenerator.numberGenerators(); ++i) {
		result += -I*imag(trace(derivative*fermionLieGenerator.get(i)*env.gaugeLinkConfiguration[site][mu]))*gaugeLieGenerator.get(i);
	}
	return result;
#endif
}

} /* namespace Update */
