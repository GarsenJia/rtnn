#include <cuda_runtime.h>

#include <sutil/Exception.h>
#include <sutil/vec_math.h>
#include <sutil/Timing.h>

#include <thrust/device_vector.h>
#include <thrust/sort.h>
#include <thrust/copy.h>
#include <thrust/sequence.h>
#include <thrust/gather.h>

#include <algorithm>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <string>
#include <random>
#include <cstdlib>
#include <queue>
#include <unordered_set>

#include "optixRangeSearch.h"
#include "func.h"
#include "state.h"
#include "grid.h"

void computeMinMax(WhittedState& state, ParticleType type)
{
  unsigned int N;
  float3* particles;
  if (type == POINT) {
    N = state.numPoints;
    particles = state.params.points;
  } else {
    N = state.numQueries;
    particles = state.params.queries;
  }

  // TODO: maybe use long since we are going to convert a float to its floor value?
  thrust::host_vector<int3> h_MinMax(2);
  h_MinMax[0] = make_int3(std::numeric_limits<int>().max(), std::numeric_limits<int>().max(), std::numeric_limits<int>().max());
  h_MinMax[1] = make_int3(std::numeric_limits<int>().min(), std::numeric_limits<int>().min(), std::numeric_limits<int>().min());

  thrust::device_vector<int3> d_MinMax = h_MinMax;

  unsigned int threadsPerBlock = 64;
  unsigned int numOfBlocks = N / threadsPerBlock + 1;
  // compare only the ints since atomicAdd has only int version
  kComputeMinMax(numOfBlocks,
                 threadsPerBlock,
                 particles,
                 N,
                 thrust::raw_pointer_cast(&d_MinMax[0]),
                 thrust::raw_pointer_cast(&d_MinMax[1])
                 );

  h_MinMax = d_MinMax;

  // minCell encloses the scene but maxCell doesn't (floor and int in the kernel) so increment by 1 to enclose the scene.
  // TODO: consider minus 1 for minCell too to avoid the numerical precision issue
  int3 minCell = h_MinMax[0];
  int3 maxCell = h_MinMax[1] + make_int3(1, 1, 1);
 
  state.Min.x = minCell.x;
  state.Min.y = minCell.y;
  state.Min.z = minCell.z;
 
  state.Max.x = maxCell.x;
  state.Max.y = maxCell.y;
  state.Max.z = maxCell.z;

  //fprintf(stdout, "\tcell boundary: (%d, %d, %d), (%d, %d, %d)\n", minCell.x, minCell.y, minCell.z, maxCell.x, maxCell.y, maxCell.z);
  //fprintf(stdout, "\tscene boundary: (%f, %f, %f), (%f, %f, %f)\n", state.Min.x, state.Min.y, state.Min.z, state.Max.x, state.Max.y, state.Max.z);
}

