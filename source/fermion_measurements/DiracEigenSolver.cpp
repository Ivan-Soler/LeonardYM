/*
 * DiracEigenSolver.cpp
 *
 *  Created on: Jun 26, 2012
 *      Author: spiem_01
 */

#include "DiracEigenSolver.h"
#include "algebra_utils/AlgebraUtils.h"
#include "inverters/BiConjugateGradient.h"
#ifdef EIGEN
#include <Eigen/Eigenvalues>
#endif
#include <algorithm>

namespace Update {

const std::complex<real_t> EigevaluesMap[4] = {1., -1., std::complex<real_t>(0.,1.), std::complex<real_t>(0.,-1.)};

bool maxcomparison(const std::complex<real_t>& i, const std::complex<real_t>& j) { return (abs(i)>abs(j)); }
bool mincomparison(const std::complex<real_t>& i, const std::complex<real_t>& j) { return (abs(i)<abs(j)); }

inline void rotateVector(reduced_dirac_vector_t& vector, EigevaluesMode mode) {
	typedef reduced_dirac_vector_t::Layout Layout;
	if (mode != LargestReal) {
#pragma omp parallel for
		for (int site = 0; site < vector.completesize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) vector[site][mu] = EigevaluesMap[mode]*vector[site][mu];
		}
	}
}

DiracEigenSolver::DiracEigenSolver() : epsilon(0.00001), extra_steps(250), biConjugateGradient(0) { }

DiracEigenSolver::~DiracEigenSolver() {
	delete biConjugateGradient;
}

void DiracEigenSolver::setPrecision(const real_t& precision) {
	epsilon = precision;
}

real_t DiracEigenSolver::getPrecision() const {
	return epsilon;
}

void DiracEigenSolver::maximumEigenvalues(DiracOperator* diracOperator, std::vector< std::complex<real_t> >& eigenvalues, std::vector<reduced_dirac_vector_t>& eigenvectors, unsigned int n, EigevaluesMode mode) {
	typedef reduced_dirac_vector_t::Layout Layout;
	
	unsigned int steps = extra_steps + n;
	//The orthonormal vectors generated by the arnoldi process
	std::vector<reduced_dirac_vector_t> V;
	//Reserve some memory
	V.resize(steps);
	AlgebraUtils::generateRandomVector(V[0]);
	AlgebraUtils::normalize(V[0]);
	reduced_dirac_vector_t w, f;
	//w = D.V[0]
	reduced_dirac_vector_t tmpm = V[0];
	rotateVector(tmpm, mode);
	diracOperator->multiplyAdd(w,tmpm,V[0],+5.);
	
	std::complex<real_t> alpha = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[0], w));
	matrix_t H(steps,steps);
	set_to_zero(H);
	//f = w - alpha*V[0]
#pragma omp parallel for
	for (int site = 0; site < w.localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			f[site][mu] = w[site][mu] - alpha*V[0][site][mu];
		}
	}
	f.updateHalo();//TODO maybe not needed
	H(0,0) = alpha;
	for (unsigned int j = 0; j < steps - 1; ++j) {
		//beta = norm(f)
		real_t beta = sqrt(AlgebraUtils::squaredNorm(f));
		//V[j] = f/beta
#pragma omp parallel for
		for (int site = 0; site < f.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				V[j+1][site][mu] = f[site][mu]/beta;
			}
		}
		V[j+1].updateHalo();
		//H(j+1,j) = beta
		H(j+1,j) = beta;
		//w = D.V[j+1]
		tmpm = V[j+1];
		rotateVector(tmpm, mode);
		diracOperator->multiplyAdd(w,tmpm,V[j+1],+5.);
		
		//Gram schimdt
#pragma omp parallel for
		for (int site = 0; site < w.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				f[site][mu] = w[site][mu];
			}
		}
		f.updateHalo();
		for (unsigned int i = 0; i <= j+1; ++i) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[i],w));
			H(i,j+1) = proj;
#pragma omp parallel for
			for (int site = 0; site < f.localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					f[site][mu] -= proj*V[i][site][mu];
				}
			}
			f.updateHalo();//TODO maybe not needed
		}
		//More stable Gram schimdt
		for (unsigned int i = 0; i <= j+1; ++i) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[i],f));
			H(i,j+1) += proj;
#pragma omp parallel for
			for (int site = 0; site < f.localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					f[site][mu] -= proj*V[i][site][mu];
				}
			}
			f.updateHalo();//TODO maybe not needed
		}
	}

	eigenvalues.resize(steps);

	matrix_t eigvec(steps,steps);
