/*******************************************************************************
GPU OPTIMIZED MONTE CARLO (GOMC) 2.70
Copyright (C) 2018  GOMC Group
A copy of the GNU General Public License can be found in the COPYRIGHT.txt
along with this program, also can be found at <http://www.gnu.org/licenses/>.
********************************************************************************/
#include "Ewald.h"
#include "CalculateEnergy.h"
#include "EnergyTypes.h"            //Energy structs
#include "EnsemblePreprocessor.h"   //Flags
#include "BasicTypes.h"             //uint
#include "System.h"                 //For init
#include "StaticVals.h"             //For init
#include "Forcefield.h"
#include "MoleculeKind.h"
#include "Coordinates.h"
#include "BoxDimensions.h"
#include "TrialMol.h"
#include "GeomLib.h"
#include "NumLib.h"
#include <cassert>
#ifdef GOMC_CUDA
#include "CalculateEwaldCUDAKernel.cuh"
#include "CalculateForceCUDAKernel.cuh"
#include "ConstantDefinitionsCUDAKernel.cuh"
#endif

//
//
//    Energy Calculation functions for Ewald summation method
//    Calculating self, correction and reciprocal part of ewald
//
//    Developed by Y. Li and Mohammad S. Barhaghi
//
//

using namespace geom;

Ewald::Ewald(StaticVals & stat, System & sys) :
  ff(stat.forcefield), mols(stat.mol), currentCoords(sys.coordinates),
  currentCOM(sys.com), sysPotRef(sys.potential), lambdaRef(sys.lambdaRef),
#ifdef VARIABLE_PARTICLE_NUMBER
  molLookup(sys.molLookup),
#else
  molLookup(stat.molLookup),
#endif
#ifdef VARIABLE_VOLUME
  currentAxes(sys.boxDimRef)
#else
  currentAxes(*stat.GetBoxDim())
#endif
{
  ewald = false;
  electrostatic = false;
  alpha = 0.0;
  recip_rcut = 0.0;
  recip_rcut_Sq = 0.0;
  multiParticleEnabled = stat.multiParticleEnabled;
}

Ewald::~Ewald()
{
  if(ff.ewald) {
#ifdef GOMC_CUDA
    DestroyEwaldCUDAVars(ff.particles->getCUDAVars());
#endif
    for (uint b = 0; b < BOXES_WITH_U_NB; b++) {
      delete[] kx[b];
      delete[] ky[b];
      delete[] kz[b];
      delete[] hsqr[b];
      delete[] prefact[b];
      delete[] kxRef[b];
      delete[] kyRef[b];
      delete[] kzRef[b];
      delete[] hsqrRef[b];
      delete[] prefactRef[b];
      delete[] sumRnew[b];
      delete[] sumInew[b];
      delete[] sumRref[b];
      delete[] sumIref[b];
    }

    delete[] kmax;
    delete[] kx;
    delete[] ky;
    delete[] kz;
    delete[] hsqr;
    delete[] prefact;
    delete[] kxRef;
    delete[] kyRef;
    delete[] kzRef;
    delete[] hsqrRef;
    delete[] prefactRef;
    delete[] sumRnew;
    delete[] sumInew;
    delete[] sumRref;
    delete[] sumIref;
    delete[] imageSize;
    delete[] imageSizeRef;
  }
}

void Ewald::Init()
{

  for(uint m = 0; m < mols.count; ++m) {
    const MoleculeKind& molKind = mols.GetKind(m);
    for(uint a = 0; a < molKind.NumAtoms(); ++a) {
      particleKind.push_back(molKind.AtomKind(a));
      particleMol.push_back(m);
      particleCharge.push_back(molKind.AtomCharge(a));
      if(std::abs(molKind.AtomCharge(a)) < 0.000000001) {
        particleHasNoCharge.push_back(true);
      } else {
        particleHasNoCharge.push_back(false);
      }
    }
  }

  // initialize starting index and length index of each molecule
  startMol.resize(currentCoords.Count());
  lengthMol.resize(currentCoords.Count());

  for(int atom = 0; atom < currentCoords.Count(); atom++) {
    startMol[atom] = mols.MolStart(particleMol[atom]);
    lengthMol[atom] = mols.MolLength(particleMol[atom]);
  }

  AllocMem();
  //initialize K vectors and reciprocate terms
  UpdateVectorsAndRecipTerms(true);
}

void Ewald::UpdateVectorsAndRecipTerms(bool output)
{
  for (uint b = 0; b < BOXES_WITH_U_NB; ++b) {
    RecipInit(b, currentAxes);
    BoxReciprocalSetup(b, currentCoords);
    SetRecipRef(b);
    if(output) {
      printf("Box: %d, RecipVectors: %6d, kmax: %d\n", b, imageSize[b], kmax[b]);
    }
  }
}

void Ewald::AllocMem()
{
  //get size of image using defined Kmax
  //Allocate Memory
  kmax = new uint[BOXES_WITH_U_NB];
  imageSize = new uint[BOXES_WITH_U_NB];
  imageSizeRef = new uint[BOXES_WITH_U_NB];
  sumRnew = new double*[BOXES_WITH_U_NB];
  sumInew = new double*[BOXES_WITH_U_NB];
  sumRref = new double*[BOXES_WITH_U_NB];
  sumIref = new double*[BOXES_WITH_U_NB];
  kx = new double*[BOXES_WITH_U_NB];
  ky = new double*[BOXES_WITH_U_NB];
  kz = new double*[BOXES_WITH_U_NB];
  hsqr = new double*[BOXES_WITH_U_NB];
  prefact = new double*[BOXES_WITH_U_NB];
  kxRef = new double*[BOXES_WITH_U_NB];
  kyRef = new double*[BOXES_WITH_U_NB];
  kzRef = new double*[BOXES_WITH_U_NB];
  hsqrRef = new double*[BOXES_WITH_U_NB];
  prefactRef = new double*[BOXES_WITH_U_NB];

  for(uint b = 0; b < BOXES_WITH_U_NB; b++) {
    RecipCountInit(b, currentAxes);
  }
  //25% larger than original box size, reserved for image size change
  imageTotal = findLargeImage();

  for(uint b = 0; b < BOXES_WITH_U_NB; b++) {
    kx[b] = new double[imageTotal];
    ky[b] = new double[imageTotal];
    kz[b] = new double[imageTotal];
    hsqr[b] = new double[imageTotal];
    prefact[b] = new double[imageTotal];
    kxRef[b] = new double[imageTotal];
    kyRef[b] = new double[imageTotal];
    kzRef[b] = new double[imageTotal];
    hsqrRef[b] = new double[imageTotal];
    prefactRef[b] = new double[imageTotal];
    sumRnew[b] = new double[imageTotal];
    sumInew[b] = new double[imageTotal];
    sumRref[b] = new double[imageTotal];
    sumIref[b] = new double[imageTotal];
  }

#ifdef GOMC_CUDA
  InitEwaldVariablesCUDA(ff.particles->getCUDAVars(), imageTotal);
#endif
}