unsigned int genGridInfo(WhittedState& state, unsigned int N, GridInfo& gridInfo) {
  float3 sceneMin = state.Min;
  float3 sceneMax = state.Max;

  gridInfo.ParticleCount = N;
  gridInfo.GridMin = sceneMin;

  // TODO: maybe crRatio should be automatically determined based on memory?
  float cellSize = state.radius/state.crRatio;
  float3 gridSize = sceneMax - sceneMin;
  gridInfo.GridDimension.x = static_cast<unsigned int>(ceilf(gridSize.x / cellSize));
  gridInfo.GridDimension.y = static_cast<unsigned int>(ceilf(gridSize.y / cellSize));
  gridInfo.GridDimension.z = static_cast<unsigned int>(ceilf(gridSize.z / cellSize));

  // Adjust grid size to multiple of cell size
  gridSize.x = gridInfo.GridDimension.x * cellSize;
  gridSize.y = gridInfo.GridDimension.y * cellSize;
  gridSize.z = gridInfo.GridDimension.z * cellSize;

  gridInfo.GridDelta.x = gridInfo.GridDimension.x / gridSize.x;
  gridInfo.GridDelta.y = gridInfo.GridDimension.y / gridSize.y;
  gridInfo.GridDelta.z = gridInfo.GridDimension.z / gridSize.z;

  // morton code can only be correctly calcuated for a cubic, where each
  // dimension is of the same size and the dimension is a power of 2. if we
  // were to generate one single morton code for the entire grid, this would
  // waste a lot of space since a lot of empty cells will have to be padded.
  // the strategy is to divide the grid into smaller equal-dimension-power-of-2
  // smaller grids (meta_grid here). the order within each meta_grid is morton,
  // but the order across meta_grids is raster order. the current
  // implementation uses a heuristics. TODO: revisit this later.
  gridInfo.meta_grid_dim = (int)pow(2, floorf(log2(std::min({gridInfo.GridDimension.x, gridInfo.GridDimension.y, gridInfo.GridDimension.z}))))/2;
  gridInfo.meta_grid_size = gridInfo.meta_grid_dim * gridInfo.meta_grid_dim * gridInfo.meta_grid_dim;

  // One meta grid cell contains meta_grid_dim^3 cells. The morton curve is
  // calculated for each metagrid, and the order of metagrid is raster order.
  // So if meta_grid_dim is 1, this is basically the same as raster order
  // across all cells. If meta_grid_dim is the same as GridDimension, this
  // calculates one single morton curve for the entire grid.
  gridInfo.MetaGridDimension.x = static_cast<unsigned int>(ceilf(gridInfo.GridDimension.x / (float)gridInfo.meta_grid_dim));
  gridInfo.MetaGridDimension.y = static_cast<unsigned int>(ceilf(gridInfo.GridDimension.y / (float)gridInfo.meta_grid_dim));
  gridInfo.MetaGridDimension.z = static_cast<unsigned int>(ceilf(gridInfo.GridDimension.z / (float)gridInfo.meta_grid_dim));

  // metagrids will slightly increase the total cells
  unsigned int numberOfCells = (gridInfo.MetaGridDimension.x * gridInfo.MetaGridDimension.y * gridInfo.MetaGridDimension.z) * gridInfo.meta_grid_size;
  fprintf(stdout, "\tGrid dimension (without meta grids): %u, %u, %u\n", gridInfo.GridDimension.x, gridInfo.GridDimension.y, gridInfo.GridDimension.z);
  fprintf(stdout, "\tGrid dimension (with meta grids): %u, %u, %u\n", gridInfo.MetaGridDimension.x * gridInfo.meta_grid_dim, gridInfo.MetaGridDimension.y * gridInfo.meta_grid_dim, gridInfo.MetaGridDimension.z * gridInfo.meta_grid_dim);
  fprintf(stdout, "\tMeta Grid dimension: %u, %u, %u\n", gridInfo.MetaGridDimension.x, gridInfo.MetaGridDimension.y, gridInfo.MetaGridDimension.z);
  fprintf(stdout, "\t# of cells in a meta grid: %u\n", gridInfo.meta_grid_dim);
  //fprintf(stdout, "\tGridDelta: %f, %f, %f\n", gridInfo.GridDelta.x, gridInfo.GridDelta.y, gridInfo.GridDelta.z);
  fprintf(stdout, "\tNumber of cells: %u\n", numberOfCells);
  fprintf(stdout, "\tCell size: %f\n", cellSize);

  // update GridDimension so that it can be used in the kernels (otherwise raster order is incorrect)
  gridInfo.GridDimension.x = gridInfo.MetaGridDimension.x * gridInfo.meta_grid_dim;
  gridInfo.GridDimension.y = gridInfo.MetaGridDimension.y * gridInfo.meta_grid_dim;
  gridInfo.GridDimension.z = gridInfo.MetaGridDimension.z * gridInfo.meta_grid_dim;
  return numberOfCells;
}

void test(GridInfo);