#ifdef EIGEN
	Eigen::ComplexEigenSolver<matrix_t> ces(H, true);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = EigevaluesMap[mode]*(ces.eigenvalues()[i] - static_cast<real_t>(5.));
	}
	eigvec = ces.eigenvectors();
#endif
#ifdef ARMADILLO
	vector_t eigval(steps);
	arma::eig_gen(eigval, eigvec, H);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = EigevaluesMap[mode]*(eigval[i] - static_cast<real_t>(5.));
	}
#endif

	eigenvectors.resize(steps);
	for (unsigned int i = 0; i < steps; ++i) {
		AlgebraUtils::setToZero(eigenvectors[i]);
		for (unsigned int j = 0; j < steps; ++j) {
#pragma omp parallel for
			for (int site = 0; site < Layout::localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					eigenvectors[i][site][mu] += eigvec.at(j,i)*V[j][site][mu];
				}
			}
		}
		eigenvectors[i].updateHalo();
		
	}

	reduced_dirac_vector_t tmp, tmpe;
	
	//Now we check the convergence
	diracOperator->multiply(tmp,eigenvectors.back());
#pragma omp parallel for
	for (int site = 0; site < Layout::localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			tmpe[site][mu] = eigenvalues.back()*eigenvectors.back()[site][mu];
		}
	}

	std::complex<long_real_t> diffnorm = AlgebraUtils::differenceNorm(tmp,tmpe);

	if (isOutputProcess()) std::cout << "DiracEigenSolver::Convergence precision: " << abs(diffnorm) << std::endl;

	std::reverse(eigenvectors.begin(),eigenvectors.end());
	std::reverse(eigenvalues.begin(),eigenvalues.end());
	
	eigenvalues.erase(eigenvalues.end() - extra_steps, eigenvalues.end());
	eigenvectors.erase(eigenvectors.end() - extra_steps, eigenvectors.end());


	/*
	typedef reduced_dirac_vector_t::Layout Layout;
	
	int steps = extra_steps + n;
	std::vector<reduced_dirac_vector_t> V(steps);
	
	AlgebraUtils::generateRandomVector(V[0]);
	AlgebraUtils::normalize(V[0]);

	for (int iteration = 1; iteration < steps; ++iteration) {
		diracOperator->multiply(V[iteration],V[iteration-1]);
		AlgebraUtils::normalize(V[iteration]);
	}

	for (int i = 0; i < steps; ++i) {
		for (int j = 0; j < i; ++j) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[j],V[i]));
#pragma omp parallel for
			for (int site = 0; site < Layout::localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					V[i][site][mu] -= proj*V[j][site][mu];
				}
			}
		}
		V[i].updateHalo();
	}
	
	reduced_dirac_vector_t tmp, tmpe;
	//Now we construct the reduced matrix
	matrix_t reduced(steps,steps);
	set_to_zero(reduced);

	for (int i = 0; i < steps; ++i) {
		diracOperator->multiply(tmp,V[i]);
		for (int j = 0; j < steps; ++j) {
			reduced(i,j) = AlgebraUtils::dot(V[j],tmp);
		}
	}

	std::vector< std::complex<real_t> > eigenvalues(steps);
	matrix_t eigvec(steps,steps);
#ifdef EIGEN
	Eigen::ComplexEigenSolver<matrix_t> ces(reduced, true);
	for (int i = 0; i < steps; ++i) {
		eigenvalues[i] = ces.eigenvalues()[i];
	}
	eigvec = ces.eigenvectors();
#endif
#ifdef ARMADILLO
	vector_t eigval(steps);
	arma::eig_gen(eigval, eigvec, reduced);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = eigval[i];
	}
#endif
	//std::sort(eigenvalues.begin(),eigenvalues.end(),maxcomparison);
	
	std::vector<reduced_dirac_vector_t> eigenvectors(steps);
	for (int i = 0; i < steps; ++i) {
		AlgebraUtils::setToZero(eigenvectors[i]);
		for (int j = 0; j < steps; ++j) {
#pragma omp parallel for
			for (int site = 0; site < Layout::localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					eigenvectors[i][site][mu] += eigvec.at(i,j)*V[j][site][mu];
				}
			}
		}
		eigenvectors[i].updateHalo();
	}
	std::reverse(eigenvectors.begin(),eigenvectors.end());
	std::reverse(eigenvalues.begin(),eigenvalues.end());

	
	//Now we check the convergence
	diracOperator->multiply(tmp,eigenvectors[0]);
#pragma omp parallel for
	for (int site = 0; site < Layout::localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			tmpe[site][mu] = eigenvalues[0]*eigenvectors[0][site][mu];
		}
	}

	std::complex<long_real_t> diffnorm = AlgebraUtils::differenceNorm(tmp,tmpe);

	std::cout << "DiracEigenSolver::Convergence precision: " << diffnorm << std::endl;
	return eigenvalues;*/
}

