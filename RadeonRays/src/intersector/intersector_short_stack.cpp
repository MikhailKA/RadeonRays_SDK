/**********************************************************************
 Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ********************************************************************/
#include "intersector_short_stack.h"

#include "calc.h"
#include "executable.h"
#include "../accelerator/bvh.h"
#include "../accelerator/split_bvh.h"
#include "../primitive/mesh.h"
#include "../primitive/instance.h"
#include "../world/world.h"

#include "../translator/fatnode_bvh_translator.h"
#include "../except/except.h"

#include <algorithm>

// Preferred work group size for Radeon devices
static int const kWorkGroupSize = 64;
static int const kMaxStackSize = 48;
static int const kMaxBatchSize = 1024 * 1024;

namespace RadeonRays
{
    struct IntersectorShortStack::GpuData
    {
        // Device
        Calc::Device* device;
        // BVH nodes
        Calc::Buffer* bvh;
        // Vertex positions
        Calc::Buffer* vertices;
        // Traversal stack
        Calc::Buffer* stack;

        Calc::Executable* executable;
        Calc::Function* isect_func;
        Calc::Function* occlude_func;

        GpuData(Calc::Device* d)
            : device(d)
            , bvh(nullptr)
            , vertices(nullptr)
            , stack(nullptr)
            , executable(nullptr)
            , isect_func(nullptr)
            , occlude_func(nullptr)
        {
        }

        ~GpuData()
        {
            device->DeleteBuffer(bvh);
            device->DeleteBuffer(vertices);
            device->DeleteBuffer(stack);
            executable->DeleteFunction(isect_func);
            executable->DeleteFunction(occlude_func);
            device->DeleteExecutable(executable);
        }
    };

    IntersectorShortStack::IntersectorShortStack(Calc::Device* device)
        : Intersector(device)
        , m_gpudata(new GpuData(device))
        , m_bvh(nullptr)
    {
        std::string buildopts;
#ifdef RR_RAY_MASK
        buildopts.append("-D RR_RAY_MASK ");
#endif

#ifdef RR_BACKFACE_CULL
        buildopts.append("-D RR_BACKFACE_CULL ");
#endif // RR_BACKFACE_CULL

#ifdef USE_SAFE_MATH
        buildopts.append("-D USE_SAFE_MATH ");
#endif

#ifndef RR_EMBED_KERNELS
        if (device->GetPlatform() == Calc::Platform::kOpenCL)
        {
            char const* headers[] = { "../RadeonRays/src/kernels/CL/common.cl" };

            int numheaders = sizeof(headers) / sizeof(char const*);

            m_gpudata->executable = m_device->CompileExecutable("../RadeonRays/src/kernels/CL/intersect_bvh2_short_stack.cl", headers, numheaders, buildopts.c_str());
        } 
        else
        {
            assert(device->GetPlatform() == Calc::Platform::kVulkan);
            m_gpudata->executable = m_device->CompileExecutable("../RadeonRays/src/kernels/GLSL/fatbvh.comp", nullptr, 0, buildopts.c_str());
        }
#else
#if USE_OPENCL
        if (device->GetPlatform() == Calc::Platform::kOpenCL)
        {
            m_gpudata->executable = m_device->CompileExecutable(g_intersect_bvh2_short_stack_opencl, std::strlen(g_intersect_bvh2_short_stack_opencl), buildopts.c_str());
        }
#endif

#if USE_VULKAN
        if (m_gpudata->executable == nullptr && device->GetPlatform() == Calc::Platform::kVulkan)
        {
            m_gpudata->executable = m_device->CompileExecutable(g_fatbvh_vulkan, std::strlen(g_fatbvh_vulkan), buildopts.c_str());
        }
#endif

#endif

        m_gpudata->isect_func = m_gpudata->executable->CreateFunction("intersect_main");
        m_gpudata->occlude_func = m_gpudata->executable->CreateFunction("occluded_main");
    }