thrust::device_ptr<int> genCellMask (WhittedState& state, unsigned int* d_repQueries, float3* particles, unsigned int* d_CellParticleCounts, unsigned int numberOfCells, GridInfo gridInfo, unsigned int N, unsigned int numUniqQs, bool morton) {
  float cellSize = state.radius / state.crRatio;

  // |maxWidth| is the max width of a cube that can be enclosed by the sphere.
  // in radius search, we can generate an AABB of this size and be sure that
  // there are >= K points within this AABB, we don't have to calc the dist
  // since these points are gauranted to be in the search sphere (but see the
  // important caveats in the search function). in knn search, however, we
  // can't be sure the nearest K points are in this AABB (there are points that
  // are outside of the AABB that are closer to the centroid than points in the
  // AABB), but the K nearest neighbors are gauranteed to be in the sphere that
  // tightly encloses this cube, and given the way we calculate |maxWidth| we
  // know that the radius of that sphere won't be greater than state.radius, so
  // we still save time.
  float maxWidth = state.radius / sqrt(2) * 2;

  thrust::device_ptr<int> d_cellMask;
  allocThrustDevicePtr(&d_cellMask, numberOfCells); // no need to memset this since every single cell will be updated.
  //CUDA_CHECK( cudaMemset ( thrust::raw_pointer_cast(d_cellMask), 0xFF, numberOfCells * sizeof(int) ) );

  //test(gridInfo); // to demonstrate the weird parameter passing bug.

  bool gpu = true;
  if (gpu) {
    //thrust::host_vector<unsigned int> h_CellParticleCounts(numberOfCells);
    //thrust::copy(thrust::device_pointer_cast(d_CellParticleCounts), thrust::device_pointer_cast(d_CellParticleCounts) + numberOfCells, h_CellParticleCounts.begin());

    unsigned int threadsPerBlock = 64;
    unsigned int numOfBlocks = numUniqQs / threadsPerBlock + 1;
    kCalcSearchSize(numOfBlocks,
                    threadsPerBlock,
                    gridInfo,
                    morton, 
                    d_CellParticleCounts,
                    d_repQueries,
                    particles,
                    cellSize,
                    maxWidth,
                    state.knn,
                    thrust::raw_pointer_cast(d_cellMask)
                   );

    //thrust::host_vector<int> h_cellMask_t(numberOfCells);
    //thrust::copy(d_cellMask, d_cellMask + numberOfCells, h_cellMask_t.begin());
  } else {
    thrust::host_vector<unsigned int> h_part_seq(numUniqQs);
    thrust::copy(thrust::device_pointer_cast(d_repQueries), thrust::device_pointer_cast(d_repQueries) + numUniqQs, h_part_seq.begin());

    thrust::host_vector<unsigned int> h_CellParticleCounts(numberOfCells);
    thrust::copy(thrust::device_pointer_cast(d_CellParticleCounts), thrust::device_pointer_cast(d_CellParticleCounts) + numberOfCells, h_CellParticleCounts.begin());

    thrust::host_vector<int> h_cellMask(numberOfCells);

    for (unsigned int i = 0; i < numUniqQs; i++) {
      unsigned int qId = h_part_seq[i];
      float3 point = state.h_points[qId];
      float3 gridCellF = (point - gridInfo.GridMin) * gridInfo.GridDelta;
      int3 gridCell = make_int3(int(gridCellF.x), int(gridCellF.y), int(gridCellF.z));

      calcSearchSize(gridCell,
                     gridInfo,
                     morton,
                     h_CellParticleCounts.data(),
                     cellSize,
                     maxWidth,
                     state.knn,
                     h_cellMask.data()
                    );
    }
    thrust::copy(h_cellMask.begin(), h_cellMask.end(), d_cellMask);
  }

  //for (unsigned int i = 0; i < numberOfCells; i++) {
  //  if (h_cellMask[i] == 2)
  //    printf("%u, %u, %x\n", i, h_cellMask[i], h_cellMask_t[i]);
  //}

  return d_cellMask;
}

void prepBatches(std::vector<int>& batches, const thrust::host_vector<unsigned int> h_rayHist) {
  unsigned int numMasks = h_rayHist.size();
  for (unsigned int i = 0; i < h_rayHist.size(); i++) {
    batches.push_back(i);
    //if (i == 0 || i == numMasks - 1) batches.push_back(i);
    //if (i <= 1 || i == numMasks - 1) batches.push_back(i);
  }
  assert(batches.size() <= numMasks);

  //int maxIter = (int)floorf(maxWidth / (2 * cellSize) - 1);
  //int histCount = maxIter + 3; // 0: empty cell counts; 1 -- maxIter+1: real counts; maxIter+2: full search counts.
  //state.numOfBatches = histCount - 1;
}