void DiracEigenSolver::minimumEigenvalues(DiracOperator* diracOperator, std::vector< std::complex<real_t> >& eigenvalues, std::vector<reduced_dirac_vector_t>& eigenvectors/*, Polynomial& map*/, unsigned int n/*, int nmode*/) {
	typedef reduced_dirac_vector_t::Layout Layout;

	/*std::complex<real_t> mode[] = {std::complex<real_t>(1,0),std::complex<real_t>(-1,0),std::complex<real_t>(0,1),std::complex<real_t>(0,-1)};
	
	unsigned int steps = extra_steps + n;
	//The orthonormal vectors generated by the arnoldi process
	std::vector<reduced_dirac_vector_t> V;
	//Reserve some memory
	V.resize(steps);
	AlgebraUtils::generateRandomVector(V[0]);
	AlgebraUtils::normalize(V[0]);
	reduced_dirac_vector_t w, f;
	//w = map(V[0])
	map.evaluate(diracOperator,w,V[0]);
	if (nmode != 0) {
#pragma omp parallel for
		for (int site = 0; site < Layout::localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				w[site][mu] = mode[nmode]*w[site][mu];
			}
		}
		w.updateHalo();
	}
	std::complex<real_t> alpha = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[0], w));
	matrix_t H(steps,steps);
	H.zeros();
	//f = w - alpha*V[0]
#pragma omp parallel for
	for (int site = 0; site < w.localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			f[site][mu] = w[site][mu] - alpha*V[0][site][mu];
		}
	}
	f.updateHalo();//TODO maybe not needed
	H(0,0) = alpha;
	for (unsigned int j = 0; j < steps - 1; ++j) {
		//beta = norm(f)
		real_t beta = sqrt(AlgebraUtils::squaredNorm(f));
		//V[j] = f/beta
#pragma omp parallel for
		for (unsigned int site = 0; site < f.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				V[j+1][site][mu] = f[site][mu]/beta;
			}
		}
		V[j+1].updateHalo();
		//H(j+1,j) = beta
		H(j+1,j) = beta;
		//w = map(V[j+1])
		map.evaluate(diracOperator,w,V[j+1]);
		if (nmode != 0) {
#pragma omp parallel for
			for (int site = 0; site < Layout::localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					w[site][mu] = mode[nmode]*w[site][mu];
				}
			}
			w.updateHalo();
		}
		//Gram schimdt
#pragma omp parallel for
		for (unsigned int site = 0; site < w.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				f[site][mu] = w[site][mu];
			}
		}
		f.updateHalo();
		for (unsigned int i = 0; i <= j+1; ++i) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[i],w));
			H(i,j+1) = proj;
#pragma omp parallel for
			for (unsigned int site = 0; site < f.localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					f[site][mu] -= proj*V[i][site][mu];
				}
			}
			f.updateHalo();//TODO maybe not needed
		}
		//More stable Gram schimdt
		for (unsigned int i = 0; i <= j+1; ++i) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[i],f));
			H(i,j+1) += proj;
#pragma omp parallel for
			for (unsigned int site = 0; site < f.localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					f[site][mu] -= proj*V[i][site][mu];
				}
			}
			f.updateHalo();//TODO maybe not needed
		}
	}

	eigenvalues.resize(steps);

	matrix_t eigvec(steps,steps);
#ifdef EIGEN
	Eigen::ComplexEigenSolver<matrix_t> ces(H, true);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = ces.eigenvalues()[i];
	}
	eigvec = ces.eigenvectors();
#endif
#ifdef ARMADILLO
	vector_t eigval(steps);
	arma::eig_gen(eigval, eigvec, H);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = eigval[i];
	}
#endif

	eigenvectors.resize(steps);
	for (int i = 0; i < steps; ++i) {
		AlgebraUtils::setToZero(eigenvectors[i]);
		for (int j = 0; j < steps; ++j) {
#pragma omp parallel for
			for (int site = 0; site < Layout::localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					eigenvectors[i][site][mu] += eigvec.at(j,i)*V[j][site][mu];
				}
			}
		}
		eigenvectors[i].updateHalo();
		
	}

	reduced_dirac_vector_t tmp, tmpe;
	
	//Now we check the convergence
	map.evaluate(diracOperator,tmp,eigenvectors.back());
#pragma omp parallel for
	for (int site = 0; site < Layout::localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			tmpe[site][mu] = eigenvalues.back()*eigenvectors.back()[site][mu];
		}
	}

	std::complex<long_real_t> diffnorm = AlgebraUtils::differenceNorm(tmp,tmpe);

	std::cout << "DiracEigenSolver::Convergence precision: " << abs(diffnorm) << std::endl;

	std::reverse(eigenvectors.begin(),eigenvectors.end());
	std::reverse(eigenvalues.begin(),eigenvalues.end());
	
	eigenvalues.erase(eigenvalues.end() - extra_steps, eigenvalues.end());
	eigenvectors.erase(eigenvectors.end() - extra_steps, eigenvectors.end());

	//Now we extract the unmapped eigenvalues
	for (int i = 0; i < eigenvectors.size(); ++i) {
		diracOperator->multiply(tmp,eigenvectors[i]);
		long_real_t eigenvalueReal = 0.;
		long_real_t eigenvalueImag = 0.;
		for (unsigned int mu = 0; mu < 4; ++mu) {
			for (int c = 0; c < diracVectorLength; ++c) {
				std::complex<real_t> ltmp = tmp[0][mu][c]/eigenvectors[i][0][mu][c];
				eigenvalueReal += real(ltmp);
				eigenvalueImag += real(ltmp);
			}
		}
		reduceAllSum(eigenvalueReal);
		reduceAllSum(eigenvalueImag);
		eigenvalues[i] = std::complex<real_t>(eigenvalueReal/(4*diracVectorLength),eigenvalueImag/(4*diracVectorLength));
	}

	*/

	
	unsigned int steps = extra_steps + n;
	//The orthonormal vectors generated by the arnoldi process
	std::vector<reduced_dirac_vector_t> V;
	//Reserve some memory
	V.resize(steps);
	AlgebraUtils::generateRandomVector(V[0]);
	AlgebraUtils::normalize(V[0]);
	reduced_dirac_vector_t w, f;
	if (biConjugateGradient == 0) biConjugateGradient = new BiConjugateGradient();
	biConjugateGradient->setPrecision(epsilon);
	//w = D^(-1).V[0]
	biConjugateGradient->solve(diracOperator,V[0],w);
	std::complex<real_t> alpha = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[0], w));
	matrix_t H(steps,steps);
	H.zeros();
	//f = w - alpha*V[0]