//calculate reciprocal term for a box
void Ewald::BoxReciprocalSetup(uint box, XYZArray const& molCoords)
{
  if (box < BOXES_WITH_U_NB) {
    MoleculeLookup::box_iterator end = molLookup.BoxEnd(box);
    MoleculeLookup::box_iterator thisMol = molLookup.BoxBegin(box);

#ifdef GOMC_CUDA
    int numberOfAtoms = 0, i = 0;

    for(int k = 0; k < mols.GetKindsCount(); k++) {
      MoleculeKind const& thisKind = mols.kinds[k];
      numberOfAtoms += thisKind.NumAtoms() * molLookup.NumKindInBox(k, box);
    }

    XYZArray thisBoxCoords(numberOfAtoms);
    std::vector<double> chargeBox;

    while (thisMol != end) {
      MoleculeKind const& thisKind = mols.GetKind(*thisMol);
      double lambdaCoef = GetLambdaCoef(*thisMol, box);
      for (uint j = 0; j < thisKind.NumAtoms(); j++) {
        thisBoxCoords.Set(i, molCoords[mols.MolStart(*thisMol) + j]);
        chargeBox.push_back(thisKind.AtomCharge(j) * lambdaCoef);
        i++;
      }
      thisMol++;
    }
    CallBoxReciprocalSetupGPU(ff.particles->getCUDAVars(),
                              thisBoxCoords, kx[box], ky[box], kz[box],
                              chargeBox, imageSize[box], sumRnew[box],
                              sumInew[box], prefact[box], hsqr[box],
                              currentEnergyRecip[box], box);
#else
#ifdef _OPENMP
    #pragma omp parallel default(none) shared(box)
#endif
    {
      std::memset(sumRnew[box], 0.0, sizeof(double) * imageSize[box]);
      std::memset(sumInew[box], 0.0, sizeof(double) * imageSize[box]);
    }

    while (thisMol != end) {
      MoleculeKind const& thisKind = mols.GetKind(*thisMol);
      double lambdaCoef = GetLambdaCoef(*thisMol, box);
      uint start = mols.MolStart(*thisMol);

#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(box, lambdaCoef, molCoords, start, thisKind)
#endif
      for (int i = 0; i < imageSize[box]; i++) {
        double sumReal = 0.0;
        double sumImaginary = 0.0;

        for (uint j = 0; j < thisKind.NumAtoms(); j++) {
          unsigned long currentAtom = start + j;
          if(particleHasNoCharge[currentAtom]) {
            continue;
          }
          double dotProduct = Dot(currentAtom, kx[box][i], ky[box][i],
                                  kz[box][i], molCoords);

          // TODO: sincos() can be used to optimize (GNU compiler only)
          // Windows doesn't have sincos() function and
          // Intel compiler automatically optimizes this part
          sumReal += (thisKind.AtomCharge(j) * cos(dotProduct));
          sumImaginary += (thisKind.AtomCharge(j) * sin(dotProduct));
        }
        //we assume all atom charges are scaled with lambda
        sumRnew[box][i] += (lambdaCoef * sumReal);
        sumInew[box][i] += (lambdaCoef * sumImaginary);
      }
      thisMol++;
    }
#endif
  }
}


//calculate reciprocal term for a box
double Ewald::BoxReciprocal(uint box) const
{
  double energyRecip = 0.0;

  if (box < BOXES_WITH_U_NB) {
#ifdef GOMC_CUDA
    return currentEnergyRecip[box];
#else
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(box) reduction(+:energyRecip)
#endif
    for (int i = 0; i < imageSize[box]; i++) {
      energyRecip += (( sumRnew[box][i] * sumRnew[box][i] +
                        sumInew[box][i] * sumInew[box][i]) *
                      prefact[box][i]);
    }
#endif
  }

  return energyRecip;
}


//calculate reciprocal term for displacement and rotation move
double Ewald::MolReciprocal(XYZArray const& molCoords,
                            const uint molIndex, const uint box)
{
  double energyRecipNew = 0.0;
  double energyRecipOld = 0.0;

  if (box < BOXES_WITH_U_NB) {
    MoleculeKind const& thisKind = mols.GetKind(molIndex);
    uint length = thisKind.NumAtoms();
    uint startAtom = mols.MolStart(molIndex);
    double lambdaCoef = GetLambdaCoef(molIndex, box);
#ifdef GOMC_CUDA
    XYZArray cCoords(length);
    std::vector<double> MolCharge;
    for(uint p = 0; p < length; p++) {
      cCoords.Set(p, currentCoords[startAtom + p]);
      MolCharge.push_back(thisKind.AtomCharge(p) * lambdaCoef);
    }
    CallMolReciprocalGPU(ff.particles->getCUDAVars(),
                         cCoords, molCoords, MolCharge, imageSizeRef[box],
                         sumRnew[box], sumInew[box], energyRecipNew, box);
#else
#ifdef _OPENMP
#if GCC_VERSION >= 90000
    #pragma omp parallel for default(none) shared(lambdaCoef, length, molCoords, \
    startAtom, thisKind, box) \
reduction(+:energyRecipNew)
#else
    #pragma omp parallel for default(none) shared(lambdaCoef, length, molCoords, \
    startAtom, thisKind) \
reduction(+:energyRecipNew)
#endif
#endif
    for (int i = 0; i < imageSizeRef[box]; i++) {
      double sumRealNew = 0.0;
      double sumImaginaryNew = 0.0;
      double sumRealOld = 0.0;
      double sumImaginaryOld = 0.0;

      for (uint p = 0; p < length; ++p) {
        uint currentAtom = startAtom + p;
        if(particleHasNoCharge[currentAtom]) {
          continue;
        }
        double dotProductNew = Dot(p, kxRef[box][i], kyRef[box][i], kzRef[box][i], molCoords);
        double dotProductOld = Dot(currentAtom, kxRef[box][i], kyRef[box][i], kzRef[box][i], currentCoords);

        sumRealNew += (thisKind.AtomCharge(p) * cos(dotProductNew));
        sumImaginaryNew += (thisKind.AtomCharge(p) * sin(dotProductNew));

        sumRealOld += (thisKind.AtomCharge(p) * cos(dotProductOld));
        sumImaginaryOld += (thisKind.AtomCharge(p) * sin(dotProductOld));
      }

      sumRnew[box][i] = sumRref[box][i] + lambdaCoef *
                        (sumRealNew - sumRealOld);
      sumInew[box][i] = sumIref[box][i] + lambdaCoef *
                        (sumImaginaryNew - sumImaginaryOld);

      energyRecipNew += (sumRnew[box][i] * sumRnew[box][i] + sumInew[box][i]
                         * sumInew[box][i]) * prefactRef[box][i];
    }
#endif
    energyRecipOld = sysPotRef.boxEnergy[box].recip;
  }
  return energyRecipNew - energyRecipOld;
}