void genBatches(WhittedState& state,
                std::vector<int>& batches,
                thrust::host_vector<unsigned int> h_rayHist,
                float3* particles,
                unsigned int N,
                thrust::device_ptr<int> d_rayMask)
{
  float cellSize = state.radius / state.crRatio;

  int lastMask = -1;
  for (int batchId = 0; batchId < state.numOfBatches; batchId++) {
    int maxMask = batches[batchId];
    unsigned int numActQs = 0;
    for (int j = lastMask + 1; j <= maxMask; j++) {
      numActQs += h_rayHist[j];
    }
    state.numActQueries[batchId] = numActQs;
    //printf("[%d, %d]: %u\n", lastMask + 1, maxMask, numActQs);

    // see comments in how maxWidth is calculated in |genCellMask|.
    float partThd = kGetWidthFromIter(maxMask, cellSize); // partThd depends on the max mask.
    if (state.searchMode == "knn")
      state.launchRadius[batchId] = (float)(partThd / 2 * sqrt(2));
    else
      state.launchRadius[batchId] = partThd / 2;
    if (batchId == (state.numOfBatches - 1)) state.launchRadius[batchId] = state.radius;
    //printf("%u, %f\n", maxMask, state.launchRadius[batchId]);

    // can't free |particles|, because it points to the points too.
    // same applies to state.h_queries. |particles| from this point
    // on will only be used to point to device queries used in kernels, and
    // will be set right before launch using d_actQs.
    thrust::device_ptr<float3> d_actQs;
    allocThrustDevicePtr(&d_actQs, numActQs);
    copyIfIdInRange(particles, N, d_rayMask, d_actQs, lastMask + 1, maxMask);
    state.d_actQs[batchId] = thrust::raw_pointer_cast(d_actQs);

    // Copy the active queries to host (for sanity check).
    state.h_actQs[batchId] = new float3[numActQs];
    thrust::copy(d_actQs, d_actQs + numActQs, state.h_actQs[batchId]);

    lastMask = maxMask;
  }
}

