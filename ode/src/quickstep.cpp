/*************************************************************************
 *                                                                       *
 * Open Dynamics Engine, Copyright (C) 2001-2003 Russell L. Smith.       *
 * All rights reserved.  Email: russ@q12.org   Web: www.q12.org          *
 *                                                                       *
 * This library is free software; you can redistribute it and/or         *
 * modify it under the terms of EITHER:                                  *
 *   (1) The GNU Lesser General Public License as published by the Free  *
 *       Software Foundation; either version 2.1 of the License, or (at  *
 *       your option) any later version. The text of the GNU Lesser      *
 *       General Public License is included with this library in the     *
 *       file LICENSE.TXT.                                               *
 *   (2) The BSD-style license that is included with this library in     *
 *       the file LICENSE-BSD.TXT.                                       *
 *                                                                       *
 * This library is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the files    *
 * LICENSE.TXT and LICENSE-BSD.TXT for more details.                     *
 *                                                                       *
 *************************************************************************/

// @@@ TODO: joint feedback is not yet operational.

#include "objects.h"
#include "joint.h"
#include <ode/config.h>
#include <ode/odemath.h>
#include <ode/rotation.h>
#include <ode/timer.h>
#include <ode/error.h>
#include <ode/matrix.h>
#include "lcp.h"
#include "util.h"

#define ALLOCA dALLOCA16

typedef const dReal *dRealPtr;
typedef dReal *dRealMutablePtr;
#define dRealArray(name,n) dReal name[n];
#define dRealAllocaArray(name,n) dReal *name = (dReal*) ALLOCA ((n)*sizeof(dReal));

//***************************************************************************
// testing stuff

#ifdef TIMING
#define IFTIMING(x) x
#else
#define IFTIMING(x) /* */
#endif

//***************************************************************************
// SOR-LCP method

// nb is the number of bodies in the body array.
// J is an m*12 matrix of constraint rows
// jb is an array of first and second body numbers for each constraint row
// invI is the global frame inverse inertia for each body (stacked 3x3 matrices)
//
// this returns lambda and fc (the constraint force).
// note: fc is returned as inv(M)*J'*lambda, the constraint force is actually J'*lambda
//
// b, lo and hi are modified on exit
//
//@@@ if we divide the system into islands we have to do this multiple times?
//	is there a speed penalty? shouldn't be big because we deal with each constraint
//	the same number of times in either case.


// uncomment the following line to determine a new constraint-solving
// order for each iteration. however, the qsort per iteration is expensive,
// and the optimal order is somewhat problem dependent. the unsorted
// order is quite often the best way to go, especially for low CFM.
// @@@ try the leaf->root ordering.

//#define REORDER_CONSTRAINTS 1


struct IndexError {
	dReal error;		// error to sort on
	int findex;
	int index;		// row index
};


#ifdef REORDER_CONSTRAINTS

static int compare_index_error (const void *a, const void *b)
{
	const IndexError *i1 = (IndexError*) a;
	const IndexError *i2 = (IndexError*) b;
	if (i1->findex < 0 && i2->findex >= 0) return -1;
	if (i1->findex >= 0 && i2->findex < 0) return 1;
	if (i1->error < i2->error) return -1;
	if (i1->error > i2->error) return 1;
	return 0;
}

#endif