//calculate reciprocal term in destination box for swap move
//No need to scale the charge with lambda, since this function will not be
// called in free energy of CFCMC
double Ewald::SwapDestRecip(const cbmc::TrialMol &newMol,
                            const uint box,
                            const int molIndex)
{
  double energyRecipNew = 0.0;
  double energyRecipOld = 0.0;

  if (box < BOXES_WITH_U_NB) {
    MoleculeKind const& thisKind = newMol.GetKind();
    XYZArray molCoords = newMol.GetCoords();
    uint length = thisKind.NumAtoms();
    uint startAtom = mols.MolStart(molIndex);
#ifdef GOMC_CUDA
    bool insert = true;
    std::vector<double> MolCharge;
    for(uint p = 0; p < length; p++) {
      MolCharge.push_back(thisKind.AtomCharge(p));
    }
    CallSwapReciprocalGPU(ff.particles->getCUDAVars(),
                          molCoords, MolCharge, imageSizeRef[box],
                          sumRnew[box], sumInew[box],
                          insert, energyRecipNew, box);
#else
#ifdef _OPENMP
#if GCC_VERSION >= 90000
    #pragma omp parallel for default(none) shared(length, molCoords, startAtom, \
thisKind, box) reduction(+:energyRecipNew)
#else
    #pragma omp parallel for default(none) shared(length, molCoords, startAtom, \
thisKind) reduction(+:energyRecipNew)
#endif
#endif
    for (int i = 0; i < imageSizeRef[box]; i++) {
      double sumRealNew = 0.0;
      double sumImaginaryNew = 0.0;

      for (uint p = 0; p < length; ++p) {
        if(particleHasNoCharge[startAtom + p]) {
          continue;
        }
        double dotProductNew = Dot(p, kxRef[box][i],
                                   kyRef[box][i], kzRef[box][i],
                                   molCoords);

        sumRealNew += (thisKind.AtomCharge(p) * cos(dotProductNew));
        sumImaginaryNew += (thisKind.AtomCharge(p) * sin(dotProductNew));
      }

      sumRnew[box][i] = sumRref[box][i] + sumRealNew;
      sumInew[box][i] = sumIref[box][i] + sumImaginaryNew;

      energyRecipNew += (sumRnew[box][i] * sumRnew[box][i] + sumInew[box][i]
                         * sumInew[box][i]) * prefactRef[box][i];
    }
#endif
    energyRecipOld = sysPotRef.boxEnergy[box].recip;
  }

  return energyRecipNew - energyRecipOld;
}

//calculate reciprocal term for lambdaNew and Old with same coordinates
double Ewald::CFCMCRecip(XYZArray const& molCoords, const double lambdaOld,
                         const double lambdaNew, const uint molIndex,
                         const uint box)
{
  double energyRecipNew = 0.0;
  double energyRecipOld = 0.0;

  //
  //Need to implement the GPU part
  //
  if (box < BOXES_WITH_U_NB) {
    MoleculeKind const& thisKind = mols.GetKind(molIndex);
    uint length = thisKind.NumAtoms();
    uint startAtom = mols.MolStart(molIndex);
    double lambdaCoef = sqrt(lambdaNew) - sqrt(lambdaOld);

#ifdef _OPENMP
#if GCC_VERSION >= 90000
    #pragma omp parallel for default(none) shared(lambdaCoef, length, molCoords, \
    startAtom, thisKind, box) \
reduction(+:energyRecipNew)
#else
    #pragma omp parallel for default(none) shared(lambdaCoef, length, molCoords, \
    startAtom, thisKind) \
reduction(+:energyRecipNew)
#endif
#endif
    for (int i = 0; i < imageSizeRef[box]; i++) {
      double sumRealNew = 0.0;
      double sumImaginaryNew = 0.0;

      for (uint p = 0; p < length; ++p) {
        if(particleHasNoCharge[startAtom + p]) {
          continue;
        }
        double dotProductNew = Dot(p, kxRef[box][i],
                                   kyRef[box][i], kzRef[box][i],
                                   molCoords);

        sumRealNew += thisKind.AtomCharge(p) * cos(dotProductNew);
        sumImaginaryNew += thisKind.AtomCharge(p) * sin(dotProductNew);
      }

      //sumRealNew;
      sumRnew[box][i] = sumRref[box][i] + lambdaCoef * sumRealNew;
      //sumImaginaryNew;
      sumInew[box][i] = sumIref[box][i] + lambdaCoef * sumImaginaryNew;

      energyRecipNew += (sumRnew[box][i] * sumRnew[box][i] + sumInew[box][i]
                         * sumInew[box][i]) * prefactRef[box][i];
    }
    energyRecipOld = sysPotRef.boxEnergy[box].recip;
  }

  return energyRecipNew - energyRecipOld;
}

//calculate reciprocal term for lambdaNew and Old with same coordinates
//used in free energy calculation
void Ewald::ChangeRecip(Energy *energyDiff, Energy &dUdL_Coul,
                        const std::vector<double> &lambda_Coul,
                        const uint iState, const uint molIndex,
                        const uint box) const
{
  //Need to implement GPU
  uint length = mols.GetKind(molIndex).NumAtoms();
  uint startAtom = mols.MolStart(molIndex);
  uint lambdaSize = lambda_Coul.size();
  double *energyRecip = new double [lambdaSize];
  std::fill_n(energyRecip, lambdaSize, 0.0);

#if defined _OPENMP && _OPENMP >= 201511 // check if OpenMP version is 4.5
#if GCC_VERSION >= 90000
  #pragma omp parallel for default(none) shared(lambda_Coul, lambdaSize, \
  length, startAtom, box, iState) \
reduction(+:energyRecip[:lambdaSize])
#else
  #pragma omp parallel for default(none) shared(lambda_Coul, lambdaSize, \
  length, startAtom) \
reduction(+:energyRecip[:lambdaSize])
#endif
#endif
  for (uint i = 0; i < imageSizeRef[box]; i++) {
    double sumReal = 0.0;
    double sumImaginary = 0.0;

    for (uint p = 0; p < length; ++p) {
      unsigned long currentAtom = startAtom + p;
      if(particleHasNoCharge[currentAtom]) {
        continue;
      }
      double dotProduct = Dot(p + startAtom, kxRef[box][i], kyRef[box][i], kzRef[box][i],
                              currentCoords);
      sumReal += particleCharge[currentAtom] * cos(dotProduct);
      sumImaginary += particleCharge[currentAtom] * sin(dotProduct);
    }
    for(uint s = 0; s < lambdaSize; s++) {
      //Calculate the energy of other state
      double coefDiff = sqrt(lambda_Coul[s]) - sqrt(lambda_Coul[iState]);
      energyRecip[s] += prefactRef[box][i] *
                        ((sumRref[box][i] + coefDiff * sumReal) *
                         (sumRref[box][i] + coefDiff * sumReal) +
                         (sumIref[box][i] + coefDiff * sumImaginary) *
                         (sumIref[box][i] + coefDiff * sumImaginary));
    }
  }

  double energyRecipOld = sysPotRef.boxEnergy[box].recip;
  for(uint s = 0; s < lambdaSize; s++) {
  energyDiff[s].recip = energyRecip[s] - energyRecipOld;
  }
  //Calculate du/dl of Reciprocal for current state  with linear scaling
  //energy difference E(lambda =1) - E(lambda = 0)
  dUdL_Coul.recip += energyDiff[lambdaSize - 1].recip - energyDiff[0].recip;
  delete [] energyRecip;
}

void Ewald::RecipInit(uint box, BoxDimensions const& boxAxes)
{
  if(boxAxes.orthogonal[box])
    RecipInitOrth(box, boxAxes);
  else
    RecipInitNonOrth(box, boxAxes);
}