#pragma omp parallel for
	for (int site = 0; site < w.localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			f[site][mu] = w[site][mu] - alpha*V[0][site][mu];
		}
	}
	f.updateHalo();//TODO maybe not needed
	H(0,0) = alpha;
	for (unsigned int j = 0; j < steps - 1; ++j) {
		//beta = norm(f)
		real_t beta = sqrt(AlgebraUtils::squaredNorm(f));
		//V[j] = f/beta
#pragma omp parallel for
		for (int site = 0; site < f.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				V[j+1][site][mu] = f[site][mu]/beta;
			}
		}
		V[j+1].updateHalo();
		//H(j+1,j) = beta
		H(j+1,j) = beta;
		//w = D^(-1).V[j+1]
		biConjugateGradient->solve(diracOperator,V[j+1],w);
		//Gram schimdt
#pragma omp parallel for
		for (int site = 0; site < w.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				f[site][mu] = w[site][mu];
			}
		}
		f.updateHalo();//TODO maybe not needed
		for (unsigned int i = 0; i <= j+1; ++i) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[i],w));
			H(i,j+1) = proj;
#pragma omp parallel for
			for (int site = 0; site < f.localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					f[site][mu] -= proj*V[i][site][mu];
				}
			}
			f.updateHalo();//TODO maybe not needed
		}
		//More stable Gram schimdt
		for (unsigned int i = 0; i <= j+1; ++i) {
			std::complex<real_t> proj = static_cast< std::complex<real_t> >(AlgebraUtils::dot(V[i],f));
			H(i,j+1) += proj;
#pragma omp parallel for
			for (int site = 0; site < f.localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					f[site][mu] -= proj*V[i][site][mu];
				}
			}
			f.updateHalo();//TODO maybe not needed
		}
	}
	//std::vector< std::complex<real_t> > result(steps);
	eigenvalues.resize(steps);
	
	matrix_t eigvec(steps,steps);