static void SOR_LCP (int m, int nb, dRealMutablePtr J, int *jb, dxBody * const *body,
	dRealPtr invI, dRealMutablePtr lambda, dRealMutablePtr fc, dRealMutablePtr b,
	dRealMutablePtr lo, dRealMutablePtr hi, dRealPtr cfm, int *findex,
	dxQuickStepParameters *qs)
{
	const int num_iterations = qs->num_iterations;
	const dReal sor_w = qs->w;		// SOR over-relaxation parameter

	int i,j;

	// the estimated solution lambda.
	//	@@@ this could be warm started, but then we'd have to set fc too
	dSetZero (lambda,m);
        dSetZero (fc,nb*6);
	
	// the lambda computed at the previous iteration
	dRealAllocaArray (last_lambda,m);

	// a copy of the 'hi' vector in case findex[] is being used
	dRealAllocaArray (hicopy,m);
	memcpy (hicopy,hi,m*sizeof(dReal));

	// precompute iMJ = inv(M)*J'
	dRealAllocaArray (iMJ,m*12);
	dRealMutablePtr iMJ_ptr = iMJ;
	dRealMutablePtr J_ptr = J;
	for (i=0; i<m; i++) {
		int b1 = jb[i*2];	
		int b2 = jb[i*2+1];
		dReal k = body[b1]->invMass;
		for (j=0; j<3; j++) iMJ_ptr[j] = k*J_ptr[j];
		dMULTIPLY0_331 (iMJ_ptr + 3, invI + 12*b1, J_ptr + 3);
		if (b2 >= 0) {
			k = body[b2]->invMass;
			for (j=0; j<3; j++) iMJ_ptr[j+6] = k*J_ptr[j+6];
			dMULTIPLY0_331 (iMJ_ptr + 9, invI + 12*b2, J_ptr + 9);
		}
		J_ptr += 12;
		iMJ_ptr += 12;
	}

	// precompute 1 / diagonals of A
	dRealAllocaArray (Ad,m);
	iMJ_ptr = iMJ;
	J_ptr = J;
	for (i=0; i<m; i++) {
		dReal sum = 0;
		for (j=0; j<6; j++) sum += iMJ_ptr[j] * J_ptr[j];
		if (jb[i*2+1] >= 0) {
			for (j=6; j<12; j++) sum += iMJ_ptr[j] * J_ptr[j];
		}
		iMJ_ptr += 12;
		J_ptr += 12;
		Ad[i] = sor_w / (sum + cfm[i]);
	}

	// scale J and b by Ad
	J_ptr = J;
	for (i=0; i<m; i++) {
		for (j=0; j<12; j++) {
			J_ptr[0] *= Ad[i];
			J_ptr++;
		}
		b[i] *= Ad[i];
	}

	// scale Ad by CFM
	for (i=0; i<m; i++) Ad[i] *= cfm[i];

	// order to solve constraint rows in
	IndexError *order = (IndexError*) alloca (m*sizeof(IndexError));

#ifndef REORDER_CONSTRAINTS
	// make sure constraints with findex < 0 come first.
	j=0;
	for (i=0; i<m; i++) if (findex[i] < 0) order[j++].index = i;
	for (i=0; i<m; i++) if (findex[i] >= 0) order[j++].index = i;
	dIASSERT (j==m);
#endif

	for (int iteration=0; iteration < num_iterations; iteration++) {

#ifdef REORDER_CONSTRAINTS
		// constraints with findex < 0 always come first.
		if (iteration < 2) {
			// for the first two iterations, solve the constraints in
			// the given order
			for (i=0; i<m; i++) {
				order[i].error = i;
				order[i].findex = findex[i];
				order[i].index = i;
			}
		}
		else {
			// sort the constraints so that the ones converging slowest
			// get solved last. use the absolute (not relative) error.
			for (i=0; i<m; i++) {
				dReal v1 = dFabs (lambda[i]);
				dReal v2 = dFabs (last_lambda[i]);
				dReal max = (v1 > v2) ? v1 : v2;
				if (max > 0) {
					order[i].error = dFabs(lambda[i]-last_lambda[i]);
				}
				else {
					order[i].error = dInfinity;
				}
				order[i].findex = findex[i];
				order[i].index = i;
			}
		}
		qsort (order,m,sizeof(IndexError),&compare_index_error);
#endif
	
		//@@@ potential optimization: swap lambda and last_lambda pointers rather
		//    than copying the data. we must make sure lambda is properly
		//    returned to the caller
		memcpy (last_lambda,lambda,m*sizeof(dReal));

		for (int i=0; i<m; i++) {
			// @@@ potential optimization: we could pre-sort J and iMJ, thereby
			//     linearizing access to those arrays. hmmm, this does not seem
			//     like a win, but we should think carefully about our memory
			//     access pattern.
		
			int index = order[i].index;
			J_ptr = J + index*12;
			iMJ_ptr = iMJ + index*12;
		
			// set the limits for this constraint. note that 'hicopy' is used.
			// this is the place where the QuickStep method differs from the
			// direct LCP solving method, since that method only performs this
			// limit adjustment once per time step, whereas this method performs
			// once per iteration per constraint row.
			// the constraints are ordered so that all lambda[] values needed have
			// already been computed.
			if (findex[index] >= 0) {
				//@@@
				//hi[index] = dFabs (hicopy[index] * lambda[findex[index]]);
				//lo[index] = -hi[index];
			}

			int b1 = jb[index*2];
			int b2 = jb[index*2+1];
			dReal delta = b[index] - lambda[index]*Ad[index];
			dRealMutablePtr fc_ptr = fc + 6*b1;
			
			// @@@ potential optimization: SIMD-ize this and the b2 >= 0 case
			delta -=fc_ptr[0] * J_ptr[0] + fc_ptr[1] * J_ptr[1] +
				fc_ptr[2] * J_ptr[2] + fc_ptr[3] * J_ptr[3] +
				fc_ptr[4] * J_ptr[4] + fc_ptr[5] * J_ptr[5];
			// @@@ potential optimization: handle 1-body constraints in a separate
			//     loop to avoid the cost of test & jump?
			if (b2 >= 0) {
				fc_ptr = fc + 6*b2;
				delta -=fc_ptr[0] * J_ptr[6] + fc_ptr[1] * J_ptr[7] +
					fc_ptr[2] * J_ptr[8] + fc_ptr[3] * J_ptr[9] +
					fc_ptr[4] * J_ptr[10] + fc_ptr[5] * J_ptr[11];
			}

			// compute lambda and clamp it to [lo,hi].
			// @@@ potential optimization: does SSE have clamping instructions
			//     to save test+jump penalties here?
			dReal new_lambda = lambda[index] + delta;
			if (new_lambda < lo[index]) {
				delta = lo[index]-lambda[index];
				lambda[index] = lo[index];
			}
			else if (new_lambda > hi[index]) {
				delta = hi[index]-lambda[index];
				lambda[index] = hi[index];
			}
			else {
				lambda[index] = new_lambda;
			}
		
			// update fc.
			// @@@ potential optimization: SIMD for this and the b2 >= 0 case
			fc_ptr = fc + 6*b1;
			fc_ptr[0] += delta * iMJ_ptr[0];
			fc_ptr[1] += delta * iMJ_ptr[1];
			fc_ptr[2] += delta * iMJ_ptr[2];
			fc_ptr[3] += delta * iMJ_ptr[3];
			fc_ptr[4] += delta * iMJ_ptr[4];
			fc_ptr[5] += delta * iMJ_ptr[5];
			// @@@ potential optimization: handle 1-body constraints in a separate
			//     loop to avoid the cost of test & jump?
			if (b2 >= 0) {
				fc_ptr = fc + 6*b2;
				fc_ptr[0] += delta * iMJ_ptr[6];
				fc_ptr[1] += delta * iMJ_ptr[7];
				fc_ptr[2] += delta * iMJ_ptr[8];
				fc_ptr[3] += delta * iMJ_ptr[9];
				fc_ptr[4] += delta * iMJ_ptr[10];
				fc_ptr[5] += delta * iMJ_ptr[11];
			}
		}
	}

}