//calculate reciprocal term in source box for swap move
//No need to scale the charge with lambda, since this function is not being
// called for free energy and CFCMC
double Ewald::SwapSourceRecip(const cbmc::TrialMol &oldMol,
                              const uint box, const int molIndex)
{
  double energyRecipNew = 0.0;
  double energyRecipOld = 0.0;

  if (box < BOXES_WITH_U_NB) {
    MoleculeKind const& thisKind = oldMol.GetKind();
    XYZArray molCoords = oldMol.GetCoords();
    uint length = thisKind.NumAtoms();
    uint startAtom = mols.MolStart(molIndex);
#ifdef GOMC_CUDA
    bool insert = false;
    std::vector<double> MolCharge;
    for(uint p = 0; p < length; p++) {
      MolCharge.push_back(thisKind.AtomCharge(p));
    }
    CallSwapReciprocalGPU(ff.particles->getCUDAVars(),
                          molCoords, MolCharge, imageSizeRef[box],
                          sumRnew[box], sumInew[box],
                          insert, energyRecipNew, box);

#else
#ifdef _OPENMP
#if GCC_VERSION >= 90000
    #pragma omp parallel for default(none) shared(length, molCoords, startAtom, \
    thisKind, box) \
reduction(+:energyRecipNew)
#else
    #pragma omp parallel for default(none) shared(length, molCoords, startAtom, \
    thisKind) \
reduction(+:energyRecipNew)
#endif
#endif
    for (int i = 0; i < imageSizeRef[box]; i++) {
      double sumRealNew = 0.0;
      double sumImaginaryNew = 0.0;

      for (uint p = 0; p < length; ++p) {
        unsigned long currentAtom = startAtom + p;
        if(particleHasNoCharge[currentAtom]) {
          continue;
        }
        double dotProductNew = Dot(p, kxRef[box][i],
                                   kyRef[box][i], kzRef[box][i],
                                   molCoords);

        sumRealNew += (thisKind.AtomCharge(p) * cos(dotProductNew));
        sumImaginaryNew += (thisKind.AtomCharge(p) * sin(dotProductNew));

      }
      sumRnew[box][i] = sumRref[box][i] - sumRealNew;
      sumInew[box][i] = sumIref[box][i] - sumImaginaryNew;

      energyRecipNew += (sumRnew[box][i] * sumRnew[box][i] + sumInew[box][i]
                         * sumInew[box][i]) * prefactRef[box][i];
    }
#endif
    energyRecipOld = sysPotRef.boxEnergy[box].recip;
  }
  return energyRecipNew - energyRecipOld;
}


//calculate reciprocal term for inserting some molecules (kindA) in destination
// box and removing molecule (kindB) from destination box
double Ewald::SwapRecip(const std::vector<cbmc::TrialMol> &newMol,
                        const std::vector<cbmc::TrialMol> &oldMol,
                        const std::vector<uint> molIndexNew,
                        const std::vector<uint> molIndexOld,
                        bool first_call)
{
  double energyRecipNew = 0.0;
  double energyRecipOld = 0.0;
  //Change in reciprocal happens in the same box.
  uint box = newMol[0].GetBox();

  if (box < BOXES_WITH_U_NB) {
    uint lengthNew, lengthOld;
    MoleculeKind const& thisKindNew = newMol[0].GetKind();
    MoleculeKind const& thisKindOld = oldMol[0].GetKind();
    lengthNew = thisKindNew.NumAtoms();
    lengthOld = thisKindOld.NumAtoms();

#ifdef _OPENMP
#if GCC_VERSION >= 90000
    #pragma omp parallel for default(none) shared(box, first_call, lengthNew, lengthOld, \
    newMol, oldMol, thisKindNew, thisKindOld, molIndexNew, molIndexOld) \
reduction(+:energyRecipNew)
#else
    #pragma omp parallel for default(none) shared(box, first_call, lengthNew, lengthOld, \
    newMol, oldMol, thisKindNew, thisKindOld) \
reduction(+:energyRecipNew)
#endif
#endif
    for (int i = 0; i < imageSizeRef[box]; i++) {
      double sumRealNew = 0.0;
      double sumImaginaryNew = 0.0;

      // Add dot sum of the new molecule
      for (uint m = 0; m < newMol.size(); m++) {
        uint newMoleculeIndex = molIndexNew[m];
        double lambdaCoef = GetLambdaCoef(newMoleculeIndex, box);
        for (uint p = 0; p < lengthNew; ++p) {
          unsigned long currentAtom = mols.MolStart(newMoleculeIndex) + p;
          if(particleHasNoCharge[currentAtom]) {
            continue;
          }
          double dotProductNew = Dot(p, kxRef[box][i], kyRef[box][i],
                                     kzRef[box][i], newMol[m].GetCoords());

          // TODO: Using GNU extension we could improve this part of the code
          // by using sincos() function and merge sin() and cos() calculation
          // However, this will not work with Visual studio
          // Intel should automatically optimize this section by using
          // internal functions like __svml_sincosf8..()
          sumRealNew += (thisKindNew.AtomCharge(p) * lambdaCoef *
                         cos(dotProductNew));
          sumImaginaryNew += (thisKindNew.AtomCharge(p) * lambdaCoef *
                              sin(dotProductNew));
        }
      }

      // Subtract the sum of the old molecule
      for (uint m = 0; m < oldMol.size(); m++) {
        uint oldMoleculeIndex = molIndexOld[m];
        double lambdaCoef = GetLambdaCoef(oldMoleculeIndex, box);
        for (uint p = 0; p < lengthOld; ++p) {
          unsigned long currentAtom = mols.MolStart(oldMoleculeIndex) + p;
          if(particleHasNoCharge[currentAtom]) {
            continue;
          }
          double dotProductOld = Dot(p, kxRef[box][i], kyRef[box][i],
                                     kzRef[box][i], oldMol[m].GetCoords());

          // TODO: Using GNU extension we could improve this part of the code
          // by using sincos() function and merge sin() and cos() calculation
          // However, this will not work with Visual studio
          // Intel should automatically optimize this section by using
          // internal functions like __svml_sincosf8..()
          sumRealNew -= thisKindOld.AtomCharge(p) * lambdaCoef *
                        cos(dotProductOld);
          sumImaginaryNew -= thisKindOld.AtomCharge(p) * lambdaCoef *
                             sin(dotProductOld);
        }
      }

      // Update the new sum value based on the difference and previous sum
      // If this is the first call to this function within the same pair of molecules,
      // then use the ref variable to update new
      // However, if this is the second time calling it then use the previous result as reference
      if (first_call) {
        sumRnew[box][i] = sumRref[box][i] + sumRealNew;
        sumInew[box][i] = sumIref[box][i] + sumImaginaryNew;
      } else {
        sumRnew[box][i] += sumRealNew;
        sumInew[box][i] += sumImaginaryNew;
      }

      // Calculate new energy recip based on the new sum real and imaginary values
      energyRecipNew += (sumRnew[box][i] * sumRnew[box][i] + sumInew[box][i]
                         * sumInew[box][i]) * prefactRef[box][i];
    }

    // Keep hold of the old recip value
    energyRecipOld = sysPotRef.boxEnergy[box].recip;
  }

  // Return the difference between old and new reciprocal energies
  return energyRecipNew - energyRecipOld;
}

//restore cosMol and sinMol
void Ewald::RestoreMol(int molIndex)
{
  return;
}