    void IntersectorShortStack::Process(World const& world)
    {

        // If something has been changed we need to rebuild BVH
        if (!m_bvh || world.has_changed() || world.GetStateChange() != ShapeImpl::kStateChangeNone)
        {
            if (m_bvh)
            {
                m_device->DeleteBuffer(m_gpudata->bvh);
                m_device->DeleteBuffer(m_gpudata->vertices);
            }

            // Check if we can allocate enough stack memory
            Calc::DeviceSpec spec;
            m_device->GetSpec(spec);
            if (spec.max_alloc_size <= kMaxBatchSize * kMaxStackSize * sizeof(int))
            {
                throw ExceptionImpl("fatbvh accelerator can't allocate enough stack memory, try using bvh instead");
            }

            int numshapes = (int)world.shapes_.size();
            int numvertices = 0;
            int numfaces = 0;

            // This buffer tracks mesh start index for next stage as mesh face indices are relative to 0
            std::vector<int> mesh_vertices_start_idx(numshapes);
            std::vector<int> mesh_faces_start_idx(numshapes);

            auto builder = world.options_.GetOption("bvh.builder");
            auto splits = world.options_.GetOption("bvh.sah.use_splits");
            auto maxdepth = world.options_.GetOption("bvh.sah.max_split_depth");
            auto overlap = world.options_.GetOption("bvh.sah.min_overlap");
            auto tcost = world.options_.GetOption("bvh.sah.traversal_cost");
            auto node_budget = world.options_.GetOption("bvh.sah.extra_node_budget");
            auto nbins = world.options_.GetOption("bvh.sah.num_bins");

            bool use_sah = false;
            bool use_splits = false;
            int max_split_depth = maxdepth ? (int)maxdepth->AsFloat() : 10;
            int num_bins = nbins ? (int)nbins->AsFloat() : 64;
            float min_overlap = overlap ? overlap->AsFloat() : 0.05f;
            float traversal_cost = tcost ? tcost->AsFloat() : 10.f;
            float extra_node_budget = node_budget ? node_budget->AsFloat() : 0.5f;

            if (builder && builder->AsString() == "sah")
            {
                use_sah = true;
            }

            if (splits && splits->AsFloat() > 0.f)
            {
                use_splits = true;
            }

            m_bvh.reset(use_splits ?
                new SplitBvh(traversal_cost, num_bins, max_split_depth, min_overlap, extra_node_budget) :
                new Bvh(traversal_cost, num_bins, use_sah)
            );

            // Partition the array into meshes and instances
            std::vector<Shape const*> shapes(world.shapes_);

            auto firstinst = std::partition(shapes.begin(), shapes.end(),
                [&](Shape const* shape)
            {
                return !static_cast<ShapeImpl const*>(shape)->is_instance();
            });

            // Count the number of meshes
            int nummeshes = (int)std::distance(shapes.begin(), firstinst);
            // Count the number of instances
            int numinstances = (int)std::distance(firstinst, shapes.end());

            for (int i = 0; i < nummeshes; ++i)
            {
                Mesh const* mesh = static_cast<Mesh const*>(shapes[i]);

                mesh_faces_start_idx[i] = numfaces;
                mesh_vertices_start_idx[i] = numvertices;

                numfaces += mesh->num_faces();
                numvertices += mesh->num_vertices();
            }

            for (int i = nummeshes; i < nummeshes + numinstances; ++i)
            {
                Instance const* instance = static_cast<Instance const*>(shapes[i]);
                Mesh const* mesh = static_cast<Mesh const*>(instance->GetBaseShape());

                mesh_faces_start_idx[i] = numfaces;
                mesh_vertices_start_idx[i] = numvertices;

                numfaces += mesh->num_faces();
                numvertices += mesh->num_vertices();
            }

            // We can't avoid allocating it here, since bounds aren't stored anywhere
            std::vector<bbox> bounds(numfaces);

            // We handle meshes first collecting their world space bounds
#pragma omp parallel for
            for (int i = 0; i < nummeshes; ++i)
            {
                Mesh const* mesh = static_cast<Mesh const*>(shapes[i]);

                for (int j = 0; j < mesh->num_faces(); ++j)
                {
                    // Here we directly get world space bounds
                    mesh->GetFaceBounds(j, false, bounds[mesh_faces_start_idx[i] + j]);
                }
            }

            // Then we handle instances. Need to flatten them into actual geometry.
#pragma omp parallel for
            for (int i = nummeshes; i < nummeshes + numinstances; ++i)
            {
                Instance const* instance = static_cast<Instance const*>(shapes[i]);
                Mesh const* mesh = static_cast<Mesh const*>(instance->GetBaseShape());

                // Instance is using its own transform for base shape geometry
                // so we need to get object space bounds and transform them manually
                matrix m, minv;
                instance->GetTransform(m, minv);

                for (int j = 0; j < mesh->num_faces(); ++j)
                {
                    bbox tmp;
                    mesh->GetFaceBounds(j, true, tmp);
                    bounds[mesh_faces_start_idx[i] + j] = transform_bbox(tmp, m);
                }
            }

            m_bvh->Build(&bounds[0], numfaces);

#ifdef RR_PROFILE
            m_bvh->PrintStatistics(std::cout);
#endif

            // Check if the tree height is reasonable
            if (m_bvh->GetHeight() >= kMaxStackSize)
            {
                m_bvh.reset(nullptr);
                throw ExceptionImpl("fatbvh accelerator can cause stack overflow for this scene, try using bvh instead");
            }

            FatNodeBvhTranslator translator;
            translator.Process(*m_bvh);

            // Update GPU data

            // Create vertex buffer
            {
                // Vertices
                m_gpudata->vertices = m_device->CreateBuffer(numvertices * sizeof(float3), Calc::BufferType::kRead);

                // Get the pointer to mapped data
                float3* vertexdata = nullptr;
                Calc::Event* e = nullptr;

                m_device->MapBuffer(m_gpudata->vertices, 0, 0, numvertices * sizeof(float3), Calc::MapType::kMapWrite, (void**)&vertexdata, &e);

                e->Wait();
                m_device->DeleteEvent(e);

                // Here we need to put data in world space rather than object space
                // So we need to get the transform from the mesh and multiply each vertex
                matrix m, minv;

#pragma omp parallel for
                for (int i = 0; i < nummeshes; ++i)
                {
                    // Get the mesh
                    Mesh const* mesh = static_cast<Mesh const*>(shapes[i]);
                    // Get vertex buffer of the current mesh
                    float3 const* myvertexdata = mesh->GetVertexData();
                    // Get mesh transform
                    mesh->GetTransform(m, minv);

                    //#pragma omp parallel for
                    // Iterate thru vertices multiply and append them to GPU buffer
                    for (int j = 0; j < mesh->num_vertices(); ++j)
                    {
                        vertexdata[mesh_vertices_start_idx[i] + j] = transform_point(myvertexdata[j], m);
                    }
                }

#pragma omp parallel for
                for (int i = nummeshes; i < nummeshes + numinstances; ++i)
                {
                    Instance const* instance = static_cast<Instance const*>(shapes[i]);
                    // Get the mesh
                    Mesh const* mesh = static_cast<Mesh const*>(instance->GetBaseShape());
                    // Get vertex buffer of the current mesh
                    float3 const* myvertexdata = mesh->GetVertexData();
                    // Get mesh transform
                    instance->GetTransform(m, minv);

                    //#pragma omp parallel for
                    // Iterate thru vertices multiply and append them to GPU buffer
                    for (int j = 0; j < mesh->num_vertices(); ++j)
                    {
                        vertexdata[mesh_vertices_start_idx[i] + j] = transform_point(myvertexdata[j], m);
                    }
                }

                m_device->UnmapBuffer(m_gpudata->vertices, 0, vertexdata, &e);
                e->Wait();
                m_device->DeleteEvent(e);
            }

            // Create face buffer
            {
                
                // This number is different from the number of faces for some BVHs 
                auto numindices = m_bvh->GetNumIndices();
                std::vector<FatNodeBvhTranslator::Face> facedata(numindices);

                // Here the point is to add mesh starting index to actual index contained within the mesh,
                // getting absolute index in the buffer.
                // Besides that we need to permute the faces accorningly to BVH reordering, whihc
                // is contained within bvh.primids_
                int const* reordering = m_bvh->GetIndices();
                for (int i = 0; i < numindices; ++i)
                {
                    int indextolook4 = reordering[i];

                    // We need to find a shape corresponding to current face
                    auto iter = std::upper_bound(mesh_faces_start_idx.cbegin(), mesh_faces_start_idx.cend(), indextolook4);

                    // Find the index of the shape
                    int shapeidx = static_cast<int>(std::distance(mesh_faces_start_idx.cbegin(), iter) - 1);

                    // Get the mesh directly or out of instance
                    Mesh const* mesh = nullptr;
                    if (shapeidx < nummeshes)
                    {
                        mesh = static_cast<Mesh const*>(shapes[shapeidx]);
                    }
                    else
                    {
                        mesh = static_cast<Mesh const*>(static_cast<Instance const*>(shapes[shapeidx])->GetBaseShape());
                    }

                    // Get vertex buffer of the current mesh
                    Mesh::Face const* myfacedata = mesh->GetFaceData();
                    // Find face idx
                    int faceidx = indextolook4 - mesh_faces_start_idx[shapeidx];
                    // Find mesh start idx
                    int mystartidx = mesh_vertices_start_idx[shapeidx];

                    // Copy face data to GPU buffer
                    facedata[i].idx[0] = myfacedata[faceidx].idx[0] + mystartidx;
                    facedata[i].idx[1] = myfacedata[faceidx].idx[1] + mystartidx;
                    facedata[i].idx[2] = myfacedata[faceidx].idx[2] + mystartidx;

                    facedata[i].shapeidx = shapes[shapeidx]->GetId();
                    facedata[i].id = faceidx;
                }

                translator.InjectIndices(&facedata[0]);
            }

            // Copy translated nodes first
            m_gpudata->bvh = m_device->CreateBuffer(translator.nodes_.size() * sizeof(FatNodeBvhTranslator::Node), Calc::BufferType::kRead, &translator.nodes_[0]);

            // Stack
            if (m_gpudata->stack)
            {
                m_device->DeleteBuffer(m_gpudata->stack);
                m_gpudata->stack = nullptr;
            }
            m_gpudata->stack = m_device->CreateBuffer(kMaxBatchSize*kMaxStackSize, Calc::BufferType::kWrite);

            // Make sure everything is commited
            m_device->Finish(0);
        }
    }

