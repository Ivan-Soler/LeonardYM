/*
 * NFlavorQCDUpdater.cpp
 *
 *  Created on: May 2, 2012
 *      Author: spiem_01
 */

#include "MultiStepNFlavorQCDUpdater.h"
#include "actions/NFlavorQCDAction.h"
#include "algebra_utils/AlgebraUtils.h"
#include "inverters/MultishiftSolver.h"
#include "dirac_functions/RationalApproximation.h"
#include "inverters/BiConjugateGradient.h"
#include "inverters/MultiGridMEMultishiftSolver.h"
#include "dirac_operators/ComplementBlockDiracOperator.h"
#include "dirac_operators/TwistedDiracOperator.h"
#include "dirac_operators/SAPPreconditioner.h"
#include "actions/GaugeAction.h"
#include "hmc_integrators/Integrate.h"
#include "utils/ToString.h"
#include "io/GlobalOutput.h"
#include "dirac_operators/SquareTwistedDiracOperator.h"
#include <iomanip>

//#define DEBUGFORCE
//#define REVERSIBILITY_CHECK

#ifdef DEBUGFORCE
#include "hmc_forces/TestForce.h"
#endif

namespace Update {

MultiStepNFlavorQCDUpdater::MultiStepNFlavorQCDUpdater() : LatticeSweep(), nFlavorQCDAction(0), gaugeAction(0), fermionAction(0), squareDiracOperatorMetropolis(0), diracOperatorMetropolis(0), squareDiracOperatorForce(0), diracOperatorForce(0), multishiftSolver(0), blackBlockDiracOperator(0), redBlockDiracOperator(0) { }

MultiStepNFlavorQCDUpdater::MultiStepNFlavorQCDUpdater(const MultiStepNFlavorQCDUpdater& toCopy) : LatticeSweep(toCopy), nFlavorQCDAction(0), gaugeAction(0), fermionAction(0), squareDiracOperatorMetropolis(0), diracOperatorMetropolis(0), squareDiracOperatorForce(0), diracOperatorForce(0), multishiftSolver(0) { }

MultiStepNFlavorQCDUpdater::~MultiStepNFlavorQCDUpdater() {
	if (nFlavorQCDAction != 0) delete nFlavorQCDAction;
	if (multishiftSolver != 0) delete multishiftSolver;
	if (blackBlockDiracOperator != 0) delete blackBlockDiracOperator;
	if (redBlockDiracOperator != 0) delete redBlockDiracOperator;
}

void MultiStepNFlavorQCDUpdater::initializeApproximations(environment_t& environment) {
	double twist = environment.configurations.get<double>("MultiStepNFlavorQCDUpdater::twist");
	if (isOutputProcess()) std::cout << "MultiStepNFlavorQCDUpdater::Using twist " << twist << std::endl;	

	if (multishiftSolver == 0) {
		if (environment.configurations.get<std::string>("MultiStepNFlavorQCDUpdater::multigrid") == "true") {
			DiracOperator* diracOperator = DiracOperator::getInstance(environment.configurations.get<std::string>("dirac_operator"), 1, environment.configurations);
			diracOperator->setLattice(environment.getFermionLattice());
			diracOperator->setGamma5(false);

			blackBlockDiracOperator = BlockDiracOperator::getInstance(environment.configurations.get<std::string>("dirac_operator"), 1, environment.configurations, Black);
			blackBlockDiracOperator->setLattice(environment.getFermionLattice());
			blackBlockDiracOperator->setGamma5(false);
			blackBlockDiracOperator->setBlockSize(environment.configurations.get< std::vector<unsigned int> >("MultiStepNFlavorQCDUpdater::sap_block_size"));
		
			redBlockDiracOperator = BlockDiracOperator::getInstance(environment.configurations.get<std::string>("dirac_operator"), 1, environment.configurations, Red);
			redBlockDiracOperator->setLattice(environment.getFermionLattice());
			redBlockDiracOperator->setGamma5(false);
			redBlockDiracOperator->setBlockSize(environment.configurations.get< std::vector<unsigned int> >("MultiStepNFlavorQCDUpdater::sap_block_size"));

			MultiGridMEMultishiftSolver* multishiftMultiGridSolver = new MultiGridMEMultishiftSolver(environment.configurations.get< unsigned int >("MultiStepNFlavorQCDUpdater::multigrid_basis_dimension"), environment.configurations.get< std::vector<unsigned int> >("MultiStepNFlavorQCDUpdater::multigrid_block_size"), blackBlockDiracOperator, redBlockDiracOperator);
			multishiftMultiGridSolver->setSAPIterations(environment.configurations.get<unsigned int>("MultiStepNFlavorQCDUpdater::sap_iterations"));
			multishiftMultiGridSolver->setSAPMaxSteps(environment.configurations.get<unsigned int>("MultiStepNFlavorQCDUpdater::sap_inverter_max_steps"));
			multishiftMultiGridSolver->setSAPPrecision(environment.configurations.get<real_t>("MultiStepNFlavorQCDUpdater::sap_inverter_precision"));
			multishiftMultiGridSolver->setGMRESIterations(environment.configurations.get<unsigned int>("MultiStepNFlavorQCDUpdater::gmres_inverter_max_steps"));
			multishiftMultiGridSolver->setGMRESPrecision(environment.configurations.get<real_t>("MultiStepNFlavorQCDUpdater::gmres_inverter_precision"));

			multishiftMultiGridSolver->initializeBasis(diracOperator);			
			multishiftSolver = multishiftMultiGridSolver;
			
			delete diracOperator;
			if (isOutputProcess()) std::cout << "MultiStepNFlavorQCDUpdater::Using multigrid inverter and SAP preconditioning ..." << std::endl;
		}
		else {
			multishiftSolver = MultishiftSolver::getInstance("minimal_residual");
		}
	}
	else {
		if (environment.configurations.get<std::string>("MultiStepNFlavorQCDUpdater::multigrid") == "true") {
			MultiGridMEMultishiftSolver* multishiftMultiGridSolver = dynamic_cast<MultiGridMEMultishiftSolver*>(multishiftSolver);
			multishiftMultiGridSolver->initializeBasis(diracOperatorMetropolis);
		}
	}
	//First take the rational function approximation for the heatbath step
	if (rationalApproximationsHeatBath.empty()) {
		int numberPseudofermions = environment.configurations.get< unsigned int >("number_pseudofermions");
		for (int i = 1; i <= numberPseudofermions; ++i) {
			std::vector<real_t> rat = environment.configurations.get< std::vector<real_t> >(std::string("heatbath_rational_fraction_")+toString(i));
			RationalApproximation rational(multishiftSolver);
			rational.setAlphas(std::vector<real_t>(rat.begin(), rat.begin() + rat.size()/2));
			rational.setBetas(std::vector<real_t>(rat.begin() + rat.size()/2, rat.end()));
			//We apply the twist
			for (unsigned int k = 0; k < rational.getBetas().size(); ++k) {
				rational.getBetas()[k] += twist;
			}
			rational.setPrecision(environment.configurations.get<double>("metropolis_inverter_precision"));
			rational.setMaximumRecursion(environment.configurations.get<unsigned int>("metropolis_inverter_max_steps"));
			rationalApproximationsHeatBath.push_back(rational);
		}
		pseudofermions.resize(numberPseudofermions);
	}

	//Then take the rational function approximation for the metropolis step
	if (rationalApproximationsMetropolis.empty()) {
		int numberPseudofermions = environment.configurations.get< unsigned int >("number_pseudofermions");
		for (int i = 1; i <= numberPseudofermions; ++i) {
			std::vector<real_t> rat = environment.configurations.get< std::vector<real_t> >(std::string("metropolis_rational_fraction_")+toString(i));
			RationalApproximation rational(multishiftSolver);
			rational.setAlphas(std::vector<real_t>(rat.begin(), rat.begin() + rat.size()/2));
			rational.setBetas(std::vector<real_t>(rat.begin() + rat.size()/2, rat.end()));
			//We apply the twist
			for (unsigned int k = 0; k < rational.getBetas().size(); ++k) {
				rational.getBetas()[k] += twist;
			}
			rational.setPrecision(environment.configurations.get<double>("metropolis_inverter_precision"));
			rational.setMaximumRecursion(environment.configurations.get<unsigned int>("metropolis_inverter_max_steps"));
			rationalApproximationsMetropolis.push_back(rational);
		}
	}

	//We allow the usage of different precision for different level
	int numberLevels = environment.configurations.get< unsigned int >("number_force_levels");
	real_t level_precisions[numberLevels];
	try {
		for (int i = 1; i <= numberLevels; ++i) {
			level_precisions[i-1] = environment.configurations.get<double>(std::string("force_inverter_precision_level_")+toString(i));
		}
	} catch (NotFoundOption& ex) {
		for (int i = 1; i <= numberLevels; ++i) {
			level_precisions[i-1] = environment.configurations.get<double>("force_inverter_precision");
		}
		if (isOutputProcess()) std::cout << "MultiStepNFlavorQCDUpdater::Warning, a single precision is provided for all the level of the force!" << std::endl;
	}

	//Then take the rational function approximation for the force step
	if (rationalApproximationsForce.empty()) {

		for (int i = 1; i <= numberLevels; ++i) {
			int numberPseudofermions = environment.configurations.get< unsigned int >("number_pseudofermions");
			std::vector<RationalApproximation> levelRationaApproximationForce;
			for (int j = 1; j <= numberPseudofermions; ++j) {
				std::vector<real_t> rat = environment.configurations.get< std::vector<real_t> >(std::string("force_rational_fraction_")+toString(j)+"_level_"+toString(i));
				RationalApproximation rational(multishiftSolver);
				rational.setAlphas(std::vector<real_t>(rat.begin(), rat.begin() + rat.size()/2));
				rational.setBetas(std::vector<real_t>(rat.begin() + rat.size()/2, rat.end()));
				//We apply the twist
				for (unsigned int k = 0; k < rational.getBetas().size(); ++k) {
					rational.getBetas()[k] += twist;
				}
				rational.setPrecision(level_precisions[i - 1]);
				rational.setMaximumRecursion(environment.configurations.get<unsigned int>("force_inverter_max_steps"));
				levelRationaApproximationForce.push_back(rational);
			}
			rationalApproximationsForce.push_back(levelRationaApproximationForce);
		}
	}
}

void MultiStepNFlavorQCDUpdater::checkTheory(const environment_t& environment) const {
	double testerForce = 1., testerMetropolis = 1., testerHeatBath = 1.;
	double epsilon = 0.0001;
	int numberPseudofermions = environment.configurations.get< unsigned int >("number_pseudofermions");
	for (int i = 0; i < numberPseudofermions; ++i) {
		testerForce *= rationalApproximationsForce[0][i].evaluate(2.).real();
		testerMetropolis *= rationalApproximationsMetropolis[i].evaluate(2.).real();
		testerHeatBath *= rationalApproximationsHeatBath[i].evaluate(2.).real();
	}
	double numberFermions = -2*log(testerMetropolis)/log(2.);//We are using the square of the dirac operator, hence we have a factor 2
	if (isOutputProcess()) std::cout << "NFlavorQCDUpdater::The theory has " <<  numberFermions << " nf." << std::endl;
#ifdef ADJOINT
	if (isOutputProcess() && fabs(numberFermions - 0.5) < epsilon) std::cout << "NFlavorQCDUpdater::The theory seems SUSY" << std::endl;
#endif
#ifndef ADJOINT
	if (isOutputProcess() && fabs(numberFermions - 1) < epsilon) std::cout << "NFlavorQCDUpdater::The theory seems 1 FlavorQCD" << std::endl;
	if (isOutputProcess() && fabs(numberFermions - 2) < epsilon) std::cout << "NFlavorQCDUpdater::The theory seems 2 FlavorQCD" << std::endl;
#endif
	if (isOutputProcess() && fabs(testerMetropolis/testerForce - 1.) > epsilon) {
		std::cout << "NFlavorQCDUpdater::Warning, large mismatch between force and metropolis approximations: " << fabs(testerMetropolis/testerForce - 1.) << std::endl;
	}
	if (isOutputProcess() && fabs(testerMetropolis*testerHeatBath*testerHeatBath - 1.) > epsilon) {
		std::cout << "NFlavorQCDUpdater::Warning, large mismatch between heatbath and metropolis approximations: " << fabs(testerMetropolis*testerHeatBath*testerHeatBath - 1.) << " " << testerMetropolis << " " << testerHeatBath << std::endl;
	}
}

void MultiStepNFlavorQCDUpdater::execute(environment_t& environment) {
	//First we initialize the approximations
	this->initializeApproximations(environment);

	//We check the theory that is simulated
	if (environment.iteration == 0 && environment.sweep == 0) {
		this->checkTheory(environment);
	}

	//Initialize the momenta
	this->randomMomenta(momenta);
	//Copy the environment
	environmentNew = environment;

	//Take the Dirac Operator for Metropolis/Heatbath
	if (diracOperatorMetropolis == 0)  diracOperatorMetropolis = DiracOperator::getInstance(environment.configurations.get<std::string>("MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::dirac_operator"), 1, environment.configurations,  "MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::");
	diracOperatorMetropolis->setLattice(environment.getFermionLattice());

	//Take the Square Dirac Operator for Metropolis/Heatbath
	if (squareDiracOperatorMetropolis == 0) squareDiracOperatorMetropolis = DiracOperator::getInstance(environment.configurations.get<std::string>("MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::dirac_operator"), 2, environment.configurations,  "MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::");
	squareDiracOperatorMetropolis->setLattice(environment.getFermionLattice());

	//Take the Dirac Operator for the force
	if (diracOperatorForce == 0)  diracOperatorForce = DiracOperator::getInstance(environment.configurations.get<std::string>("MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::dirac_operator"), 1, environment.configurations,  "MultiStepNFlavorQCDUpdater::dirac_operator_force::");
	diracOperatorForce->setLattice(environment.getFermionLattice());

	//Take the Square Dirac Operator for the force
	if (squareDiracOperatorForce == 0) squareDiracOperatorForce = DiracOperator::getInstance(environment.configurations.get<std::string>("MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::dirac_operator"), 2, environment.configurations,  "MultiStepNFlavorQCDUpdater::dirac_operator_force::");
	squareDiracOperatorForce->setLattice(environment.getFermionLattice());

	//Take the gauge action
	if (gaugeAction == 0) gaugeAction = GaugeAction::getInstance(environment.configurations.get<std::string>("name_action"),environment.configurations.get<double>("beta"));

	//Initialize the pseudofermion fields
	std::vector<extended_dirac_vector_t>::iterator i;
	std::vector<RationalApproximation>::iterator j = rationalApproximationsHeatBath.begin();
	long_real_t oldPseudoFermionEnergy = 0.;

	for (i = pseudofermions.begin(); i != pseudofermions.end(); ++i) {
		this->generateGaussianDiracVector(tmp_pseudofermion);
		oldPseudoFermionEnergy += AlgebraUtils::squaredNorm(tmp_pseudofermion);
		//Heat bath by multiply the tmp_pseudofermion to the polynomial of the dirac operator
		j->evaluate(squareDiracOperatorMetropolis, *i, tmp_pseudofermion);
		++j;


		if (i == pseudofermions.begin()) {
			try {
				std::string flag = environment.configurations.get<std::string>("check_rational_approximations");

				if (environment.sweep == 0 && environment.iteration == 0 && flag == "true") {
					//Now we test the correctness of rational/heatbath
					long_real_t test = 0.;

					//Now we use the better approximation for the metropolis step
					std::vector<RationalApproximation>::iterator rational = rationalApproximationsMetropolis.begin();
					//Now we evaluate it with the rational approximation of the inverse
					rational->evaluate(squareDiracOperatorMetropolis,tmp_pseudofermion,*i);
					test += real(AlgebraUtils::dot(*i,tmp_pseudofermion));
					if (isOutputProcess()) std::cout << "NFlavoQCDUpdater::Consistency check for the metropolis: " << test - oldPseudoFermionEnergy << std::endl;

					//Now we use the approximation for the force step
					rational = rationalApproximationsForce[0].begin();
					test = 0.;
					//Now we evaluate it with the rational approximation of the inverse
					rational->evaluate(squareDiracOperatorForce,tmp_pseudofermion,*i);
					test += real(AlgebraUtils::dot(*i,tmp_pseudofermion));

					if (isOutputProcess()) std::cout << "NFlavoQCDUpdater::Consistency check for the first level of the force: " << test - oldPseudoFermionEnergy << std::endl;
				}

			} catch (NotFoundOption& ex) {
				if (isOutputProcess() && environment.sweep == 0 && environment.iteration == 0 && environment.measurement) std::cout << "NFlavorQCDUpdater::No consistency check of metropolis/force approximations!" << std::endl;
			}
		}

	}

	//Get the initial energy of momenta
	long_real_t oldMomentaEnergy = this->momentaEnergy(momenta);
	//Get the initial energy of the lattice
	long_real_t oldLatticeEnergy = gaugeAction->energy(environment);

	//Set the dirac operator to the new environment
	squareDiracOperatorMetropolis->setLattice(environmentNew.getFermionLattice());
	diracOperatorMetropolis->setLattice(environmentNew.getFermionLattice());
	squareDiracOperatorForce->setLattice(environmentNew.getFermionLattice());
	diracOperatorForce->setLattice(environmentNew.getFermionLattice());

	//Take the fermion action
	if (fermionAction == 0) {
		int numberLevels = environment.configurations.get< unsigned int >("number_force_levels");
		fermionAction = new NFlavorFermionAction*[numberLevels];
		for (int j = 0; j < numberLevels; ++j) {
			fermionAction[j] = new NFlavorFermionAction(squareDiracOperatorForce, diracOperatorForce, rationalApproximationsForce[j]);
			//TODO dirac operator correctly set?
			for (i = pseudofermions.begin(); i != pseudofermions.end(); ++i) {
				//Add the pseudofermions
				fermionAction[j]->addPseudoFermion(&(*i));
			}
			//Set the precision of the inverter
			fermionAction[j]->setForcePrecision(rationalApproximationsForce[j].begin()->getPrecision());
			fermionAction[j]->setForceMaxIterations(environment.configurations.get<unsigned int>("force_inverter_max_steps"));
		}
	}


	//Take the global action
	if (nFlavorQCDAction == 0) nFlavorQCDAction = new NFlavorAction(gaugeAction, fermionAction[0]);//TODO, we skip the other forces

	//The t-length of a single integration step
	real_t t_length = environment.configurations.get<double>("hmc_t_length");
	std::vector<Force*> forces;
	//The numbers of integration steps
	std::vector<unsigned int> numbers_steps = environment.configurations.get< std::vector<unsigned int> >("number_hmc_steps");
	if (numbers_steps.size() == 1) {
		int numberLevels = environment.configurations.get< unsigned int >("number_force_levels");
		if (isOutputProcess() && numberLevels != 1) std::cout << "MultiStepNFlavorHMCUpdater::Warning, with only one time integration only the first level of the force is used!" << std::endl;
		forces.push_back(nFlavorQCDAction);
	} else if (numbers_steps.size() == 2) {
		int numberLevels = environment.configurations.get< unsigned int >("number_force_levels");
		if (isOutputProcess() && numberLevels != 1) std::cout << "MultiStepNFlavorHMCUpdater::Warning, with only two time integration only the first level of the force is used!" << std::endl;
		forces.push_back(fermionAction[0]);
		forces.push_back(gaugeAction);
	} else if (numbers_steps.size() == 3) {
		int numberLevels = environment.configurations.get< unsigned int >("number_force_levels");
		if (isOutputProcess() && numberLevels != 2) std::cout << "MultiStepNFlavorHMCUpdater::Warning, with only three time integration only the first two levels of the force is used!" << std::endl;
		forces.push_back(fermionAction[1]);
		forces.push_back(fermionAction[0]);
		forces.push_back(gaugeAction);
	} else if (numbers_steps.size() == 4) {
		int numberLevels = environment.configurations.get< unsigned int >("number_force_levels");
		if (isOutputProcess() && numberLevels != 3) std::cout << "MultiStepNFlavorHMCUpdater::Warning, with only four time integration only the first two levels of the force is used!" << std::endl;
		forces.push_back(fermionAction[2]);
		forces.push_back(fermionAction[1]);
		forces.push_back(fermionAction[0]);
		forces.push_back(gaugeAction);
	}
	else {
		if (isOutputProcess()) std::cout << "MultiStepNFlavorHMCUpdater::Warning, NFlavor does not support more than four time integrations!" << std::endl;
		numbers_steps.resize(1);
		forces.push_back(nFlavorQCDAction);
	}

#ifdef DEBUGFORCE
	//Test of the force
	TestForce testForce;
	extended_gauge_lattice_t tmp;
	//Internal calculations needed
	nFlavorQCDAction->updateForce(tmp, environment);
	testForce.genericTestForce(environment, nFlavorQCDAction, tmp[5][2], 5, 2);
#endif

	Integrate* integrate = Integrate::getInstance(environment.configurations.get<std::string>("name_integrator"));

	//Integrate numerically the equation of motion
	integrate->integrate(environmentNew, momenta, forces, numbers_steps, t_length);
#ifdef REVERSIBILITY_CHECK
	integrate->integrate(environmentNew, momenta, forces, numbers_steps, -t_length);
#endif

	//Get the final energy of momenta
	long_real_t newMomentaEnergy = this->momentaEnergy(momenta);
	//Get the final energy of the lattice
	long_real_t newLatticeEnergy = gaugeAction->energy(environmentNew);
	//Get the final energy of the pseudofermions
	long_real_t newPseudoFermionEnergy = 0.;
	diracOperatorMetropolis->setLattice(environmentNew.getFermionLattice());//TODO TODO TODO
	squareDiracOperatorMetropolis->setLattice(environmentNew.getFermionLattice());
	//Now we use the better approximation for the metropolis step
	std::vector<RationalApproximation>::iterator rational = rationalApproximationsMetropolis.begin();
	for (i = pseudofermions.begin(); i != pseudofermions.end(); ++i) {
		//Now we evaluate it with the rational approximation of the inverse
		rational->evaluate(squareDiracOperatorMetropolis,tmp_pseudofermion,*i);
		newPseudoFermionEnergy += real(AlgebraUtils::dot(*i,tmp_pseudofermion));
		++rational;
	}

	//Global Metropolis Step
	bool metropolis = this->metropolis(oldMomentaEnergy + oldLatticeEnergy + oldPseudoFermionEnergy, newMomentaEnergy + newLatticeEnergy + newPseudoFermionEnergy);

	if (metropolis) {
		environment = environmentNew;
		if (environment.measurement && isOutputProcess()) {
			GlobalOutput* output = GlobalOutput::getInstance();

			output->push("hmc_history");

			output->write("hmc_history", - (oldMomentaEnergy + oldLatticeEnergy + oldPseudoFermionEnergy) + (newMomentaEnergy + newLatticeEnergy + newPseudoFermionEnergy));
			output->write("hmc_history", 1);

			output->pop("hmc_history");
		}
	}
	else {
		if (environment.measurement && isOutputProcess()) {
			GlobalOutput* output = GlobalOutput::getInstance();

			output->push("hmc_history");

			output->write("hmc_history", - (oldMomentaEnergy + oldLatticeEnergy + oldPseudoFermionEnergy) + (newMomentaEnergy + newLatticeEnergy + newPseudoFermionEnergy));
			output->write("hmc_history", 0);

			output->pop("hmc_history");
		}
	}

	delete integrate;

	/*
	DiracOperator* diracOperator2 = new SquareDiracWilsonOperator(&environment.gaugeLinkConfiguration, environment.configurations.get<double>("kappa"));

	dirac_vector_t tmp1, tmp2, tmp3, tmp_pseudofermion;

	std::vector<complex> pol = environment.configurations.get< std::vector<complex> >("heatbath_polynomial");
	Polynomial polynomial;
	polynomial.setScaling(pol.front());
	pol.erase(pol.begin());
	polynomial.setRoots(pol);

	std::cout << "Rational 1/sqrt(1): " << std::setprecision(15) << rational.evaluate(complex(1,0.)) << std::endl;

	//std::cout << "Vediamo la nostra morte 0: " << std::endl << pseudofermions.front()[5][3] << std::endl;

	//polynomial.evaluate(diracOperator, tmp1, pseudofermions.front());

	this->generateGaussianDiracVector(tmp_pseudofermion);

	//polynomial.evaluate(diracOperator, tmp1, tmp_pseudofermion);

	BiConjugateGradient* biConjugateGradient = BiConjugateGradient::getInstance();
	//
	biConjugateGradient->solve(diracOperator, tmp_pseudofermion, tmp3);

	diracOperator = new SquareDiracWilsonOperator();
	diracOperator->setLattice(&environment.gaugeLinkConfiguration);
	diracOperator->setKappa(environment.configurations.get<double>("kappa"));

	//std::cout << "Vediamo la nostra morte 01: " << std::endl << tmp_pseudofermion[5][3] << std::endl;

	rational.evaluate(diracOperator, tmp1, tmp_pseudofermion);
	//rational.evaluate(diracOperator, tmp2, tmp1);
	polynomial.evaluate(diracOperator, tmp2, tmp1);
	//biConjugateGradient->setPrecision(0.00000000000001);
	biConjugateGradient->solve(diracOperator, tmp_pseudofermion, tmp3);
	//diracOperator->multiply(tmp3,tmp2);

	std::cout << "Vediamo la nostra morte 1: " << std::endl << tmp1[5][3] << std::endl;
	std::cout << "Vediamo la nostra morte 2: " << std::endl << tmp2[5][3] << std::endl;
	std::cout << "Vediamo la nostra morte 3: " << std::endl << tmp_pseudofermion[5][3] << std::endl;
	*/
}

void MultiStepNFlavorQCDUpdater::registerParameters(po::options_description& desc) {
	static bool single = true;
	if (single) desc.add_options()
		("MultiStepNFlavorQCDUpdater::twist", po::value<double>()->default_value(0.0), "set the value of the twist applied to fermions")
		("MultiStepNFlavorQCDUpdater::inverter_precision", po::value<double>()->default_value(0.0000000001), "set the precision used by the inverter")
		("MultiStepNFlavorQCDUpdater::inverter_max_steps", po::value<unsigned int>()->default_value(5000), "set the maximum steps used by the inverter")
		
		("MultiStepNFlavorQCDUpdater::multigrid", po::value<std::string>()->default_value("false"), "Should we use the multigrid inverter? true/false")
		("MultiStepNFlavorQCDUpdater::multigrid_basis_dimension", po::value<unsigned int>()->default_value(20), "The dimension of the basis for multigrid")
		("MultiStepNFlavorQCDUpdater::multigrid_block_size", po::value<std::string>()->default_value("{4,4,4,4}"), "Block size for Multigrid (syntax: {bx,by,bz,bt})")

		("MultiStepNFlavorQCDUpdater::sap_block_size", po::value<std::string>()->default_value("{4,4,4,4}"), "Block size for SAP (syntax: {bx,by,bz,bt})")
		("MultiStepNFlavorQCDUpdater::sap_iterations", po::value<unsigned int>()->default_value(5), "The number of sap iterations")
		("MultiStepNFlavorQCDUpdater::sap_inverter_precision", po::value<double>()->default_value(0.00000000001), "The precision of the inner SAP inverter")
		("MultiStepNFlavorQCDUpdater::sap_inverter_max_steps", po::value<unsigned int>()->default_value(50), "The maximum number of steps for the inner SAP inverter")
		("MultiStepNFlavorQCDUpdater::gmres_inverter_precision", po::value<double>()->default_value(0.00000000001), "The precision of the GMRES inverter used to initialize the multigrid basis")
		("MultiStepNFlavorQCDUpdater::gmres_inverter_max_steps", po::value<unsigned int>()->default_value(100), "The maximum number of steps for the GMRES inverter used to initialize the multigrid basis")
		;
	if (single) {
		DiracOperator::registerParameters(desc, "MultiStepNFlavorQCDUpdater::dirac_operator_metropolis::");
		DiracOperator::registerParameters(desc, "MultiStepNFlavorQCDUpdater::dirac_operator_force::");
	}
	single = false;
}

} /* namespace Update */