//restore the whole cosMolRef & sinMolRef into cosMolBoxRecip & sinMolBoxRecip
void Ewald::exgMolCache()
{
  return;
}

//compare number of images in different boxes and select the largest one
uint Ewald::findLargeImage()
{
  uint maxImg = 0;
  for (int b = 0; b < BOXES_WITH_U_NB; b++) {
    if (maxImg < imageSize[b])
      maxImg = imageSize[b];
  }
  return maxImg;
}

//backup the whole cosMolRef & sinMolRef into cosMolBoxRecip & sinMolBoxRecip
void Ewald::backupMolCache()
{
  return;
}

void Ewald::RecipInitOrth(uint box, BoxDimensions const& boxAxes)
{
  uint counter = 0;
  int x, y, z, nkx_max, nky_max, nky_min, nkz_max, nkz_min;
  double ksqr, kX, kY, kZ;
  double alpsqr4 = 1.0 / (4.0 * ff.alphaSq[box]);
  XYZ constValue = boxAxes.axis.Get(box);
  constValue.Inverse();
  constValue *= 2 * M_PI;

  double vol = boxAxes.volume[box] / (4 * M_PI);
  nkx_max = int(ff.recip_rcut[box] * boxAxes.axis.Get(box).x / (2 * M_PI)) + 1;
  nky_max = int(ff.recip_rcut[box] * boxAxes.axis.Get(box).y / (2 * M_PI)) + 1;
  nkz_max = int(ff.recip_rcut[box] * boxAxes.axis.Get(box).z / (2 * M_PI)) + 1;
  kmax[box] = std::max(std::max(nkx_max, nky_max), std::max(nky_max, nkz_max));

  for(x = 0; x <= nkx_max; x++) {
    if(x == 0.0)
      nky_min = 0;
    else
      nky_min = -nky_max;

    for(y = nky_min; y <= nky_max; y++) {
      if(x == 0.0 && y == 0.0)
        nkz_min = 1;
      else
        nkz_min = -nkz_max;

      for(z = nkz_min; z <= nkz_max; z++) {
        kX = constValue.x * x;
        kY = constValue.y * y;
        kZ = constValue.z * z;
        ksqr = kX * kX + kY * kY + kZ * kZ;

        if(ksqr < ff.recip_rcut_Sq[box]) {
          kx[box][counter] = kX;
          ky[box][counter] = kY;
          kz[box][counter] = kZ;
          hsqr[box][counter] = ksqr;
          prefact[box][counter] = num::qqFact * exp(-ksqr * alpsqr4) /
                                  (ksqr * vol);
          counter++;
        }
      }
    }
  }

  imageSize[box] = counter;

  if (counter > imageTotal) {
    std::cout << "Error: Kmax exceeded due to large change in system volume.\n";
    std::cout << "Restart the simulation from restart files.\n";
    exit(EXIT_FAILURE);
  }
}

void Ewald::RecipInitNonOrth(uint box, BoxDimensions const& boxAxes)
{
  uint counter = 0;
  int x, y, z, nkx_max, nky_max, nky_min, nkz_max, nkz_min;
  double ksqr, kX, kY, kZ;
  double alpsqr4 = 1.0 / (4.0 * ff.alphaSq[box]);
  XYZArray cellB(boxAxes.cellBasis[box]);
  cellB.Scale(0, boxAxes.axis.Get(box).x);
  cellB.Scale(1, boxAxes.axis.Get(box).y);
  cellB.Scale(2, boxAxes.axis.Get(box).z);
  XYZArray cellB_Inv(3);
  double det = cellB.AdjointMatrix(cellB_Inv);
  cellB_Inv.ScaleRange(0, 3, (2 * M_PI) / det);

  double vol = boxAxes.volume[box] / (4 * M_PI);
  nkx_max = int(ff.recip_rcut[box] * boxAxes.axis.Get(box).x / (2 * M_PI)) + 1;
  nky_max = int(ff.recip_rcut[box] * boxAxes.axis.Get(box).y / (2 * M_PI)) + 1;
  nkz_max = int(ff.recip_rcut[box] * boxAxes.axis.Get(box).z / (2 * M_PI)) + 1;
  kmax[box] = std::max(std::max(nkx_max, nky_max), std::max(nky_max, nkz_max));

  for (x = 0; x <= nkx_max; x++) {
    if(x == 0.0)
      nky_min = 0;
    else
      nky_min = -nky_max;

    for(y = nky_min; y <= nky_max; y++) {
      if(x == 0.0 && y == 0.0)
        nkz_min = 1;
      else
        nkz_min = -nkz_max;

      for(z = nkz_min; z <= nkz_max; z++) {
        kX = Dot(cellB_Inv.Get(0), XYZ(x, y, z));
        kY = Dot(cellB_Inv.Get(1), XYZ(x, y, z));
        kZ = Dot(cellB_Inv.Get(2), XYZ(x, y, z));
        ksqr = kX * kX + kY * kY + kZ * kZ;

        if(ksqr < ff.recip_rcut_Sq[box]) {
          kx[box][counter] = kX;
          ky[box][counter] = kY;
          kz[box][counter] = kZ;
          hsqr[box][counter] = ksqr;
          prefact[box][counter] = num::qqFact * exp(-ksqr * alpsqr4) /
                                  (ksqr * vol);
          counter++;
        }
      }
    }
  }

  imageSize[box] = counter;

  if(counter > imageTotal) {
    std::cout << "Error: Kmax exceeded due to large change in system volume.\n";
    std::cout << "Restart the simulation from restart files.\n";
    exit(EXIT_FAILURE);
  }
}

//estimate number of vectors
void Ewald::RecipCountInit(uint box, BoxDimensions const& boxAxes)
{
  uint counter = 0;
  int x, y, z, nkx_max, nky_max, nky_min, nkz_max, nkz_min;
  double ksqr, excess, kX, kY, kZ;
  XYZArray cellB(boxAxes.cellBasis[box]);
  XYZ constValue = boxAxes.axis.Get(box);
#if ENSEMBLE == GEMC
  excess = 1.25;
#elif ENSEMBLE == NPT
  excess = 1.5;
#else
  excess = 1.00;
#endif
  constValue *= excess;
  cellB.Scale(0, constValue.x);
  cellB.Scale(1, constValue.y);
  cellB.Scale(2, constValue.z);
  XYZArray cellB_Inv(3);
  double det = cellB.AdjointMatrix(cellB_Inv);
  cellB_Inv.ScaleRange(0, 3, (2 * M_PI) / det);

  nkx_max = int(ff.recip_rcut[box] * constValue.x / (2 * M_PI)) + 1;
  nky_max = int(ff.recip_rcut[box] * constValue.y / (2 * M_PI)) + 1;
  nkz_max = int(ff.recip_rcut[box] * constValue.z / (2 * M_PI)) + 1;

  for(x = 0; x <= nkx_max; x++) {
    if(x == 0.0)
      nky_min = 0;
    else
      nky_min = -nky_max;

    for(y = nky_min; y <= nky_max; y++) {
      if(x == 0.0 && y == 0.0)
        nkz_min = 1;
      else
        nkz_min = -nkz_max;

      for(z = nkz_min; z <= nkz_max; z++) {
        kX = Dot(cellB_Inv.Get(0), XYZ(x, y, z));
        kY = Dot(cellB_Inv.Get(1), XYZ(x, y, z));
        kZ = Dot(cellB_Inv.Get(2), XYZ(x, y, z));
        ksqr = kX * kX + kY * kY + kZ * kZ;

        if(ksqr < ff.recip_rcut_Sq[box]) {
          counter++;
        }
      }
    }
  }
  imageSize[box] = counter;
}