    void IntersectorShortStack::Intersect(std::uint32_t queueidx, Calc::Buffer const* rays, Calc::Buffer const* numrays, std::uint32_t maxrays, Calc::Buffer* hits, Calc::Event const* waitevent, Calc::Event** event) const
    {
        size_t stack_size = 4 * maxrays * kMaxStackSize; //required stack size, kMaxStackSize * sizeof(int) bytes per ray
        // Check if we need to relocate memory
        if (stack_size > m_gpudata->stack->GetSize())
        {
            m_device->DeleteBuffer(m_gpudata->stack);
            m_gpudata->stack = nullptr;
            m_gpudata->stack = m_device->CreateBuffer(stack_size, Calc::BufferType::kWrite);
        }

        auto& func = m_gpudata->isect_func;

        // Set args
        int arg = 0;

        func->SetArg(arg++, m_gpudata->bvh);
        func->SetArg(arg++, m_gpudata->vertices);
        func->SetArg(arg++, rays);
        func->SetArg(arg++, numrays);
        func->SetArg(arg++, m_gpudata->stack);
        func->SetArg(arg++, hits);

        size_t localsize = kWorkGroupSize;
        size_t globalsize = ((maxrays + kWorkGroupSize - 1) / kWorkGroupSize) * kWorkGroupSize;

        m_device->Execute(func, queueidx, globalsize, localsize, event);
    }

