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
#include "Renderers/PT/ptrenderer.h"
#include "CLW/clwoutput.h"
#include "SceneGraph/scene1.h"
#include "SceneGraph/Collector/collector.h"

#include <numeric>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <algorithm>

#include "Utils/sobol.h"

#ifdef RR_EMBED_KERNELS
#include "./CL/cache/kernels.h"
#endif

namespace Baikal
{
    using namespace RadeonRays;

    static int const kMaxLightSamples = 1;

    struct PtRenderer::PathState
    {
        float4 throughput;
        int volume;
        int flags;
        int extra0;
        int extra1;
    };

    struct PtRenderer::RenderData
    {
        // OpenCL stuff
        CLWBuffer<ray> rays[2];
        CLWBuffer<int> hits;

        CLWBuffer<ray> shadowrays;
        CLWBuffer<int> shadowhits;

        CLWBuffer<Intersection> intersections;
        CLWBuffer<int> compacted_indices;
        CLWBuffer<int> pixelindices[2];
        CLWBuffer<int> iota;

        CLWBuffer<float3> lightsamples;
        CLWBuffer<PathState> paths;
        CLWBuffer<std::uint32_t> random;
        CLWBuffer<std::uint32_t> sobolmat;
        CLWBuffer<int> hitcount;

        CLWProgram program;
        CLWParallelPrimitives pp;

        // RadeonRays stuff
        Buffer* fr_rays[2];
        Buffer* fr_shadowrays;
        Buffer* fr_shadowhits;
        Buffer* fr_hits;
        Buffer* fr_intersections;
        Buffer* fr_hitcount;
        
        Collector mat_collector;
        Collector tex_collector;

        RenderData()
            : fr_shadowrays(nullptr)
            , fr_shadowhits(nullptr)
            , fr_hits(nullptr)
            , fr_intersections(nullptr)
            , fr_hitcount(nullptr)
        {
            fr_rays[0] = nullptr;
            fr_rays[1] = nullptr;
        }
    };

    // Constructor
    PtRenderer::PtRenderer(CLWContext context, int devidx, int num_bounces)
        : m_context(context)
        , m_render_data(new RenderData)
        , m_vidmemws(0)
        , m_scene_controller(context, devidx)
        , m_num_bounces(num_bounces)
        , m_framecnt(0)
    {
        std::string buildopts;

        buildopts.append(" -cl-mad-enable -cl-fast-relaxed-math -cl-std=CL1.2 -I . ");

        buildopts.append(
#if defined(__APPLE__)
            "-D APPLE "
#elif defined(_WIN32) || defined (WIN32)
            "-D WIN32 "
#elif defined(__linux__)
            "-D __linux__ "
#else
            ""
#endif
            );
        
        // Create parallel primitives
        m_render_data->pp = CLWParallelPrimitives(m_context, buildopts.c_str());

        // Load kernels
#ifndef RR_EMBED_KERNELS
        m_render_data->program = CLWProgram::CreateFromFile("../App/CL/integrator_pt.cl", buildopts.c_str(), m_context);
#else
        m_render_data->program = CLWProgram::CreateFromSource(cl_app, std::strlen(cl_integrator_pt), buildopts.c_str(), context);
#endif

        m_render_data->sobolmat = m_context.CreateBuffer<unsigned int>(1024 * 52, CL_MEM_READ_ONLY, &g_SobolMatrices[0]);
    }

    PtRenderer::~PtRenderer()
    {
    }

    Output* PtRenderer::CreateOutput(std::uint32_t w, std::uint32_t h) const
    {
        return new ClwOutput(w, h, m_context);
    }

    void PtRenderer::DeleteOutput(Output* output) const
    {
        delete output;
    }

    void PtRenderer::Clear(RadeonRays::float3 const& val, Output& output) const
    {
        static_cast<ClwOutput&>(output).Clear(val);
        m_framecnt = 0;
    }

    void PtRenderer::SetNumBounces(int num_bounces)
    {
        m_num_bounces = num_bounces;
    }