void sortGenBatch(WhittedState& state,
                  unsigned int N,
                  bool morton,
                  unsigned int numberOfCells,
                  unsigned int numOfBlocks,
                  unsigned int threadsPerBlock,
                  GridInfo gridInfo,
                  float3* particles,
                  thrust::device_ptr<unsigned int> d_CellParticleCounts_ptr,
                  thrust::device_ptr<unsigned int> d_ParticleCellIndices_ptr,
                  thrust::device_ptr<unsigned int> d_CellOffsets_ptr,
                  thrust::device_ptr<unsigned int> d_LocalSortedIndices_ptr,
                  thrust::device_ptr<unsigned int> d_posInSortedPoints_ptr
                 )
{
    // pick one particle from each cell, and store all their indices in |d_repQueries|
    thrust::device_ptr<unsigned int> d_ParticleCellIndices_ptr_copy;
    allocThrustDevicePtr(&d_ParticleCellIndices_ptr_copy, N);
    thrustCopyD2D(d_ParticleCellIndices_ptr_copy, d_ParticleCellIndices_ptr, N);
    thrust::device_ptr<unsigned int> d_repQueries;
    allocThrustDevicePtr(&d_repQueries, N);
    genSeqDevice(d_repQueries, N);
    sortByKey(d_ParticleCellIndices_ptr_copy, d_repQueries, N);
    unsigned int numUniqQs = uniqueByKey(d_ParticleCellIndices_ptr_copy, N, d_repQueries);
    fprintf(stdout, "\tNum of Rep queries: %u\n", numUniqQs);

    thrust::device_ptr<int> d_cellMask = genCellMask(state,
            thrust::raw_pointer_cast(d_repQueries),
            particles,
            thrust::raw_pointer_cast(d_CellParticleCounts_ptr),
            numberOfCells,
            gridInfo,
            N,
            numUniqQs,
            morton
           );

    thrust::device_ptr<int> d_rayMask;
    allocThrustDevicePtr(&d_rayMask, N);

    // generate the sorted indices, and also set the rayMask according to cellMask.
    kCountingSortIndices_setRayMask(numOfBlocks,
                                    threadsPerBlock,
                                    gridInfo,
                                    thrust::raw_pointer_cast(d_ParticleCellIndices_ptr),
                                    thrust::raw_pointer_cast(d_CellOffsets_ptr),
                                    thrust::raw_pointer_cast(d_LocalSortedIndices_ptr),
                                    thrust::raw_pointer_cast(d_posInSortedPoints_ptr),
                                    thrust::raw_pointer_cast(d_cellMask),
                                    thrust::raw_pointer_cast(d_rayMask)
                                   );

    // make a copy of the keys since they are useless after the first sort. no
    // need to use stable sort since the keys are unique, so masks and the
    // queries are gauranteed to be sorted in exactly the same way.
    // TODO: Can we do away with th extra copy by replacing sort by key with scatter? That'll need new space too...
    thrust::device_ptr<unsigned int> d_posInSortedPoints_ptr_copy;
    allocThrustDevicePtr(&d_posInSortedPoints_ptr_copy, N);
    thrustCopyD2D(d_posInSortedPoints_ptr_copy, d_posInSortedPoints_ptr, N);

    //thrust::host_vector<int> h_rayMask(N);
    //thrust::copy(d_rayMask, d_rayMask + N, h_rayMask.begin());
    //thrust::host_vector<unsigned int> h_ParticleCellIndices(N);
    //thrust::copy(d_ParticleCellIndices_ptr, d_ParticleCellIndices_ptr + N, h_ParticleCellIndices.begin());
    //for (unsigned int i = 0; i < N; i++) {
    //  float3 query = state.h_queries[i];
    //  if (isClose(query, make_float3(-57.230999, 2.710000, 9.608000))) {
    //    printf("particle [%f, %f, %f], %d, in cell %u\n", query.x, query.y, query.z, h_rayMask[i], h_ParticleCellIndices[i]);
    //    break;
    //  }
    //}
    //exit(1);

    // get a histogram of d_rayMask, which won't be mutated. this needs to happen before sorting |d_rayMask|.
    // the last mask in the histogram indicates full search.
    thrust::device_vector<unsigned int> d_rayHist;
    unsigned int numMasks = thrustGenHist(d_rayMask, d_rayHist, N);
    thrust::host_vector<unsigned int> h_rayHist(numMasks);
    thrust::copy(d_rayHist.begin(), d_rayHist.end(), h_rayHist.begin());

    // sort the ray masks the same way as query sorting.
    sortByKey(d_posInSortedPoints_ptr_copy, d_rayMask, N);
    // this MUST happen right after sorting the masks and before copy so that the queries and the masks are consistent!!!
    sortByKey(d_posInSortedPoints_ptr, thrust::device_pointer_cast(particles), N);
    CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_posInSortedPoints_ptr_copy) ) );

    // TODO: does it make sense to do non-consecutive batches?
    // |batches| will contain the last mask of each batch.
    std::vector<int> batches;
    prepBatches(batches, h_rayHist);
    state.numOfBatches = batches.size();
    fprintf(stdout, "\tNumber of batches: %d\n", state.numOfBatches);

    genBatches(state, batches, h_rayHist, particles, N, d_rayMask);

    CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_rayMask) ) );
    CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_cellMask) ) );
}