//back up reciprocal value to Ref (will be called during initialization)
void Ewald::SetRecipRef(uint box)
{
#ifdef _OPENMP
  #pragma omp parallel default(none) shared(box)
#endif
  {
    std::memcpy(sumRref[box], sumRnew[box], sizeof(double) * imageSize[box]);
    std::memcpy(sumIref[box], sumInew[box], sizeof(double) * imageSize[box]);
    std::memcpy(kxRef[box], kx[box], sizeof(double) * imageSize[box]);
    std::memcpy(kyRef[box], ky[box], sizeof(double) * imageSize[box]);
    std::memcpy(kzRef[box], kz[box], sizeof(double) * imageSize[box]);
    std::memcpy(hsqrRef[box], hsqr[box], sizeof(double) * imageSize[box]);
    std::memcpy(prefactRef[box], prefact[box], sizeof(double) *imageSize[box]);
  }
#ifdef GOMC_CUDA
  CopyCurrentToRefCUDA(ff.particles->getCUDAVars(),
                       box, imageSize[box]);
#endif
  for(uint b = 0; b < BOXES_WITH_U_NB; b++) {
    imageSizeRef[b] = imageSize[b];
  }
}

//calculate correction term for a molecule, with system lambda
double Ewald::MolCorrection(uint molIndex, uint box) const
{
  if (box >= BOXES_WITH_U_NB)
    return 0.0;

  double dist, distSq;
  double correction = 0.0;
  XYZ virComponents;

  MoleculeKind& thisKind = mols.kinds[mols.kIndex[molIndex]];
  uint atomSize = thisKind.NumAtoms();
  uint start = mols.MolStart(molIndex);
  double lambdaCoef = GetLambdaCoef(molIndex, box);

  for (uint i = 0; i < atomSize; i++) {
    if(particleHasNoCharge[start + i]) {
      continue;
    }
    for (uint j = i + 1; j < atomSize; j++) {
      currentAxes.InRcut(distSq, virComponents, currentCoords,
                         start + i, start + j, box);
      dist = sqrt(distSq);
      correction += (thisKind.AtomCharge(i) * thisKind.AtomCharge(j) *
                     erf(ff.alpha[box] * dist) / dist);
    }
  }

  return -1.0 * num::qqFact * correction * lambdaCoef * lambdaCoef;
}

//It's called in free energy calculation to calculate the change in
// correction energy in all lambda states
void Ewald::ChangeCorrection(Energy *energyDiff, Energy &dUdL_Coul,
                             const std::vector<double> &lambda_Coul,
                             const uint iState, const uint molIndex,
                             const uint box) const
{
  uint atomSize = mols.GetKind(molIndex).NumAtoms();
  uint start = mols.MolStart(molIndex);
  uint lambdaSize = lambda_Coul.size();
  double coefDiff, distSq, dist, correction = 0.0;
  XYZ virComponents;

  //Calculate the correction energy with lambda = 1
  for (uint i = 0; i < atomSize; i++) {
    if(particleHasNoCharge[start + i]) {
      continue;
    }

    for (uint j = i + 1; j < atomSize; j++) {
      distSq = 0.0;
      currentAxes.InRcut(distSq, virComponents, currentCoords,
                         start + i, start + j, box);
      dist = sqrt(distSq);
      correction += (particleCharge[i + start] * particleCharge[j + start] *
                     erf(ff.alpha[box] * dist) / dist);
    }
  }
  correction *= -1.0 * num::qqFact;
  //Calculate the energy difference for each lambda state
  for (uint s = 0; s < lambdaSize; s++) {
    coefDiff = lambda_Coul[s] - lambda_Coul[iState];
    energyDiff[s].correction += coefDiff * correction;
  }
  //Calculate du/dl of correction for current state, for linear scaling
  dUdL_Coul.correction += correction;
}

//calculate self term for a box, using system lambda
double Ewald::BoxSelf(BoxDimensions const& boxAxes, uint box) const
{
  if (box >= BOXES_WITH_U_NB)
    return 0.0;

  double self = 0.0;
  double molSelfEnergy;
  uint i, j, length, molNum;
  double lambdaCoef = 1.0;

  for (i = 0; i < mols.GetKindsCount(); i++) {
    MoleculeKind const& thisKind = mols.kinds[i];
    length = thisKind.NumAtoms();
    molNum = molLookup.NumKindInBox(i, box);
    molSelfEnergy = 0.0;
    if(lambdaRef.KindIsFractional(i, box)) {
      //If a molecule is fractional, we subtract the fractional molecule and
      // add it later
      --molNum;
      //returns lambda and not sqrt(lambda)
      lambdaCoef = lambdaRef.GetLambdaCoulomb(i, box);
    }

    for (j = 0; j < length; j++) {
      molSelfEnergy += (thisKind.AtomCharge(j) * thisKind.AtomCharge(j));
    }
    self += (molSelfEnergy * molNum);
    if(lambdaRef.KindIsFractional(i, box)) {
      //Add the fractional molecule part
      self += (molSelfEnergy * lambdaCoef);
    }
  }

  self = -1.0 * self * ff.alpha[box] * num::qqFact / sqrt(M_PI);

  return self;
}