    // Render the scene into the output
    void PtRenderer::Render(Scene1 const& scene)
    {
        auto api = m_scene_controller.GetIntersectionApi();
        auto& clwscene = m_scene_controller.CompileScene(scene, m_render_data->mat_collector, m_render_data->tex_collector);

        // Number of rays to generate
        auto output = GetOutput(OutputType::kColor);

        if (output)
        {
            int maxrays = output->width() * output->height();

            // Generate primary
            GeneratePrimaryRays(clwscene, *output);

            // Copy compacted indices to track reverse indices
            m_context.CopyBuffer(0, m_render_data->iota, m_render_data->pixelindices[0], 0, 0, m_render_data->iota.GetElementCount());
            m_context.CopyBuffer(0, m_render_data->iota, m_render_data->pixelindices[1], 0, 0, m_render_data->iota.GetElementCount());
            m_context.FillBuffer(0, m_render_data->hitcount, maxrays, 1);

            // Initialize first pass
            for (int pass = 0; pass < static_cast<int>(m_num_bounces); ++pass)
            {
                // Clear ray hits buffer
                m_context.FillBuffer(0, m_render_data->hits, 0, m_render_data->hits.GetElementCount());

                // Intersect ray batch
                api->QueryIntersection(m_render_data->fr_rays[pass & 0x1], m_render_data->fr_hitcount, maxrays, m_render_data->fr_intersections, nullptr, nullptr);

                // Apply scattering
                EvaluateVolume(clwscene, pass);

                if (pass > 0 && clwscene.envmapidx > -1)
                {
                    ShadeMiss(clwscene, pass);
                }

                // Convert intersections to predicates
                FilterPathStream(pass);

                // Compact batch
                m_render_data->pp.Compact(0, m_render_data->hits, m_render_data->iota, m_render_data->compacted_indices, m_render_data->hitcount);

                // Advance indices to keep pixel indices up to date
                RestorePixelIndices(pass);

                // Shade hits
                ShadeVolume(clwscene, pass);

                // Shade hits
                ShadeSurface(clwscene, pass);

                // Shade missing rays
                if (pass == 0)
                    ShadeBackground(clwscene, pass);

                // Intersect shadow rays
                api->QueryOcclusion(m_render_data->fr_shadowrays, m_render_data->fr_hitcount, maxrays, m_render_data->fr_shadowhits, nullptr, nullptr);

                // Gather light samples and account for visibility
                GatherLightSamples(clwscene, pass);

                //
                m_context.Flush(0);
            }
        }

        // Check if we have other outputs, than color
        bool aov_pass_needed = false;
        for (auto i = 1U; i < static_cast<std::uint32_t>(Renderer::OutputType::kMax); ++i)
        {
            if (GetOutput(static_cast<Renderer::OutputType>(i)))
            {
                aov_pass_needed = true;
                break;
            }
        }

        if (aov_pass_needed)
        {
            FillAOVs(clwscene);
            m_context.Flush(0);
        }

        ++m_framecnt;
    }

    void PtRenderer::SetOutput(OutputType type, Output* output)
    {
        auto current_output = GetOutput(type);
        if (!current_output || current_output->width() < output->width() || current_output->height() < output->height())
        {
            ResizeWorkingSet(*output);
        }

        Renderer::SetOutput(type, output);
    }