void gridSort(WhittedState& state, ParticleType type, bool morton) {
  unsigned int N;
  float3* particles;
  float3* h_particles;
  if (type == POINT) {
    N = state.numPoints;
    particles = state.params.points;
    h_particles = state.h_points;
  } else {
    N = state.numQueries;
    particles = state.params.queries;
    h_particles = state.h_queries;
  }

  GridInfo gridInfo;
  unsigned int numberOfCells = genGridInfo(state, N, gridInfo);

  thrust::device_ptr<unsigned int> d_ParticleCellIndices_ptr;
  allocThrustDevicePtr(&d_ParticleCellIndices_ptr, N);
  thrust::device_ptr<unsigned int> d_CellParticleCounts_ptr;
  allocThrustDevicePtr(&d_CellParticleCounts_ptr, numberOfCells); // this takes a lot of memory
  fillByValue(d_CellParticleCounts_ptr, numberOfCells, 0);
  thrust::device_ptr<unsigned int> d_LocalSortedIndices_ptr;
  allocThrustDevicePtr(&d_LocalSortedIndices_ptr, N);

  unsigned int threadsPerBlock = 64;
  unsigned int numOfBlocks = N / threadsPerBlock + 1;
  kInsertParticles(numOfBlocks,
                   threadsPerBlock,
                   gridInfo,
                   particles,
                   thrust::raw_pointer_cast(d_ParticleCellIndices_ptr),
                   thrust::raw_pointer_cast(d_CellParticleCounts_ptr),
                   thrust::raw_pointer_cast(d_LocalSortedIndices_ptr),
                   morton
                  );

  thrust::device_ptr<unsigned int> d_CellOffsets_ptr;
  allocThrustDevicePtr(&d_CellOffsets_ptr, numberOfCells);
  fillByValue(d_CellOffsets_ptr, numberOfCells, 0); // need to initialize it even for exclusive scan
  exclusiveScan(d_CellParticleCounts_ptr, numberOfCells, d_CellOffsets_ptr);

  thrust::device_ptr<unsigned int> d_posInSortedPoints_ptr;
  allocThrustDevicePtr(&d_posInSortedPoints_ptr, N);
  // if samepq and partition is enabled, do it here. we are partitioning points, but it's the same as queries.
  if (state.partition) {
    // normal particle sorting is done here too.
    sortGenBatch(state,
                 N,
                 morton,
                 numberOfCells,
                 numOfBlocks,
                 threadsPerBlock,
                 gridInfo,
                 particles,
                 d_CellParticleCounts_ptr,
                 d_ParticleCellIndices_ptr,
                 d_CellOffsets_ptr,
                 d_LocalSortedIndices_ptr,
                 d_posInSortedPoints_ptr
                );
  } else {
    kCountingSortIndices(numOfBlocks,
                         threadsPerBlock,
                         gridInfo,
                         thrust::raw_pointer_cast(d_ParticleCellIndices_ptr),
                         thrust::raw_pointer_cast(d_CellOffsets_ptr),
                         thrust::raw_pointer_cast(d_LocalSortedIndices_ptr),
                         thrust::raw_pointer_cast(d_posInSortedPoints_ptr)
                        );
    // in-place sort; no new device memory is allocated
    sortByKey(d_posInSortedPoints_ptr, thrust::device_pointer_cast(particles), N);
  }

  // copy particles to host, regardless of partition. for POINT, this makes
  // sure the points in device are consistent with the host points used to
  // build the GAS. for QUERY and POINT, this sets up data for sanity check.
  thrust::device_ptr<float3> d_particles_ptr = thrust::device_pointer_cast(particles);
  thrust::copy(d_particles_ptr, d_particles_ptr + N, h_particles);

  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_ParticleCellIndices_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_posInSortedPoints_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_CellOffsets_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_LocalSortedIndices_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_CellParticleCounts_ptr) ) );
}

void oneDSort ( WhittedState& state, ParticleType type ) {
  // sort points/queries based on coordinates (x/y/z)
  unsigned int N;
  float3* particles;
  float3* h_particles;
  if (type == POINT) {
    N = state.numPoints;
    particles = state.params.points;
    h_particles = state.h_points;
  } else {
    N = state.numQueries;
    particles = state.params.queries;
    h_particles = state.h_queries;
  }

  // TODO: do this whole thing on GPU.
  // create 1d points as the sorting key and upload it to device memory
  thrust::host_vector<float> h_key(N);
  for(unsigned int i = 0; i < N; i++) {
    h_key[i] = h_particles[i].x;
  }

  thrust::device_ptr<float> d_key_ptr;
  state.d_1dsort_key = allocThrustDevicePtr(&d_key_ptr, state.numQueries);
  thrust::copy(h_key.begin(), h_key.end(), d_key_ptr);

  // actual sort
  thrust::device_ptr<float3> d_particles_ptr = thrust::device_pointer_cast(particles);
  sortByKey( d_key_ptr, d_particles_ptr, N );

  // TODO: lift it outside of this function and combine with other sorts?
  // copy the sorted queries to host so that we build the GAS in the same order
  // note that the h_queries at this point still point to what h_points points to
  thrust::copy(d_particles_ptr, d_particles_ptr + N, h_particles);
}

void sortParticles ( WhittedState& state, ParticleType type, int sortMode ) {
  // 0: no sort
  // 1: z-order sort
  // 2: raster sort
  // 3: 1D sort
  if (!sortMode) return;

  // the semantices of the two sort functions are: sort data in device, and copy the sorted data back to host.
  std::string typeName = ((type == POINT) ? "points" : "queries");
  Timing::startTiming("sort " + typeName);
    if (sortMode == 3) {
      oneDSort(state, type);
    } else {
      computeMinMax(state, type);

      bool morton; // false for raster order
      if (sortMode == 1) morton = true;
      else {
        assert(sortMode == 2);
        morton = false;
      }
      gridSort(state, type, morton);
    }
  Timing::stopTiming(true);
}