#ifdef EIGEN
	Eigen::ComplexEigenSolver<matrix_t> ces(H, true);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = static_cast<real_t>(1.)/ces.eigenvalues()[i];
	}
	eigvec = ces.eigenvectors();
#endif
#ifdef ARMADILLO
	vector_t eigval(steps);
	arma::eig_gen(eigval, eigvec, H);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = eigval[i];
	}
#endif

	eigenvectors.resize(steps);
	for (unsigned int i = 0; i < steps; ++i) {
		AlgebraUtils::setToZero(eigenvectors[i]);
		for (unsigned int j = 0; j < steps; ++j) {
#pragma omp parallel for
			for (int site = 0; site < Layout::localsize; ++site) {
				for (unsigned int mu = 0; mu < 4; ++mu) {
					eigenvectors[i][site][mu] += eigvec.at(j,i)*V[j][site][mu];
				}
			}
		}
		eigenvectors[i].updateHalo();
		
	}

	reduced_dirac_vector_t tmp, tmpe;
	
	//Now we check the convergence
	diracOperator->multiply(tmp,eigenvectors.back());
#pragma omp parallel for
	for (int site = 0; site < Layout::localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			tmpe[site][mu] = eigenvalues.back()*eigenvectors.back()[site][mu];
		}
	}

	std::complex<long_real_t> diffnorm = AlgebraUtils::differenceNorm(tmp,tmpe);

	if (isOutputProcess()) std::cout << "DiracEigenSolver::Convergence precision: " << abs(diffnorm) << std::endl;

	std::reverse(eigenvectors.begin(),eigenvectors.end());
	std::reverse(eigenvalues.begin(),eigenvalues.end());
	
	eigenvalues.erase(eigenvalues.end() - extra_steps, eigenvalues.end());
	eigenvectors.erase(eigenvectors.end() - extra_steps, eigenvectors.end());

	//std::reverse(eigenvectors.begin(),eigenvectors.end());
	//std::reverse(eigenvalues.begin(),eigenvalues.end());
/*#ifdef EIGEN
	Eigen::ComplexEigenSolver<matrix_t> ces(H, true);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = ces.eigenvalues()[i];
	}
#endif
#ifdef ARMADILLO
	matrix_t eigvec(steps,steps);
	vector_t eigval(steps);
	arma::eig_gen(eigval, eigvec, H);
	for (unsigned int i = 0; i < steps; ++i) {
		eigenvalues[i] = eigval[i];
	}
#endif
	std::sort(eigenvalues.begin(),eigenvalues.end(),mincomparison);

#ifdef EIGEN
	//Now we test the results, looking at the last error
	unsigned int lastvectorindex = 0;
	for (unsigned int i = 0; i < steps; ++i) {
		if (ces.eigenvalues()[i] == eigenvalues[0]) {
			lastvectorindex = i;
			break;
		}
	}

	reduced_dirac_vector_t eigenvector;
#pragma omp parallel for
	for (int site = 0; site < w.localsize; ++site) {
		for (unsigned int mu = 0; mu < 4; ++mu) {
			set_to_zero(eigenvector[site][mu]);
		}
	}

	for (unsigned int i = 0; i < steps; ++i) {
#pragma omp parallel for
		for (int site = 0; site < w.localsize; ++site) {
			for (unsigned int mu = 0; mu < 4; ++mu) {
				eigenvector[site][mu] += V[i][site][mu]*ces.eigenvectors()(lastvectorindex,i);
			}
		}
	}

	eigenvector.updateHalo();

	reduced_dirac_vector_t tmp;

	diracOperator->multiply(tmp, eigenvector);

	std::cout << "Error in norm: " << (tmp[0][0][0] - eigenvalues[n]*eigenvector[0][0][0]) << std::endl;
#endif*/
	//result.erase(result.end() - extra_steps, result.end());
	//return result;
}

void DiracEigenSolver::setExtraSteps(unsigned int _extra_steps) {
	extra_steps = _extra_steps;
}

unsigned int DiracEigenSolver::getExtraSteps() const {
	return extra_steps;
}

} /* namespace Update */
