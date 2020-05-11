/*******************************************************************************
GPU OPTIMIZED MONTE CARLO (GOMC) 2.50
Copyright (C) 2018  GOMC Group
A copy of the GNU General Public License can be found in the COPYRIGHT.txt
along with this program, also can be found at <http://www.gnu.org/licenses/>.
********************************************************************************/
#ifndef NOEWALD_H
#define NOEWALD_H

#include "Ewald.h"


class NoEwald : public Ewald
{
  //friend class CalculateEnergy;
public:

  NoEwald(StaticVals & stat, System & sys);

  virtual void Init();

  virtual void AllocMem();

  //initiliazie term used for ewald calculation
  virtual void RecipInit(uint box, BoxDimensions const& boxAxes);

  //calculate self term for a box
  virtual double BoxSelf(BoxDimensions const& boxAxes, uint box) const;

  //setup reciprocate term for a box
  virtual void BoxReciprocalSetup(uint box, XYZArray const& molCoords);

  //calculate reciprocate energy term for a box
  virtual double BoxReciprocal(uint box) const;

  //calculate reciprocate force term for a box with molCoords
  virtual double BoxForceReciprocal(SystemPotential &pot, XYZArray const& molCoords,
                                    XYZArray& atomForceRec, XYZArray& molForceRec,
                                    uint box);

  //calculate reciprocate force term for a box
  virtual Virial VirialReciprocal(Virial& virial, uint box) const;

  //calculate correction term for a molecule
  virtual double MolCorrection(uint molIndex, uint box)const;

  //calculate reciprocate term for displacement and rotation move
  virtual double MolReciprocal(XYZArray const& molCoords, const uint molIndex,
                               const uint box);

  //back up reciptocate value to Ref (will be called during initialization)
  virtual void SetRecipRef(uint box);

  //update reciprocate values
  virtual void UpdateRecip(uint box);

  //update the hx,y,z hsqr and prefact
  virtual void UpdateRecipVec(uint box);

  //restore cosMol and sinMol
  virtual void RestoreMol(int molIndex);

  //update sinMol and cosMol
  virtual void exgMolCache();

  //backup the whole cosMolRef & sinMolRef into cosMolBoxRecip & sinMolBoxRecip
  virtual void backupMolCache();

  virtual void UpdateVectorsAndRecipTerms();

};


#endif /*NOEWALD_H*/