thrust::device_ptr<unsigned int> sortQueriesByFHCoord( WhittedState& state, thrust::device_ptr<unsigned int> d_firsthit_idx_ptr, int batch_id ) {
  // this is sorting queries by the x/y/z coordinate of the first hit primitives.
  unsigned int numQueries = state.numActQueries[batch_id];

  Timing::startTiming("gas-sort queries init");
    // allocate device memory for storing the keys, which will be generated by a gather and used in sort_by_keys
    thrust::device_ptr<float> d_key_ptr;
    state.d_fhsort_key = allocThrustDevicePtr(&d_key_ptr, numQueries);
  
    // create indices for gather and upload to device
    thrust::host_vector<float> h_orig_points_1d(numQueries);
    // TODO: do this in CUDA
    for (unsigned int i = 0; i < numQueries; i++) {
      h_orig_points_1d[i] = state.h_points[i].z; // could be other dimensions
    }
    thrust::device_vector<float> d_orig_points_1d = h_orig_points_1d;

    // initialize a sequence to be sorted, which will become the r2q map.
    thrust::device_ptr<unsigned int> d_r2q_map_ptr;
    allocThrustDevicePtr(&d_r2q_map_ptr, numQueries);
    genSeqDevice(d_r2q_map_ptr, numQueries, state.stream[batch_id]);
  Timing::stopTiming(true);
  
  Timing::startTiming("gas-sort queries");
    // TODO: do thrust work in a stream: https://forums.developer.nvidia.com/t/thrust-and-streams/53199
    // first use a gather to generate the keys, then sort by keys
    gatherByKey(d_firsthit_idx_ptr, &d_orig_points_1d, d_key_ptr, numQueries, state.stream[batch_id]);
    sortByKey( d_key_ptr, d_r2q_map_ptr, numQueries, state.stream[batch_id] );
    state.d_r2q_map[batch_id] = thrust::raw_pointer_cast(d_r2q_map_ptr);
  Timing::stopTiming(true);
  
  // if debug, copy the sorted keys and values back to host
  bool debug = false;
  if (debug) {
    thrust::host_vector<unsigned int> h_vec_val(numQueries);
    thrust::copy(d_r2q_map_ptr, d_r2q_map_ptr+numQueries, h_vec_val.begin());

    thrust::host_vector<float> h_vec_key(numQueries);
    thrust::copy(d_key_ptr, d_key_ptr+numQueries, h_vec_key.begin());
    
    float3* h_queries = state.h_actQs[batch_id];
    for (unsigned int i = 0; i < h_vec_val.size(); i++) {
      std::cout << h_vec_key[i] << "\t" 
                << h_vec_val[i] << "\t" 
                << h_queries[h_vec_val[i]].x << "\t"
                << h_queries[h_vec_val[i]].y << "\t"
                << h_queries[h_vec_val[i]].z
                << std::endl;
    }
  }

  return d_r2q_map_ptr;
}

