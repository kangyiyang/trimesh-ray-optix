/**
 * @file ray.cpp
 * @author helmholtz
 * @brief evrything per-instance
 *
 */

#include "ray.h"
#include "ATen/core/TensorBody.h"
#include "CUDABuffer.h"
#include "LaunchParams.h"
#include "base.h"
#include "c10/core/ScalarType.h"
#include "c10/core/TensorOptions.h"
#include "c10/util/ArrayRef.h"
#include "optix8.h"
#include "optix_host.h"
#include "optix_types.h"
#include "sbtdef.h"
#include "type.h"

namespace hmesh {

void OptixAccelStructureWrapperCPP::buildAccelStructure(torch::Tensor vertices,
                                                        torch::Tensor faces) {
    OptixAccelBuildOptions buildOptions = {};
    OptixBuildInput buildInput = {};

    // CUdeviceptr tempBuffer, outputBuffer;
    size_t tempBufferSizeInBytes, outputBufferSizeInBytes;

    buildOptions.buildFlags =
        OPTIX_BUILD_FLAG_NONE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    buildOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
    buildOptions.motionOptions.numKeys = 1;

    CUdeviceptr pVert = (CUdeviceptr)vertices.data_ptr();
    CUdeviceptr pFace = (CUdeviceptr)faces.data_ptr();

    buildInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    buildInput.triangleArray.vertexBuffers = &pVert;
    buildInput.triangleArray.numVertices = vertices.size(0);
    buildInput.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    buildInput.triangleArray.vertexStrideInBytes = sizeof(vec3f);
    buildInput.triangleArray.indexBuffer = pFace;
    buildInput.triangleArray.numIndexTriplets = faces.size(0);
    buildInput.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    buildInput.triangleArray.indexStrideInBytes = sizeof(vec3i);
    buildInput.triangleArray.preTransform = 0;

    buildInput.triangleArray.numSbtRecords = 1;
    buildInput.triangleArray.sbtIndexOffsetBuffer = 0;
    buildInput.triangleArray.sbtIndexOffsetSizeInBytes = 0;
    buildInput.triangleArray.sbtIndexOffsetStrideInBytes = 0;

    uint32_t triangleBuildFlags = 0;
    buildInput.triangleArray.flags = &triangleBuildFlags;

    OptixAccelBufferSizes bufferSizes = {};
    OPTIX_CHECK(optixAccelComputeMemoryUsage(optixContext, &buildOptions,
                                             &buildInput, 1, &bufferSizes));

    CUDABuffer tempBuffer;
    CUDABuffer accelStructureBuffer;
    accelStructureBuffer.alloc(bufferSizes.outputSizeInBytes);
    tempBuffer.alloc(bufferSizes.tempSizeInBytes);

    CUDABuffer compactedSizeBuffer;
    compactedSizeBuffer.alloc(sizeof(uint64_t));
    OptixAccelEmitDesc emitDesc;
    emitDesc.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emitDesc.result = compactedSizeBuffer.d_pointer();

    OPTIX_CHECK(optixAccelBuild(
        optixContext, cuStream, &buildOptions, &buildInput, 1,
        (CUdeviceptr)tempBuffer.d_ptr, tempBuffer.sizeInBytes,
        (CUdeviceptr)accelStructureBuffer.d_ptr,
        accelStructureBuffer.sizeInBytes, &asHandle, &emitDesc, 1));

    CUDA_SYNC_CHECK();

    uint64_t compactedSize;
    compactedSizeBuffer.download(&compactedSize, 1);
    asBuffer.resize(compactedSize);

    OPTIX_CHECK(optixAccelCompact(optixContext, cuStream, asHandle,
                                  asBuffer.d_pointer(), compactedSize,
                                  &asHandle));

    CUDA_SYNC_CHECK();

    compactedSizeBuffer.free();
    tempBuffer.free();
    accelStructureBuffer.free();
}

void OptixAccelStructureWrapperCPP::freeAccelStructure() { asBuffer.free(); }

template <typename... Ts> inline bool tensorInputCheck(Ts... ts) {
    bool valid = true;
    (
        [&] {
            if (!ts.is_cuda()) {
                std::cerr << "error in file " << __FILE__ << " line "
                          << __LINE__
                          << ": input tensors must reside in cuda device.\n";
                valid = false;
            }
            if (!ts.is_contiguous()) {
                std::cerr << "error in file " << __FILE__ << " line "
                          << __LINE__
                          << ": input tensors must be contiguous.\n";
                valid = false;
            }
        }(),
        ...);
    return valid;
}

torch::Tensor intersectsAny(OptixAccelStructureWrapperCPP as,
                            const torch::Tensor &origins,
                            const torch::Tensor &directions) {
    if (!tensorInputCheck(origins, directions))
        return {};
    // output buffer
    auto options =
        torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA);
    auto resultSize = origins.sizes();
    resultSize = resultSize.slice(0, resultSize.size() - 1);
    auto nray = 1;
    for (auto s : resultSize)
        nray *= s;
    auto result = torch::empty(resultSize, options);
    // fill launch params
    LaunchParams lp = {};
    lp.rays.origins = origins.data_ptr<float>();
    lp.rays.directions = directions.data_ptr<float>();
    lp.rays.nray = nray;
    lp.traversable = as.asHandle;
    lp.results.hit = result.data_ptr<bool>();
    CUDABuffer lpBuffer;
    lpBuffer.alloc_and_upload(&lp, 1);
    optixLaunch(optixPipelines[SBTType::INTERSECTS_ANY], cuStream,
                lpBuffer.d_pointer(), sizeof(LaunchParams),
                &sbts[SBTType::INTERSECTS_ANY], lp.rays.nray, 1, 1);
    lpBuffer.free();
    return result;
}

torch::Tensor intersectsFirst(OptixAccelStructureWrapperCPP as,
                              const torch::Tensor &origins,
                              const torch::Tensor &directions) {
    if (!tensorInputCheck(origins, directions))
        return {};
    // output buffer
    auto options =
        torch::TensorOptions().dtype(torch::kInt).device(torch::kCUDA);
    auto resultSize = origins.sizes();
    resultSize = resultSize.slice(0, resultSize.size() - 1);
    auto nray = 1;
    for (auto s : resultSize)
        nray *= s;
    auto result = torch::empty(resultSize, options);
    // fill launch params
    LaunchParams lp = {};
    lp.rays.origins = origins.data_ptr<float>();
    lp.rays.directions = directions.data_ptr<float>();
    lp.rays.nray = nray;
    lp.traversable = as.asHandle;
    lp.results.triIdx = result.data_ptr<int>();
    CUDABuffer lpBuffer;
    lpBuffer.alloc_and_upload(&lp, 1);
    optixLaunch(optixPipelines[SBTType::INTERSECTS_FIRST], cuStream,
                lpBuffer.d_pointer(), sizeof(LaunchParams),
                &sbts[SBTType::INTERSECTS_FIRST], lp.rays.nray, 1, 1);
    lpBuffer.free();
    return result;
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor>
intersectsClosest(OptixAccelStructureWrapperCPP as, torch::Tensor origins,
                  torch::Tensor dirs) {
    if (!(origins.is_cuda() && dirs.is_cuda())) {
        std::cerr << "error in file " << __FILE__ << " line " << __LINE__
                  << ": input tensors must reside in cuda device.\n";
        return {};
    }
    if (!(origins.is_contiguous() && dirs.is_contiguous())) {
        std::cerr << "error in file " << __FILE__ << " line " << __LINE__
                  << ": input tensors must be contiguous.\n";
        return {};
    }
    return {};
}

} // namespace hmesh
