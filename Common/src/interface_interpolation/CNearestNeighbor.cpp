/*!
 * \file CNearestNeighbor.cpp
 * \brief Implementation of nearest neighbor interpolation.
 * \author H. Kline
 * \version 7.0.2 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2020, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../include/interface_interpolation/CNearestNeighbor.hpp"
#include "../../include/CConfig.hpp"
#include "../../include/geometry/CGeometry.hpp"


/*! \brief Helper struct to (partially) sort neighbours according to distance while
 *         keeping track of the origin of the point (i.e. index and processor). */
struct DonorInfo {
  su2double dist;
  unsigned pidx;
  int proc;
  DonorInfo(su2double d = 0.0, unsigned i = 0, int p = 0) :
    dist(d), pidx(i), proc(p) { }
};

CNearestNeighbor::CNearestNeighbor(CGeometry ****geometry_container, const CConfig* const* config,  unsigned int iZone,
                                   unsigned int jZone) : CInterpolator(geometry_container, config, iZone, jZone) {
  Set_TransferCoeff(config);
}

void CNearestNeighbor::Set_TransferCoeff(const CConfig* const* config) {

  /*--- Desired number of donor points. ---*/
  const auto nDonor = max<unsigned long>(config[donorZone]->GetNumNearestNeighbors(), 1);

  const su2double eps = numeric_limits<passivedouble>::epsilon();

  const int nProcessor = size;
  const auto nMarkerInt = config[donorZone]->GetMarker_n_ZoneInterface()/2;
  const auto nDim = donor_geometry->GetnDim();

  Buffer_Receive_nVertex_Donor = new unsigned long [nProcessor];

  vector<vector<DonorInfo> > DonorInfoVec(omp_get_max_threads());

  /*--- Cycle over nMarkersInt interface to determine communication pattern. ---*/

  for (unsigned short iMarkerInt = 1; iMarkerInt <= nMarkerInt; iMarkerInt++) {

    /*--- On the donor side: find the tag of the boundary sharing the interface. ---*/
    const auto markDonor = Find_InterfaceMarker(config[donorZone], iMarkerInt);

    /*--- On the target side: find the tag of the boundary sharing the interface. ---*/
    const auto markTarget = Find_InterfaceMarker(config[targetZone], iMarkerInt);

    /*--- Checks if the zone contains the interface, if not continue to the next step. ---*/
    if (!CheckInterfaceBoundary(markDonor, markTarget)) continue;

    unsigned long nVertexDonor = 0, nVertexTarget = 0;
    if(markDonor != -1) nVertexDonor = donor_geometry->GetnVertex( markDonor );
    if(markTarget != -1) nVertexTarget = target_geometry->GetnVertex( markTarget );

    /* Sets MaxLocalVertex_Donor, Buffer_Receive_nVertex_Donor. */
    Determine_ArraySize(false, markDonor, markTarget, nVertexDonor, nDim);

    const auto nPossibleDonor = accumulate(Buffer_Receive_nVertex_Donor,
                                Buffer_Receive_nVertex_Donor+nProcessor, 0ul);

    Buffer_Send_Coord = new su2double [ MaxLocalVertex_Donor * nDim ];
    Buffer_Send_GlobalPoint = new long [ MaxLocalVertex_Donor ];
    Buffer_Receive_Coord = new su2double [ nProcessor * MaxLocalVertex_Donor * nDim ];
    Buffer_Receive_GlobalPoint = new long [ nProcessor * MaxLocalVertex_Donor ];

    /*-- Collect coordinates and global point indices. ---*/
    Collect_VertexInfo( false, markDonor, markTarget, nVertexDonor, nDim );

    /*--- Find the closest donor points to each target. ---*/
    SU2_OMP_PARALLEL
    {
    /*--- Working array for this thread. ---*/
    auto& donorInfo = DonorInfoVec[omp_get_thread_num()];
    donorInfo.resize(nPossibleDonor);

    SU2_OMP_FOR_DYN(roundUpDiv(nVertexTarget,2*omp_get_max_threads()))
    for (auto iVertexTarget = 0ul; iVertexTarget < nVertexTarget; iVertexTarget++) {

      auto target_vertex = target_geometry->vertex[markTarget][iVertexTarget];
      const auto Point_Target = target_vertex->GetNode();

      if (!target_geometry->node[Point_Target]->GetDomain()) continue;

      /*--- Coordinates of the target point. ---*/
      const su2double* Coord_i = target_geometry->node[Point_Target]->GetCoord();

      /*--- Compute all distances. ---*/
      for (int iProcessor = 0, iDonor = 0; iProcessor < nProcessor; ++iProcessor) {
        for (auto jVertex = 0ul; jVertex < Buffer_Receive_nVertex_Donor[iProcessor]; ++jVertex) {

          const auto idx = iProcessor*MaxLocalVertex_Donor + jVertex;
          const auto pGlobalPoint = Buffer_Receive_GlobalPoint[idx];
          const su2double* Coord_j = &Buffer_Receive_Coord[idx*nDim];
          const auto dist2 = PointsSquareDistance(nDim, Coord_i, Coord_j);

          donorInfo[iDonor++] = DonorInfo(dist2, pGlobalPoint, iProcessor);
        }
      }

      /*--- Find k closest points. ---*/
      partial_sort(donorInfo.begin(), donorInfo.begin()+nDonor, donorInfo.end(),
                   [](const DonorInfo& a, const DonorInfo& b){return a.dist < b.dist;});

      /*--- Compute interpolation numerators and denominator. ---*/
      su2double denom = 0.0;
      for (auto iDonor = 0ul; iDonor < nDonor; ++iDonor) {
        donorInfo[iDonor].dist = 1.0 / (donorInfo[iDonor].dist + eps);
        denom += donorInfo[iDonor].dist;
      }

      /*--- Set interpolation coefficients. ---*/
      target_vertex->Allocate_DonorInfo(nDonor);

      for (auto iDonor = 0ul; iDonor < nDonor; ++iDonor) {
        target_vertex->SetInterpDonorPoint(iDonor, donorInfo[iDonor].pidx);
        target_vertex->SetInterpDonorProcessor(iDonor, donorInfo[iDonor].proc);
        target_vertex->SetDonorCoeff(iDonor, donorInfo[iDonor].dist/denom);
      }
    }
    } // end SU2_OMP_PARALLEL

    delete[] Buffer_Send_Coord;
    delete[] Buffer_Send_GlobalPoint;

    delete[] Buffer_Receive_Coord;
    delete[] Buffer_Receive_GlobalPoint;

  }

  delete[] Buffer_Receive_nVertex_Donor;

}