thrust::device_ptr<unsigned int> sortQueriesByFHIdx( WhittedState& state, thrust::device_ptr<unsigned int> d_firsthit_idx_ptr, int batch_id ) {
  // this is sorting queries just by the first hit primitive IDs
  unsigned int numQueries = state.numActQueries[batch_id];

  // initialize a sequence to be sorted, which will become the r2q map
  Timing::startTiming("gas-sort queries init");
    thrust::device_ptr<unsigned int> d_r2q_map_ptr;
    allocThrustDevicePtr(&d_r2q_map_ptr, numQueries);
    genSeqDevice(d_r2q_map_ptr, numQueries, state.stream[batch_id]);
  Timing::stopTiming(true);

  Timing::startTiming("gas-sort queries");
    sortByKey( d_firsthit_idx_ptr, d_r2q_map_ptr, numQueries, state.stream[batch_id] );
    // thrust can't be used in kernel code since NVRTC supports only a
    // limited subset of C++, so we would have to explicitly cast a
    // thrust device vector to its raw pointer. See the problem discussed
    // here: https://github.com/cupy/cupy/issues/3728 and
    // https://github.com/cupy/cupy/issues/3408. See how cuNSearch does it:
    // https://github.com/InteractiveComputerGraphics/cuNSearch/blob/master/src/cuNSearchDeviceData.cu#L152
    state.d_r2q_map[batch_id] = thrust::raw_pointer_cast(d_r2q_map_ptr);
    //printf("%d, %p\n", batch_id, state.d_r2q_map[batch_id]);
  Timing::stopTiming(true);

  bool debug = false;
  if (debug) {
    thrust::host_vector<unsigned int> h_vec_val(numQueries);
    thrust::copy(d_r2q_map_ptr, d_r2q_map_ptr+numQueries, h_vec_val.begin());

    thrust::host_vector<unsigned int> h_vec_key(numQueries);
    thrust::copy(d_firsthit_idx_ptr, d_firsthit_idx_ptr+numQueries, h_vec_key.begin());

    float3* h_queries = state.h_actQs[batch_id];
    for (unsigned int i = 0; i < h_vec_val.size(); i++) {
      std::cout << h_vec_key[i] << "\t"
                << h_vec_val[i] << "\t"
                << h_queries[h_vec_val[i]].x << "\t"
                << h_queries[h_vec_val[i]].y << "\t"
                << h_queries[h_vec_val[i]].z
                << std::endl;
    }
  }

  return d_r2q_map_ptr;
}

void gatherQueries( WhittedState& state, thrust::device_ptr<unsigned int> d_indices_ptr, int batch_id ) {
  // Perform a device gather before launching the actual search, which by
  // itself is not useful, since we access each query only once (in the RG
  // program) anyways. in reality we see little gain by gathering queries. but
  // if queries and points point to the same device memory, gathering queries
  // effectively reorders the points too. we access points in the IS program
  // (get query origin using the hit primIdx), and so it would be nice to
  // coalesce memory by reordering points. but note two things. First, we
  // access only one point and only once in each IS program and the bulk of
  // memory access is to the BVH which is out of our control, so better memory
  // coalescing has less effect than in traditional grid search. Second, if the
  // points are already sorted in a good order (raster scan or z-order), this
  // reordering has almost zero effect. empirically, we get 10% search time
  // reduction for large point clouds and the points originally are poorly
  // ordered. but this comes at a chilling overhead that we need to rebuild the
  // GAS (to make sure the ID of a box in GAS is the ID of the sphere in device
  // memory; otherwise IS program is in correct), which is on the critical path
  // and whose overhead can't be hidden. so almost always this optimization
  // leads to performance degradation, both |toGather| and |reorderPoints| are
  // disabled by default. |reorderPoints| are now removed.

  Timing::startTiming("gather queries");
    unsigned int numQueries = state.numActQueries[batch_id];

    // allocate device memory for reordered/gathered queries
    thrust::device_ptr<float3> d_reord_queries_ptr;
    allocThrustDevicePtr(&d_reord_queries_ptr, numQueries);

    // get pointer to original queries in device memory
    thrust::device_ptr<float3> d_orig_queries_ptr = thrust::device_pointer_cast(state.d_actQs[batch_id]);

    // gather by key, which is generated by the previous sort
    gatherByKey(d_indices_ptr, d_orig_queries_ptr, d_reord_queries_ptr, numQueries, state.stream[batch_id]);

    // if not samepq or partition is enabled (which will copy queries), then we can free the original query device memory
    if (!state.samepq || state.partition) CUDA_CHECK( cudaFree( (void*)state.d_actQs[batch_id] ) );
    state.d_actQs[batch_id] = thrust::raw_pointer_cast(d_reord_queries_ptr);
    //assert(state.params.points != state.params.queries);
  Timing::stopTiming(true);

  // Copy reordered queries to host for sanity check
  // if not samepq, free the original query host memory first
  if (!state.samepq || state.partition) delete state.h_actQs[batch_id];
  state.h_actQs[batch_id] = new float3[numQueries]; // don't overwrite h_points
  thrust::host_vector<float3> host_reord_queries(numQueries);
  thrust::copy(d_reord_queries_ptr, d_reord_queries_ptr+numQueries, state.h_actQs[batch_id]);
  //assert (state.h_points != state.h_queries);
}