    void PtRenderer::ResizeWorkingSet(Output const& output)
    {
        m_vidmemws = 0;

        // Create ray payloads
        m_render_data->rays[0] = m_context.CreateBuffer<ray>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(ray);

        m_render_data->rays[1] = m_context.CreateBuffer<ray>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(ray);

        m_render_data->hits = m_context.CreateBuffer<int>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(int);

        m_render_data->intersections = m_context.CreateBuffer<Intersection>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(Intersection);

        m_render_data->shadowrays = m_context.CreateBuffer<ray>(output.width() * output.height() * kMaxLightSamples, CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(ray)* kMaxLightSamples;

        m_render_data->shadowhits = m_context.CreateBuffer<int>(output.width() * output.height() * kMaxLightSamples, CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(int)* kMaxLightSamples;

        m_render_data->lightsamples = m_context.CreateBuffer<float3>(output.width() * output.height() * kMaxLightSamples, CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(float3)* kMaxLightSamples;

        m_render_data->paths = m_context.CreateBuffer<PathState>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(PathState);

        std::vector<std::uint32_t> random_buffer(output.width() * output.height());
        std::generate(random_buffer.begin(), random_buffer.end(), std::rand);

        m_render_data->random = m_context.CreateBuffer<std::uint32_t>(output.width() * output.height(), CL_MEM_READ_WRITE, &random_buffer[0]);
        m_vidmemws += output.width() * output.height() * sizeof(std::uint32_t);

        std::vector<int> initdata(output.width() * output.height());
        std::iota(initdata.begin(), initdata.end(), 0);
        m_render_data->iota = m_context.CreateBuffer<int>(output.width() * output.height(), CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, &initdata[0]);
        m_vidmemws += output.width() * output.height() * sizeof(int);

        m_render_data->compacted_indices = m_context.CreateBuffer<int>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(int);

        m_render_data->pixelindices[0] = m_context.CreateBuffer<int>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(int);

        m_render_data->pixelindices[1] = m_context.CreateBuffer<int>(output.width() * output.height(), CL_MEM_READ_WRITE);
        m_vidmemws += output.width() * output.height() * sizeof(int);

        m_render_data->hitcount = m_context.CreateBuffer<int>(1, CL_MEM_READ_WRITE);

        auto api = m_scene_controller.GetIntersectionApi();

        // Recreate FR buffers
        api->DeleteBuffer(m_render_data->fr_rays[0]);
        api->DeleteBuffer(m_render_data->fr_rays[1]);
        api->DeleteBuffer(m_render_data->fr_shadowrays);
        api->DeleteBuffer(m_render_data->fr_hits);
        api->DeleteBuffer(m_render_data->fr_shadowhits);
        api->DeleteBuffer(m_render_data->fr_intersections);
        api->DeleteBuffer(m_render_data->fr_hitcount);

        m_render_data->fr_rays[0] = CreateFromOpenClBuffer(api, m_render_data->rays[0]);
        m_render_data->fr_rays[1] = CreateFromOpenClBuffer(api, m_render_data->rays[1]);
        m_render_data->fr_shadowrays = CreateFromOpenClBuffer(api, m_render_data->shadowrays);
        m_render_data->fr_hits = CreateFromOpenClBuffer(api, m_render_data->hits);
        m_render_data->fr_shadowhits = CreateFromOpenClBuffer(api, m_render_data->shadowhits);
        m_render_data->fr_intersections = CreateFromOpenClBuffer(api, m_render_data->intersections);
        m_render_data->fr_hitcount = CreateFromOpenClBuffer(api, m_render_data->hitcount);

        std::cout << "Vidmem usage (working set): " << m_vidmemws / (1024 * 1024) << "Mb\n";
    }

    void PtRenderer::FillAOVs(ClwScene const& scene)
    {
        auto api = m_scene_controller.GetIntersectionApi();

        // Find first non-zero AOV to get buffer dimensions
        auto output = GetOutput(OutputType::kColor);
        if (!output)
        {
            for (auto i = 1U; i < static_cast<std::uint32_t>(Renderer::OutputType::kMax); ++i)
            {
                output = GetOutput(static_cast<Renderer::OutputType>(i));

                if (output)
                {
                    break;
                }
            }
        }

        int num_items = output->width() * output->height();

        // Generate primary
        GeneratePrimaryRays(scene, *output);

         // Intersect ray batch
         api->QueryIntersection(m_render_data->fr_rays[0], num_items, m_render_data->fr_intersections, nullptr, nullptr);

         CLWKernel fill_kernel = m_render_data->program.GetKernel("FillAOVs");

         auto argc = 0U;
         fill_kernel.SetArg(argc++, m_render_data->rays[0]);
         fill_kernel.SetArg(argc++, m_render_data->intersections);
         fill_kernel.SetArg(argc++, num_items);
         fill_kernel.SetArg(argc++, scene.vertices);
         fill_kernel.SetArg(argc++, scene.normals);
         fill_kernel.SetArg(argc++, scene.uvs);
         fill_kernel.SetArg(argc++, scene.indices);
         fill_kernel.SetArg(argc++, scene.shapes);
         fill_kernel.SetArg(argc++, scene.materialids);
         fill_kernel.SetArg(argc++, scene.materials);
         fill_kernel.SetArg(argc++, scene.textures);
         fill_kernel.SetArg(argc++, scene.texturedata);
         fill_kernel.SetArg(argc++, scene.envmapidx);
         fill_kernel.SetArg(argc++, scene.lights);
         fill_kernel.SetArg(argc++, scene.num_lights);
         fill_kernel.SetArg(argc++, rand_uint());
         fill_kernel.SetArg(argc++, m_render_data->random);
         fill_kernel.SetArg(argc++, m_render_data->sobolmat);
         fill_kernel.SetArg(argc++, m_framecnt);
         for (auto i = 1U; i < static_cast<std::uint32_t>(Renderer::OutputType::kMax); ++i)
         {
             if (auto aov = static_cast<ClwOutput*>(GetOutput(static_cast<Renderer::OutputType>(i))))
             {
                 fill_kernel.SetArg(argc++, 1);
                 fill_kernel.SetArg(argc++, aov->data());
             }
             else
             {
                 fill_kernel.SetArg(argc++, 0);
                 // This is simply a dummy buffer
                 fill_kernel.SetArg(argc++, m_render_data->hitcount);
             }
         }

         // Run AOV kernel
         {
             int globalsize = output->width() * output->height();
             m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, fill_kernel);
         }
    }

    void PtRenderer::GeneratePrimaryRays(ClwScene const& scene, Output const& output)
    {
        // Fetch kernel
        std::string kernel_name = (scene.camera_type == CameraType::kDefault) ? "PerspectiveCamera_GeneratePaths" : "PerspectiveCameraDof_GeneratePaths";

        CLWKernel genkernel = m_render_data->program.GetKernel(kernel_name);

        // Set kernel parameters
        int argc = 0;
        genkernel.SetArg(argc++, scene.camera);
        genkernel.SetArg(argc++, output.width());
        genkernel.SetArg(argc++, output.height());
        genkernel.SetArg(argc++, (int)rand_uint());
        genkernel.SetArg(argc++, m_framecnt);
        genkernel.SetArg(argc++, m_render_data->rays[0]);
        genkernel.SetArg(argc++, m_render_data->random);
        genkernel.SetArg(argc++, m_render_data->sobolmat);
        genkernel.SetArg(argc++, m_render_data->paths);

        // Run generation kernel
        {
            size_t gs[] = { static_cast<size_t>((output.width() + 7) / 8 * 8), static_cast<size_t>((output.height() + 7) / 8 * 8) };
            size_t ls[] = { 8, 8 };

            m_context.Launch2D(0, gs, ls, genkernel);
        }
    }

    void PtRenderer::ShadeSurface(ClwScene const& scene, int pass)
    {
        // Fetch kernel
        CLWKernel shadekernel = m_render_data->program.GetKernel("ShadeSurface");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        // Set kernel parameters
        int argc = 0;
        shadekernel.SetArg(argc++, m_render_data->rays[pass & 0x1]);
        shadekernel.SetArg(argc++, m_render_data->intersections);
        shadekernel.SetArg(argc++, m_render_data->compacted_indices);
        shadekernel.SetArg(argc++, m_render_data->pixelindices[pass & 0x1]);
        shadekernel.SetArg(argc++, m_render_data->hitcount);
        shadekernel.SetArg(argc++, scene.vertices);
        shadekernel.SetArg(argc++, scene.normals);
        shadekernel.SetArg(argc++, scene.uvs);
        shadekernel.SetArg(argc++, scene.indices);
        shadekernel.SetArg(argc++, scene.shapes);
        shadekernel.SetArg(argc++, scene.materialids);
        shadekernel.SetArg(argc++, scene.materials);
        shadekernel.SetArg(argc++, scene.textures);
        shadekernel.SetArg(argc++, scene.texturedata);
        shadekernel.SetArg(argc++, scene.envmapidx);
        shadekernel.SetArg(argc++, scene.lights);
        shadekernel.SetArg(argc++, scene.num_lights);
        shadekernel.SetArg(argc++, rand_uint());
        shadekernel.SetArg(argc++, m_render_data->random);
        shadekernel.SetArg(argc++, m_render_data->sobolmat);
        shadekernel.SetArg(argc++, pass);
        shadekernel.SetArg(argc++, m_framecnt);
        shadekernel.SetArg(argc++, scene.volumes);
        shadekernel.SetArg(argc++, m_render_data->shadowrays);
        shadekernel.SetArg(argc++, m_render_data->lightsamples);
        shadekernel.SetArg(argc++, m_render_data->paths);
        shadekernel.SetArg(argc++, m_render_data->rays[(pass + 1) & 0x1]);
        shadekernel.SetArg(argc++, output->data());

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, shadekernel);
        }
    }