void dxQuickStepper (dxWorld *world, dxBody * const *body, int nb,
		     dxJoint * const *_joint, int nj, dReal stepsize)
{
	int i,j;
	IFTIMING(dTimerStart("preprocessing");)

	dReal stepsize1 = dRecip(stepsize);

	// number all bodies in the body list - set their tag values
	for (i=0; i<nb; i++) body[i]->tag = i;
	
	// make a local copy of the joint array, because we might want to modify it.
	// (the "dxJoint *const*" declaration says we're allowed to modify the joints
	// but not the joint array, because the caller might need it unchanged).
	//@@@ do we really need to do this? we'll be sorting constraint rows individually, not joints
	dxJoint **joint = (dxJoint**) alloca (nj * sizeof(dxJoint*));
	memcpy (joint,_joint,nj * sizeof(dxJoint*));
	
	// for all bodies, compute the inertia tensor and its inverse in the global
	// frame, and compute the rotational force and add it to the torque
	// accumulator. invI is a vertical stack of 3x4 matrices, one per body.
	dRealAllocaArray (invI,3*4*nb);
	for (i=0; i<nb; i++) {
		dMatrix3 I,tmp;
		// compute inertia tensor in global frame
		dMULTIPLY2_333 (tmp,body[i]->mass.I,body[i]->R);
		dMULTIPLY0_333 (I,body[i]->R,tmp);
		// compute inverse inertia tensor in global frame
		dMULTIPLY2_333 (tmp,body[i]->invI,body[i]->R);
		dMULTIPLY0_333 (invI+i*12,body[i]->R,tmp);
		// compute rotational force
		dMULTIPLY0_331 (tmp,I,body[i]->avel);
		dCROSS (body[i]->tacc,-=,body[i]->avel,tmp);
	}

	// add the gravity force to all bodies
	for (i=0; i<nb; i++) {
		if ((body[i]->flags & dxBodyNoGravity)==0) {
			body[i]->facc[0] += body[i]->mass.mass * world->gravity[0];
			body[i]->facc[1] += body[i]->mass.mass * world->gravity[1];
			body[i]->facc[2] += body[i]->mass.mass * world->gravity[2];
		}
	}

	// get joint information (m = total constraint dimension, nub = number of unbounded variables).
	// joints with m=0 are inactive and are removed from the joints array
	// entirely, so that the code that follows does not consider them.
	//@@@ do we really need to save all the info1's
	dxJoint::Info1 *info = (dxJoint::Info1*) alloca (nj*sizeof(dxJoint::Info1));
	for (i=0, j=0; j<nj; j++) {	// i=dest, j=src
		joint[j]->vtable->getInfo1 (joint[j],info+i);
		dIASSERT (info[i].m >= 0 && info[i].m <= 6 && info[i].nub >= 0 && info[i].nub <= info[i].m);
		if (info[i].m > 0) {
			joint[i] = joint[j];
			i++;
		}
	}
	nj = i;

	// create the row offset array
	int m = 0;
	int *ofs = (int*) alloca (nj*sizeof(int));
	for (i=0; i<nj; i++) {
		ofs[i] = m;
		m += info[i].m;
	}

	// if there are constraints, compute the constraint force
	if (m > 0) {
		// create a constraint equation right hand side vector `c', a constraint
		// force mixing vector `cfm', and LCP low and high bound vectors, and an
		// 'findex' vector.
		dRealAllocaArray (c,m);
		dRealAllocaArray (cfm,m);
		dRealAllocaArray (lo,m);
		dRealAllocaArray (hi,m);
		int *findex = (int*) alloca (m*sizeof(int));
		dSetZero (c,m);
		dSetValue (cfm,m,world->global_cfm);
		dSetValue (lo,m,-dInfinity);
		dSetValue (hi,m, dInfinity);
		for (i=0; i<m; i++) findex[i] = -1;
		
		// get jacobian data from constraints. an m*12 matrix will be created
		// to store the two jacobian blocks from each constraint. it has this
		// format:
		//
		//   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 \    .
		//   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2  }-- jacobian for joint 0, body 1 and body 2 (3 rows)
		//   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 /
		//   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 }--- jacobian for joint 1, body 1 and body 2 (3 rows)
		//   etc...
		//
		//   (lll) = linear jacobian data
		//   (aaa) = angular jacobian data
		//
		IFTIMING (dTimerNow ("create J");)
		dRealAllocaArray (J,m*12);
		dSetZero (J,m*12);
		dxJoint::Info2 Jinfo;
		Jinfo.rowskip = 12;
		Jinfo.fps = stepsize1;
		Jinfo.erp = world->global_erp;
		for (i=0; i<nj; i++) {
			Jinfo.J1l = J + ofs[i]*12;
			Jinfo.J1a = Jinfo.J1l + 3;
			Jinfo.J2l = Jinfo.J1l + 6;
			Jinfo.J2a = Jinfo.J1l + 9;
			Jinfo.c = c + ofs[i];
			Jinfo.cfm = cfm + ofs[i];
			Jinfo.lo = lo + ofs[i];
			Jinfo.hi = hi + ofs[i];
			Jinfo.findex = findex + ofs[i];
			joint[i]->vtable->getInfo2 (joint[i],&Jinfo);
			// adjust returned findex values for global index numbering
			for (j=0; j<info[i].m; j++) {
				if (findex[ofs[i] + j] >= 0) findex[ofs[i] + j] += ofs[i];
			}
		}

		// create an array of body numbers for each joint row
		int *jb = (int*) alloca (m*2*sizeof(int));
		int *jb_ptr = jb;
		for (i=0; i<nj; i++) {
			int b1 = (joint[i]->node[0].body) ? (joint[i]->node[0].body->tag) : -1;
			int b2 = (joint[i]->node[1].body) ? (joint[i]->node[1].body->tag) : -1;
			for (j=0; j<info[i].m; j++) {
				jb_ptr[0] = b1;
				jb_ptr[1] = b2;
				jb_ptr += 2;
			}
		}
		dIASSERT (jb_ptr == jb+2*m);

		// compute the right hand side `rhs'
		IFTIMING (dTimerNow ("compute rhs");)
		dRealAllocaArray (tmp1,nb*6);	//@@@ do we really need this big vector? incremental approach...
		// put v/h + invM*fe into tmp1
		for (i=0; i<nb; i++) {
			dReal body_invMass = body[i]->invMass;
			for (j=0; j<3; j++) tmp1[i*6+j] = body[i]->facc[j] * body_invMass + body[i]->lvel[j] * stepsize1;
			dMULTIPLY0_331 (tmp1 + i*6 + 3,invI + i*12,body[i]->tacc);
			for (j=0; j<3; j++) tmp1[i*6+3+j] += body[i]->avel[j] * stepsize1;
		}

		// put J*tmp1 into rhs
		dRealAllocaArray (rhs,m);
		dRealPtr J_ptr = J;
		for (i=0; i<m; i++) {
			int b1 = jb[i*2];
			int b2 = jb[i*2+1];
			dReal sum = 0;
			for (j=0; j<6; j++) sum += J_ptr[j] * tmp1[b1*6+j];
			J_ptr += 6;
			if (b2 >= 0) {
				for (j=0; j<6; j++) sum += J_ptr[j] * tmp1[b2*6+j];
			}
			J_ptr += 6;
			rhs[i] = sum;
		}

		// complete rhs
		for (i=0; i<m; i++) rhs[i] = c[i]*stepsize1 - rhs[i];

		// scale CFM
		for (i=0; i<m; i++) cfm[i] *= stepsize1;

		// solve the LCP problem and get lambda and constraint force
		IFTIMING (dTimerNow ("solving LCP problem");)
		dRealAllocaArray (lambda,m);
		dRealAllocaArray (cforce,nb*6);
		SOR_LCP (m,nb,J,jb,body,invI,lambda,cforce,rhs,lo,hi,cfm,findex,&world->qs);

		// note that rhs and J are overwritten at this point and should not be used again.
		// make sure of this.
		J=0;
		rhs=0;
		
		// add stepsize * cforce to the body velocity
		for (i=0; i<nb; i++) {
			for (j=0; j<3; j++) body[i]->lvel[j] += stepsize * cforce[i*6+j];
			for (j=0; j<3; j++) body[i]->avel[j] += stepsize * cforce[i*6+3+j];
		}

		      //@@@ reinstate joint feedback:
		      // BUT: cforce is inv(M)*J'*lambda, whereas we want just J'*lambda
		      /*
		      dJointFeedback *fb = joint[i]->feedback;
		
		      if (fb) {
			// the user has requested feedback on the amount of force that this
			// joint is applying to the bodies. we use a slightly slower
			// computation that splits out the force components and puts them
			// in the feedback structure.
			dVector8 data1,data2;
			Multiply1_8q1 (data1, JJ, lambda+ofs[i], info[i].m);
			dRealPtr cf1 = cforce + 8*b1->tag;
			cf1[0] += (fb->f1[0] = data1[0]);
			cf1[1] += (fb->f1[1] = data1[1]);
			cf1[2] += (fb->f1[2] = data1[2]);
			cf1[4] += (fb->t1[0] = data1[4]);
			cf1[5] += (fb->t1[1] = data1[5]);
			cf1[6] += (fb->t1[2] = data1[6]);
			if (b2){
			  Multiply1_8q1 (data2, JJ + 8*info[i].m, lambda+ofs[i], info[i].m);
			  dRealPtr cf2 = cforce + 8*b2->tag;
			  cf2[0] += (fb->f2[0] = data2[0]);
			  cf2[1] += (fb->f2[1] = data2[1]);
			  cf2[2] += (fb->f2[2] = data2[2]);
			  cf2[4] += (fb->t2[0] = data2[4]);
			  cf2[5] += (fb->t2[1] = data2[5]);
			  cf2[6] += (fb->t2[2] = data2[6]);
			}
		      }
		      */
	}

	// compute the velocity update:
	// add stepsize * invM * fe to the body velocity

	IFTIMING (dTimerNow ("compute velocity update");)
	for (i=0; i<nb; i++) {
		dReal body_invMass = body[i]->invMass;
		for (j=0; j<3; j++) body[i]->lvel[j] += stepsize * body_invMass * body[i]->facc[j];
		for (j=0; j<3; j++) body[i]->tacc[j] *= stepsize;	
		dMULTIPLYADD0_331 (body[i]->avel,invI + i*12,body[i]->tacc);
	}

	// update the position and orientation from the new linear/angular velocity
	// (over the given timestep)
	IFTIMING (dTimerNow ("update position");)
	for (i=0; i<nb; i++) dxStepBody (body[i],stepsize);

	IFTIMING (dTimerNow ("tidy up");)

	// zero all force accumulators
	for (i=0; i<nb; i++) {
		dSetZero (body[i]->facc,3);
		dSetZero (body[i]->tacc,3);
	}

	IFTIMING (dTimerEnd();)
	IFTIMING (if (m > 0) dTimerReport (stdout,1);)
}