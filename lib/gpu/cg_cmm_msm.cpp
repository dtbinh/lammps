/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing authors: Mike Brown (ORNL), brownw@ornl.gov
------------------------------------------------------------------------- */

#ifdef USE_OPENCL
#include "cg_cmm_msm_ext_cl.h"
#else
#include "cg_cmm_msm_ext_ptx.h"
#endif

#include "cg_cmm_msm.h"
#include <cassert>
#define CMMM_GPU_MemoryT CMMM_GPU_Memory<numtyp, acctyp>

extern PairGPUDevice<PRECISION,ACC_PRECISION> device;

template <class numtyp, class acctyp>
CMMM_GPU_MemoryT::CMMM_GPU_Memory() : ChargeGPUMemory<numtyp,acctyp>(),
                                    _allocated(false) {
}

template <class numtyp, class acctyp>
CMMM_GPU_MemoryT::~CMMM_GPU_Memory() {
  clear();
}
 
template <class numtyp, class acctyp>
int CMMM_GPU_MemoryT::bytes_per_atom(const int max_nbors) const {
  return this->bytes_per_atom_atomic(max_nbors);
}

template <class numtyp, class acctyp>
int CMMM_GPU_MemoryT::init(const int ntypes, double **host_cutsq, 
                           int **host_cg_type, double **host_lj1, 
                           double **host_lj2, double **host_lj3, 
                           double **host_lj4, double **host_offset, 
                           double *host_special_lj, const int nlocal,
                           const int nall, const int max_nbors,
                           const int maxspecial, const double cell_size,
                           const double gpu_split, FILE *_screen,
                           double **host_cut_ljsq, 
                           const double host_cut_coulsq,
                           double *host_special_coul, const double qqrd2e,
                           const int smooth) {
  int success;
  success=this->init_atomic(nlocal,nall,max_nbors,maxspecial,cell_size,gpu_split,
                            _screen,cg_cmm_msm);
  if (success!=0)
    return success;

  // If atom type constants fit in shared memory use fast kernel
  int lj_types=ntypes;
  shared_types=false;
  int max_shared_types=this->device->max_shared_types();
  if (lj_types<=max_shared_types && this->_block_size>=max_shared_types) {
    lj_types=max_shared_types;
    shared_types=true;
  }
  _lj_types=lj_types;

  // Allocate a host write buffer for data initialization
  UCL_H_Vec<numtyp> host_write(lj_types*lj_types*32,*(this->ucl_device),
                               UCL_WRITE_OPTIMIZED);

  for (int i=0; i<lj_types*lj_types; i++)
    host_write[i]=0.0;

  lj1.alloc(lj_types*lj_types,*(this->ucl_device),UCL_READ_ONLY);
  this->atom->type_pack4(ntypes,lj_types,lj1,host_write,host_cutsq,
                         host_cut_ljsq,host_lj1,host_lj2);

  lj3.alloc(lj_types*lj_types,*(this->ucl_device),UCL_READ_ONLY);
  this->atom->type_pack4(ntypes,lj_types,lj3,host_write,host_cg_type,host_lj3,
                         host_lj4,host_offset);

  sp_lj.alloc(8,*(this->ucl_device),UCL_READ_ONLY);
  for (int i=0; i<4; i++) {
    host_write[i]=host_special_lj[i];
    host_write[i+4]=host_special_coul[i];
  }
  ucl_copy(sp_lj,host_write,8,false);

  _cut_coulsq=host_cut_coulsq;
  _qqrd2e=qqrd2e;
  _smooth=smooth;

  _allocated=true;
  this->_max_bytes=lj1.row_bytes()+lj3.row_bytes()+sp_lj.row_bytes();
  return 0;
}

template <class numtyp, class acctyp>
void CMMM_GPU_MemoryT::clear() {
  if (!_allocated)
    return;
  _allocated=false;

  lj1.clear();
  lj3.clear();
  sp_lj.clear();
  this->clear_atomic();
}

template <class numtyp, class acctyp>
double CMMM_GPU_MemoryT::host_memory_usage() const {
  return this->host_memory_usage_atomic()+sizeof(CMMM_GPU_Memory<numtyp,acctyp>);
}

// ---------------------------------------------------------------------------
// Calculate energies, forces, and torques
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
void CMMM_GPU_MemoryT::loop(const bool _eflag, const bool _vflag) {
  // Compute the block size and grid size to keep all cores busy
  const int BX=this->block_size();
  int eflag, vflag;
  if (_eflag)
    eflag=1;
  else
    eflag=0;

  if (_vflag)
    vflag=1;
  else
    vflag=0;
  
  int GX=static_cast<int>(ceil(static_cast<double>(this->ans->inum())/
                               (BX/this->_threads_per_atom)));

  int ainum=this->ans->inum();
  int nbor_pitch=this->nbor->nbor_pitch();
  this->time_pair.start();
  if (shared_types) {
    this->k_pair_fast.set_size(GX,BX);
    this->k_pair_fast.run(&this->atom->dev_x.begin(), &lj1.begin(),
                          &lj3.begin(), &sp_lj.begin(),
                          &this->nbor->dev_nbor.begin(),
                          &this->_nbor_data->begin(),
                          &this->ans->dev_ans.begin(),
                          &this->ans->dev_engv.begin(), &eflag, &vflag,
                          &ainum, &nbor_pitch,
                          &this->atom->dev_q.begin(), &_cut_coulsq,
                          &_qqrd2e, &_smooth, &this->_threads_per_atom);
  } else {
    this->k_pair.set_size(GX,BX);
    this->k_pair.run(&this->atom->dev_x.begin(), &lj1.begin(), &lj3.begin(),
                     &_lj_types, &sp_lj.begin(), &this->nbor->dev_nbor.begin(),
                     &this->_nbor_data->begin(), &this->ans->dev_ans.begin(),
                     &this->ans->dev_engv.begin(), &eflag, &vflag, &ainum,
                     &nbor_pitch, &this->atom->dev_q.begin(), &_cut_coulsq,
                     &_qqrd2e, &_smooth, &this->_threads_per_atom);
  }
  this->time_pair.stop();
}

template class CMMM_GPU_Memory<PRECISION,ACC_PRECISION>;