    void PtRenderer::ShadeVolume(ClwScene const& scene, int pass)
    {
        // Fetch kernel
        CLWKernel shadekernel = m_render_data->program.GetKernel("ShadeVolume");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        // Set kernel parameters
        int argc = 0;
        shadekernel.SetArg(argc++, m_render_data->rays[pass & 0x1]);
        shadekernel.SetArg(argc++, m_render_data->intersections);
        shadekernel.SetArg(argc++, m_render_data->compacted_indices);
        shadekernel.SetArg(argc++, m_render_data->pixelindices[pass & 0x1]);
        shadekernel.SetArg(argc++, m_render_data->hitcount);
        shadekernel.SetArg(argc++, scene.vertices);
        shadekernel.SetArg(argc++, scene.normals);
        shadekernel.SetArg(argc++, scene.uvs);
        shadekernel.SetArg(argc++, scene.indices);
        shadekernel.SetArg(argc++, scene.shapes);
        shadekernel.SetArg(argc++, scene.materialids);
        shadekernel.SetArg(argc++, scene.materials);
        shadekernel.SetArg(argc++, scene.textures);
        shadekernel.SetArg(argc++, scene.texturedata);
        shadekernel.SetArg(argc++, scene.envmapidx);
        shadekernel.SetArg(argc++, scene.lights);
        shadekernel.SetArg(argc++, scene.num_lights);
        shadekernel.SetArg(argc++, rand_uint());
        shadekernel.SetArg(argc++, m_render_data->random);
        shadekernel.SetArg(argc++, m_render_data->sobolmat);
        shadekernel.SetArg(argc++, pass);
        shadekernel.SetArg(argc++, m_framecnt);
        shadekernel.SetArg(argc++, scene.volumes);
        shadekernel.SetArg(argc++, m_render_data->shadowrays);
        shadekernel.SetArg(argc++, m_render_data->lightsamples);
        shadekernel.SetArg(argc++, m_render_data->paths);
        shadekernel.SetArg(argc++, m_render_data->rays[(pass + 1) & 0x1]);
        shadekernel.SetArg(argc++, output->data());

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, shadekernel);
        }

    }

    void PtRenderer::EvaluateVolume(ClwScene const& scene, int pass)
    {
        // Fetch kernel
        CLWKernel evalkernel = m_render_data->program.GetKernel("EvaluateVolume");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        // Set kernel parameters
        int argc = 0;
        evalkernel.SetArg(argc++, m_render_data->rays[pass & 0x1]);
        evalkernel.SetArg(argc++, m_render_data->pixelindices[(pass + 1) & 0x1]);
        evalkernel.SetArg(argc++, m_render_data->hitcount);
        evalkernel.SetArg(argc++, scene.volumes);
        evalkernel.SetArg(argc++, scene.textures);
        evalkernel.SetArg(argc++, scene.texturedata);
        evalkernel.SetArg(argc++, rand_uint());
        evalkernel.SetArg(argc++, m_render_data->random);
        evalkernel.SetArg(argc++, m_render_data->sobolmat);
        evalkernel.SetArg(argc++, pass);
        evalkernel.SetArg(argc++, m_framecnt);
        evalkernel.SetArg(argc++, m_render_data->intersections);
        evalkernel.SetArg(argc++, m_render_data->paths);
        evalkernel.SetArg(argc++, output->data());

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, evalkernel);
        }
    }

    void PtRenderer::ShadeBackground(ClwScene const& scene, int pass)
    {
        // Fetch kernel
        CLWKernel misskernel = m_render_data->program.GetKernel("ShadeBackgroundEnvMap");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        int numrays = output->width() * output->height();

        // Set kernel parameters
        int argc = 0;
        misskernel.SetArg(argc++, m_render_data->rays[pass & 0x1]);
        misskernel.SetArg(argc++, m_render_data->intersections);
        misskernel.SetArg(argc++, m_render_data->pixelindices[(pass + 1) & 0x1]);
        misskernel.SetArg(argc++, numrays);
        misskernel.SetArg(argc++, scene.lights);
        misskernel.SetArg(argc++, scene.envmapidx);
        misskernel.SetArg(argc++, scene.textures);
        misskernel.SetArg(argc++, scene.texturedata);
        misskernel.SetArg(argc++, m_render_data->paths);
        misskernel.SetArg(argc++, scene.volumes);
        misskernel.SetArg(argc++, output->data());

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, misskernel);
        }
    }

    void PtRenderer::GatherLightSamples(ClwScene const& scene, int pass)
    {
        // Fetch kernel
        CLWKernel gatherkernel = m_render_data->program.GetKernel("GatherLightSamples");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        // Set kernel parameters
        int argc = 0;
        gatherkernel.SetArg(argc++, m_render_data->pixelindices[pass & 0x1]);
        gatherkernel.SetArg(argc++, m_render_data->hitcount);
        gatherkernel.SetArg(argc++, m_render_data->shadowhits);
        gatherkernel.SetArg(argc++, m_render_data->lightsamples);
        gatherkernel.SetArg(argc++, m_render_data->paths);
        gatherkernel.SetArg(argc++, output->data());

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, gatherkernel);
        }
    }

    void PtRenderer::RestorePixelIndices(int pass)
    {
        // Fetch kernel
        CLWKernel restorekernel = m_render_data->program.GetKernel("RestorePixelIndices");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        // Set kernel parameters
        int argc = 0;
        restorekernel.SetArg(argc++, m_render_data->compacted_indices);
        restorekernel.SetArg(argc++, m_render_data->hitcount);
        restorekernel.SetArg(argc++, m_render_data->pixelindices[(pass + 1) & 0x1]);
        restorekernel.SetArg(argc++, m_render_data->pixelindices[pass & 0x1]);

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, restorekernel);
        }
    }

    void PtRenderer::FilterPathStream(int pass)
    {
        // Fetch kernel
        CLWKernel restorekernel = m_render_data->program.GetKernel("FilterPathStream");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        // Set kernel parameters
        int argc = 0;
        restorekernel.SetArg(argc++, m_render_data->intersections);
        restorekernel.SetArg(argc++, m_render_data->hitcount);
        restorekernel.SetArg(argc++, m_render_data->pixelindices[(pass + 1) & 0x1]);
        restorekernel.SetArg(argc++, m_render_data->paths);
        restorekernel.SetArg(argc++, m_render_data->hits);

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, restorekernel);
        }

        //int cnt = 0;
        //m_context.ReadBuffer(0, m_render_data->debug, &cnt, 1).Wait();
        //std::cout << "Pass " << pass << " killed " << cnt << "\n";
    }

    CLWKernel PtRenderer::GetCopyKernel()
    {
        return m_render_data->program.GetKernel("ApplyGammaAndCopyData");
    }

    CLWKernel PtRenderer::GetAccumulateKernel()
    {
        return m_render_data->program.GetKernel("AccumulateData");
    }

    // Shade background
    void PtRenderer::ShadeMiss(ClwScene const& scene, int pass)
    {
        // Fetch kernel
        CLWKernel misskernel = m_render_data->program.GetKernel("ShadeMiss");

        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        //int numrays = m_output->width() * m_output->height();

        // Set kernel parameters
        int argc = 0;
        misskernel.SetArg(argc++, m_render_data->rays[pass & 0x1]);
        misskernel.SetArg(argc++, m_render_data->intersections);
        misskernel.SetArg(argc++, m_render_data->pixelindices[(pass + 1) & 0x1]);
        misskernel.SetArg(argc++, m_render_data->hitcount);
        misskernel.SetArg(argc++, scene.lights);
        misskernel.SetArg(argc++, scene.envmapidx);
        misskernel.SetArg(argc++, scene.textures);
        misskernel.SetArg(argc++, scene.texturedata);
        misskernel.SetArg(argc++, m_render_data->paths);
        misskernel.SetArg(argc++, scene.volumes);
        misskernel.SetArg(argc++, output->data());

        // Run shading kernel
        {
            int globalsize = output->width() * output->height();
            m_context.Launch1D(0, ((globalsize + 63) / 64) * 64, 64, misskernel);
        }

    }

    void PtRenderer::RunBenchmark(Scene1 const& scene, std::uint32_t num_passes, BenchmarkStats& stats)
    {
        auto output = static_cast<ClwOutput*>(GetOutput(OutputType::kColor));

        stats.num_passes = num_passes;
        stats.resolution = int2(output->width(), output->height());

        auto api = m_scene_controller.GetIntersectionApi();
        auto& clwscene = m_scene_controller.CompileScene(scene, m_render_data->mat_collector, m_render_data->tex_collector);

        // Number of rays to generate
        int maxrays = output->width() * output->height();

        // Generate primary
        GeneratePrimaryRays(clwscene, *output);

        // Copy compacted indices to track reverse indices
        m_context.CopyBuffer(0, m_render_data->iota, m_render_data->pixelindices[0], 0, 0, m_render_data->iota.GetElementCount());
        m_context.CopyBuffer(0, m_render_data->iota, m_render_data->pixelindices[1], 0, 0, m_render_data->iota.GetElementCount());
        m_context.FillBuffer(0, m_render_data->hitcount, maxrays, 1);


        // Clear ray hits buffer
        m_context.FillBuffer(0, m_render_data->hits, 0, m_render_data->hits.GetElementCount());

        // Intersect ray batch
        auto start = std::chrono::high_resolution_clock::now();

        for (auto i = 0U; i < num_passes; ++i)
        {
            api->QueryIntersection(m_render_data->fr_rays[0], m_render_data->fr_hitcount, maxrays, m_render_data->fr_intersections, nullptr, nullptr);
        }

        m_context.Finish(0);

        auto delta = std::chrono::high_resolution_clock::now() - start;

        stats.primary_rays_time_in_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() / num_passes;

        // Convert intersections to predicates
        FilterPathStream(0);

        // Compact batch
        m_render_data->pp.Compact(0, m_render_data->hits, m_render_data->iota, m_render_data->compacted_indices, m_render_data->hitcount);

        // Advance indices to keep pixel indices up to date
        RestorePixelIndices(0);

        // Shade hits
        ShadeSurface(clwscene, 0);

        // Shade missing rays
        ShadeMiss(clwscene, 0);

        // Intersect ray batch
        start = std::chrono::high_resolution_clock::now();

        for (auto i = 0U; i < num_passes; ++i)
        {
            api->QueryOcclusion(m_render_data->fr_shadowrays, m_render_data->fr_hitcount, maxrays, m_render_data->fr_shadowhits, nullptr, nullptr);
        }

        m_context.Finish(0);

        delta = std::chrono::high_resolution_clock::now() - start;

        stats.shadow_rays_time_in_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() / num_passes;

        // Gather light samples and account for visibility
        GatherLightSamples(clwscene, 0);

        //
        m_context.Flush(0);

        // Clear ray hits buffer
        m_context.FillBuffer(0, m_render_data->hits, 0, m_render_data->hits.GetElementCount());

        // Intersect ray batch
        start = std::chrono::high_resolution_clock::now();

        for (auto i = 0U; i < num_passes; ++i)
        {
            api->QueryIntersection(m_render_data->fr_rays[1], m_render_data->fr_hitcount, maxrays, m_render_data->fr_intersections, nullptr, nullptr);
        }

        m_context.Finish(0);

        delta = std::chrono::high_resolution_clock::now() - start;

        stats.secondary_rays_time_in_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() / num_passes;

        // Convert intersections to predicates
        FilterPathStream(1);

        // Compact batch
        m_render_data->pp.Compact(0, m_render_data->hits, m_render_data->iota, m_render_data->compacted_indices, m_render_data->hitcount);

        // Advance indices to keep pixel indices up to date
        RestorePixelIndices(1);

        // Shade hits
        ShadeSurface(clwscene, 1);

        // Shade missing rays
        ShadeMiss(clwscene, 1);

        api->QueryOcclusion(m_render_data->fr_shadowrays, m_render_data->fr_hitcount, maxrays, m_render_data->fr_shadowhits, nullptr, nullptr);

        // Gather light samples and account for visibility
        GatherLightSamples(clwscene, 0);

        //
        m_context.Flush(0);
    }
}