    void IntersectorShortStack::Occluded(std::uint32_t queueidx, Calc::Buffer const* rays, Calc::Buffer const* numrays, std::uint32_t maxrays, Calc::Buffer* hits, Calc::Event const* waitevent, Calc::Event** event) const
    {
        size_t stack_size = 4 * maxrays * kMaxStackSize; //required stack size, kMaxStackSize * sizeof(int) bytes per ray
        // Check if we need to relocate memory
        if (stack_size > m_gpudata->stack->GetSize())
        {
            m_device->DeleteBuffer(m_gpudata->stack);
            m_gpudata->stack = nullptr;
            m_gpudata->stack = m_device->CreateBuffer(stack_size, Calc::BufferType::kWrite);
        }

        auto& func = m_gpudata->occlude_func;

       // Set args
        int arg = 0;

        func->SetArg(arg++, m_gpudata->bvh);
        func->SetArg(arg++, m_gpudata->vertices);
        func->SetArg(arg++, rays);
        func->SetArg(arg++, numrays);
        func->SetArg(arg++, m_gpudata->stack);
        func->SetArg(arg++, hits);

        size_t localsize = kWorkGroupSize;
        size_t globalsize = ((maxrays + kWorkGroupSize - 1) / kWorkGroupSize) * kWorkGroupSize;

        m_device->Execute(func, queueidx, globalsize, localsize, event);
    }
}