// NOTE: The calculation of W12, W13, W23 is expensive and would not be
// required for pressure and surface tension calculation. So, they have been
// commented out. In case you need to calculate them, uncomment them.
Virial Ewald::VirialReciprocal(Virial& virial, uint box) const
{
  Virial tempVir = virial;
  if (box >= BOXES_WITH_U_NB)
    return tempVir;

  double wT11 = 0.0, wT12 = 0.0, wT13 = 0.0;
  double wT22 = 0.0, wT23 = 0.0, wT33 = 0.0;

  double recipIntra = 0.0;
  double constVal = 1.0 / (4.0 * ff.alphaSq[box]);
  double charge, lambdaCoef;
  uint p, length, startAtom, atom;

  MoleculeLookup::box_iterator thisMol = molLookup.BoxBegin(box),
                               end = molLookup.BoxEnd(box);

  XYZ atomC, comC, diffC;

#ifdef GOMC_CUDA
  int numberOfAtoms = 0, atomIndex = 0;

  for(int k = 0; k < mols.GetKindsCount(); k++) {
    MoleculeKind const& thisKind = mols.kinds[k];
    numberOfAtoms += thisKind.NumAtoms() * molLookup.NumKindInBox(k, box);
  }

  XYZArray thisBoxCoords(numberOfAtoms);
  XYZArray thisBoxCOMDiff(numberOfAtoms);
  std::vector<double> chargeBox;

  while (thisMol != end) {
    length = mols.GetKind(*thisMol).NumAtoms();
    startAtom = mols.MolStart(*thisMol);
    comC = currentCOM.Get(*thisMol);
    lambdaCoef = GetLambdaCoef(*thisMol, box);

    for (p = 0; p < length; p++) {
      atom = startAtom + p;
      //compute the vector of the bead to the COM (p)
      // need to unwrap the atom coordinate
      atomC = currentCoords.Get(atom);
      currentAxes.UnwrapPBC(atomC, box, comC);
      diffC = atomC - comC;

      thisBoxCoords.Set(atomIndex, atomC);
      thisBoxCOMDiff.Set(atomIndex, diffC);
      // scale the charge with lambda
      chargeBox.push_back(particleCharge[atom] * lambdaCoef);
      atomIndex++;
    }
    thisMol++;
  }

  CallVirialReciprocalGPU(ff.particles->getCUDAVars(), thisBoxCoords,
                          thisBoxCOMDiff, chargeBox, wT11, wT12,
                          wT13, wT22, wT23, wT33, imageSizeRef[box], constVal,
                          box);
#else
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(box, constVal) reduction(+:wT11, wT22, wT33)
#endif
  for (int i = 0; i < imageSizeRef[box]; i++) {
    double factor = prefactRef[box][i] * (sumRref[box][i] * sumRref[box][i] +
                                          sumIref[box][i] * sumIref[box][i]);

    wT11 += factor * (1.0 - 2.0 * (constVal + 1.0 / hsqrRef[box][i]) *
                      kxRef[box][i] * kxRef[box][i]);

    wT22 += factor * (1.0 - 2.0 * (constVal + 1.0 / hsqrRef[box][i]) *
                      kyRef[box][i] * kyRef[box][i]);

    wT33 += factor * (1.0 - 2.0 * (constVal + 1.0 / hsqrRef[box][i]) *
                      kzRef[box][i] * kzRef[box][i]);
  }

  //Intramolecular part
  while (thisMol != end) {
    length = mols.GetKind(*thisMol).NumAtoms();
    startAtom = mols.MolStart(*thisMol);
    comC = currentCOM.Get(*thisMol);
    lambdaCoef = GetLambdaCoef(*thisMol, box);

    for (p = 0; p < length; p++) {
      atom = startAtom + p;
      if(particleHasNoCharge[atom]) {
        continue;
      }
      //compute the vector of the bead to the COM (p)
      // need to unwrap the atom coordinate
      atomC = currentCoords.Get(atom);
      currentAxes.UnwrapPBC(atomC, box, comC);
      diffC = atomC - comC;
      //scale the charge with lambda for Free energy calc
      charge = particleCharge[atom] * lambdaCoef;

#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(atom, box, charge, diffC) reduction(+:wT11, wT22, wT33)
#endif
      for (int i = 0; i < imageSizeRef[box]; i++) {
        //compute the dot product of k and r
        double arg = Dot(atom, kxRef[box][i], kyRef[box][i],
                         kzRef[box][i], currentCoords);

        double factor = prefactRef[box][i] * 2.0 * (sumIref[box][i] * cos(arg) -
                        sumRref[box][i] * sin(arg)) * charge;

        wT11 += factor * (kxRef[box][i] * diffC.x);

        wT22 += factor * (kyRef[box][i] * diffC.y);

        wT33 += factor * (kzRef[box][i] * diffC.z);
      }
    }
    ++thisMol;
  }
#endif

  // set the all tensor values
  tempVir.recipTens[0][0] = wT11;
  tempVir.recipTens[0][1] = wT12;
  tempVir.recipTens[0][2] = wT13;

  tempVir.recipTens[1][0] = wT12;
  tempVir.recipTens[1][1] = wT22;
  tempVir.recipTens[1][2] = wT23;

  tempVir.recipTens[2][0] = wT13;
  tempVir.recipTens[2][1] = wT23;
  tempVir.recipTens[2][2] = wT33;

  // setting virial of reciprocal space
  tempVir.recip = wT11 + wT22 + wT33;

  return tempVir;
}

//calculate correction term for a molecule with lambda = 1
//It's called when the molecule configuration changes, moleculeTransfer, MEMC
//It never been called in Free Energy calculation, because we are in
// NVT and NPT ensemble
double Ewald::SwapCorrection(const cbmc::TrialMol& trialMol) const
{
  uint box = trialMol.GetBox();
  if (box >= BOXES_WITH_U_NB)
    return 0.0;

  double dist, distSq;
  double correction = 0.0;
  XYZ virComponents;
  const MoleculeKind& thisKind = trialMol.GetKind();
  uint atomSize = thisKind.NumAtoms();

  for (uint i = 0; i < atomSize; i++) {
    for (uint j = i + 1; j < atomSize; j++) {
      currentAxes.InRcut(distSq, virComponents, trialMol.GetCoords(),
                         i, j, box);

      dist = sqrt(distSq);
      correction -= (thisKind.AtomCharge(i) * thisKind.AtomCharge(j) *
                     erf(ff.alpha[box] * dist) / dist);
    }
  }
  return num::qqFact * correction;
}

//calculate correction term for a molecule with system lambda
//It's called when the molecule configuration changes, regrowth, crankshaft, IntraSwap, IntraMEMC ...
double Ewald::SwapCorrection(const cbmc::TrialMol& trialMol,
                             const uint molIndex) const
{
  uint box = trialMol.GetBox();
  if (box >= BOXES_WITH_U_NB)
    return 0.0;

  double dist, distSq;
  double correction = 0.0;
  XYZ virComponents;
  const MoleculeKind& thisKind = trialMol.GetKind();
  uint atomSize = thisKind.NumAtoms();
  uint start = mols.MolStart(molIndex);
  double lambdaCoef = GetLambdaCoef(molIndex, box);

  for (uint i = 0; i < atomSize; i++) {
    if(particleHasNoCharge[start + i]) {
      continue;
    }
    for (uint j = i + 1; j < atomSize; j++) {
      currentAxes.InRcut(distSq, virComponents, trialMol.GetCoords(),
                         i, j, box);

      dist = sqrt(distSq);
      correction -= (thisKind.AtomCharge(i) * thisKind.AtomCharge(j) *
                     erf(ff.alpha[box] * dist) / dist);
    }
  }
  return num::qqFact * correction * lambdaCoef * lambdaCoef;
}

//It's called if we transfer one molecule from one box to another
//No need to scale the charge with lambda, since this function is not being
// called from free energy or CFCMC
double Ewald::SwapSelf(const cbmc::TrialMol& trialMol) const
{
  uint box = trialMol.GetBox();
  if (box >= BOXES_WITH_U_NB)
    return 0.0;

  MoleculeKind const& thisKind = trialMol.GetKind();
  uint atomSize = thisKind.NumAtoms();
  double en_self = 0.0;

  for (uint i = 0; i < atomSize; i++) {
    en_self -= (thisKind.AtomCharge(i) * thisKind.AtomCharge(i));
  }
  return (en_self * ff.alpha[box] * num::qqFact / sqrt(M_PI));
}

//It's called in free energy calculation to calculate the change in
// self energy in all lambda states
void Ewald::ChangeSelf(Energy *energyDiff, Energy &dUdL_Coul,
                       const std::vector<double> &lambda_Coul,
                       const uint iState, const uint molIndex,
                       const uint box) const
{
  uint atomSize = mols.GetKind(molIndex).NumAtoms();
  uint start = mols.MolStart(molIndex);
  uint lambdaSize = lambda_Coul.size();
  double coefDiff, en_self = 0.0;
  //Calculate the self energy with lambda = 1
  for (uint i = 0; i < atomSize; i++) {
    en_self += (particleCharge[i + start] * particleCharge[i + start]);
  }
  en_self *= -1.0 * ff.alpha[box] * num::qqFact / sqrt(M_PI);

  //Calculate the energy difference for each lambda state
  for (uint s = 0; s < lambdaSize; s++) {
    coefDiff = lambda_Coul[s] - lambda_Coul[iState];
    energyDiff[s].self += coefDiff * en_self;
  }
  //Calculate du/dl of self for current state, for linear scaling
  dUdL_Coul.self += en_self;
}

//update reciprocal values
void Ewald::UpdateRecip(uint box)
{
  if (box >= BOXES_WITH_U_NB)
    return;

  double* tempR, * tempI;
  tempR = sumRnew[box];
  tempI = sumInew[box];
  sumRnew[box] = sumRref[box];
  sumInew[box] = sumIref[box];
  sumRref[box] = tempR;
  sumIref[box] = tempI;
#ifdef GOMC_CUDA
  UpdateRecipCUDA(ff.particles->getCUDAVars(), box);
#endif
}

void Ewald::UpdateRecipVec(uint box)
{
  double *tempKx, *tempKy, *tempKz, *tempHsqr, *tempPrefact;
  tempKx = kxRef[box];
  tempKy = kyRef[box];
  tempKz = kzRef[box];
  tempHsqr = hsqrRef[box];
  tempPrefact = prefactRef[box];

  kxRef[box] = kx[box];
  kyRef[box] = ky[box];
  kzRef[box] = kz[box];
  hsqrRef[box] = hsqr[box];
  prefactRef[box] = prefact[box];

  kx[box] = tempKx;
  ky[box] = tempKy;
  kz[box] = tempKz;
  hsqr[box] = tempHsqr;
  prefact[box] = tempPrefact;
#ifdef GOMC_CUDA
  UpdateRecipVecCUDA(ff.particles->getCUDAVars(), box);
#endif

  for(uint b = 0; b < BOXES_WITH_U_NB; b++) {
    imageSizeRef[b] = imageSize[b];
  }
}

void compareDouble(const double &x, const double &y, const int &i)
{
  if(abs(x - y) > 1e-15) {
    printf("%d: %lf != %lf\n", i, x, y);
  }
}


//calculate reciprocal force term for a box with molCoords
void Ewald::BoxForceReciprocal(XYZArray const& molCoords,
                               XYZArray& atomForceRec,
                               XYZArray& molForceRec,
                               uint box)
{
  if(multiParticleEnabled && (box < BOXES_WITH_U_NB)) {
    double constValue = 2.0 * ff.alpha[box] / sqrt(M_PI);

#ifdef GOMC_CUDA
    MoleculeLookup::box_iterator thisMol = molLookup.BoxBegin(box);
    MoleculeLookup::box_iterator end = molLookup.BoxEnd(box);
    while(thisMol != end) {
      uint molIndex = *thisMol;
      uint length = mols.GetKind(molIndex).NumAtoms();
      uint start = mols.MolStart(molIndex);
      for(int p = 0; p < length; p++) {
        atomForceRec.Set(start + p, 0.0, 0.0, 0.0);
      }
      molForceRec.Set(molIndex, 0.0, 0.0, 0.0);
      thisMol++;
    }
    // initialize the start and end of each box
    initializeBoxRange();

    CallBoxForceReciprocalGPU(
      ff.particles->getCUDAVars(),
      atomForceRec,
      molForceRec,
      particleCharge,
      particleMol,
      particleKind,
      particleHasNoCharge,
      startMol,
      lengthMol,
      ff.alpha[box],
      ff.alphaSq[box],
      num::qqFact,
      constValue,
      imageSize[box],
      molCoords,
      boxStart[box],
      boxEnd[box],
      currentAxes,
      box
    );
#else
    // molecule iterator
    MoleculeLookup::box_iterator thisMol = molLookup.BoxBegin(box);
    MoleculeLookup::box_iterator end = molLookup.BoxEnd(box);

    while(thisMol != end) {
      uint molIndex = *thisMol;
      uint length, start, p;
      double distSq;
      XYZ distVect;
      molForceRec.Set(molIndex, 0.0, 0.0, 0.0);
      length = mols.GetKind(molIndex).NumAtoms();
      start = mols.MolStart(molIndex);
      double lambdaCoef = GetLambdaCoef(molIndex, box);

      for(p = start; p < start + length; p++) {
        double X = 0.0, Y = 0.0, Z = 0.0;
        double intraForce;

        if(!particleHasNoCharge[p]) {
          // subtract the intra forces(correction)
          for(uint j = start; j < start + length; j++) {
            //no self term in force
            if(p != j) {
              currentAxes.InRcut(distSq, distVect, molCoords, p, j, box);
              double dist = sqrt(distSq);
              double expConstValue = exp(-1.0 * ff.alphaSq[box] * distSq);
              double qiqj = particleCharge[p] * particleCharge[j] * num::qqFact;
              intraForce = qiqj * lambdaCoef * lambdaCoef / distSq;
              intraForce *= ((erf(ff.alpha[box] * dist) / dist) -
                             constValue * expConstValue);
              X -= intraForce * distVect.x;
              Y -= intraForce * distVect.y;
              Z -= intraForce * distVect.z;
            }
          }
#ifdef _OPENMP
          #pragma omp parallel for default(none) shared(box, lambdaCoef, molCoords, p) reduction(+:X, Y, Z)
#endif
          for(int i = 0; i < imageSize[box]; i++) {
            double dot = Dot(p, kx[box][i], ky[box][i], kz[box][i], molCoords);

            double factor = 2.0 * particleCharge[p] * prefact[box][i] * lambdaCoef *
                            (sin(dot) * sumRnew[box][i] - cos(dot) * sumInew[box][i]);

            X += factor * kx[box][i];
            Y += factor * ky[box][i];
            Z += factor * kz[box][i];
          }
        }
        atomForceRec.Set(p, X, Y, Z);
        molForceRec.Add(molIndex, X, Y, Z);
      }
      thisMol++;
    }
#endif
  }
}

double Ewald::GetLambdaCoef(uint molA, uint box) const
{
  double lambda = lambdaRef.GetLambdaCoulomb(molA, mols.GetMolKind(molA), box);
  //Each charge gets sq root of it.
  return sqrt(lambda);
}

void Ewald::initializeBoxRange()
{
  if(BOX_TOTAL == 1) {
    boxStart[mv::BOX0] = 0;
    boxEnd[mv::BOX0] = currentCoords.Count();
  } else {
    boxStart[mv::BOX0] = 0;
    MoleculeLookup::box_iterator startBox1 = molLookup.BoxBegin(mv::BOX1);
    int firstMoleculeInBox1 = *startBox1;
    int indexOfFirstAtomInBox1 = mols.MolStart(firstMoleculeInBox1);
    boxEnd[mv::BOX0] = indexOfFirstAtomInBox1;
    boxStart[mv::BOX1] = indexOfFirstAtomInBox1;
    boxEnd[mv::BOX1] = currentCoords.Count();
  }
}
