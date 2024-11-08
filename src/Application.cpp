// This file is part of the AMD & HSC Work Graph Playground.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc. and Coburg University of Applied Sciences and Arts.
// All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "Application.h"

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <iostream>
#include <sstream>

Application::Application(const Options& options)
{
    // Check if tutorials are available
    {
        const auto tutorials = GetTutorials();

        if (tutorials.empty()) {
            throw std::runtime_error("No tutorials found. Please check \"tutorials/\" folder.");
        }
    }

    window_ = std::make_unique<Window>(options.title, options.windowWidth, options.windowHeight);
    device_ =
        std::make_unique<Device>(options.forceWarpAdapter, options.enableDebugLayer, options.enableGpuValidationLayer);
    swapchain_ = std::make_unique<Swapchain>(device_.get(), window_.get());

    CreateResourceDescriptorHeaps();
    CreateWritableBackbuffer(window_->GetWidth(), window_->GetHeight());
    CreateScratchBuffer();
    CreatePersistentScratchBuffer();

    CreateFontBuffer();

    CreateImGuiContext();

    CreateWorkGraphRootSignature();
    CreateWorkGraph();
}

Application::~Application()
{
    DestroyImGuiContext();
}

void Application::Run()
{
    do {
        // Check if resize is needed
        if ((window_->GetWidth() != swapchain_->GetWidth()) ||  //
            (window_->GetHeight() != swapchain_->GetHeight()))
        {
            // Resize swapchain
            OnResize(window_->GetWidth(), window_->GetHeight());
        }

        // Check if re-creation of work graph is required
        if (shaderCompiler_.CheckShaderSourceFiles()) {
            std::cout << "Changes to shader source files detected. Recompiling work graph..." << std::endl;
            // Recompile shaders & re-create work graph
            const bool success = CreateWorkGraph();

            if (success) {
                // Reset error message time
                errorMessageEndTime_ = std::chrono::high_resolution_clock::now();
            } else {
                using namespace std::chrono_literals;
                // Show error message pop-up for 5s
                errorMessageEndTime_ = std::chrono::high_resolution_clock::now() + 5s;
            }
        }

        // Check if tutorial was switched
        if ((workGraph_->GetTutorialIndex() != workGraphTutorialIndex_) ||
            (workGraph_->IsSampleSolution() != workGraphUseSampleSolution_))
        {
            std::cout << "Compiling ";
            if (workGraphUseSampleSolution_) {
                std::cout << "sample solution ";
            }
            std::cout << "work graph for tutorial " << workGraphTutorialIndex_ << "... " << std::endl;

            // Try to compile work graph for new tutorial
            const auto success = CreateWorkGraph();

            if (success) {
                // Clear persistent scratch buffer if work graph was changes successfully
                clearPersistentScratchBuffer_ = true;

                // Reset start and error message time
                startTime_ = errorMessageEndTime_ = std::chrono::high_resolution_clock::now();
            } else {
                // Set current tutorial index to current work graph.
                // This prevents endlessly re-creating the work graph in case the compilation fails
                workGraphTutorialIndex_     = workGraph_->GetTutorialIndex();
                workGraphUseSampleSolution_ = workGraph_->IsSampleSolution();

                using namespace std::chrono_literals;
                // Show error message pop-up for 5s
                errorMessageEndTime_ = std::chrono::high_resolution_clock::now() + 5s;
            }
        }

        // Advance to next command buffer
        auto*      commandList  = device_->GetNextFrameCommandList();
        const auto renderTarget = swapchain_->GetNextRenderTarget();

        // Advance ImGui to next frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Transition render target to RENDER_TARGET state
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTarget.colorResource.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ResourceBarrier(1, &barrier);
        }

        OnRender(commandList, renderTarget);

        OnRenderUserInterface(commandList, renderTarget);

        // Transition render target to PRESENT state
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTarget.colorResource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            commandList->ResourceBarrier(1, &barrier);
        }

        // Execute command list
        device_->ExecuteCurrentFrameCommandList();
        // Present frame
        swapchain_->Present(vsync_);
    } while (window_->HandleEvents());

    device_->WaitForDevice();
}

std::span<const WorkGraph::WorkGraphTutorial> Application::GetTutorials()
{
    const auto LoadTutorials = []() {
        std::vector<WorkGraph::WorkGraphTutorial> result;

        const auto shaderFolder = std::filesystem::path("tutorials");

        for (const auto& entry : std::filesystem::recursive_directory_iterator(shaderFolder)) {
            const auto& path = entry.path();

            // Ignore non-HLSL files
            if (path.extension() != ".hlsl") {
                continue;
            }
            // Ignore solution
            if (path.stem().string().ends_with("Solution")) {
                continue;
            }

            const auto stem = path.stem().string();

            std::stringstream nameStream;

            // Compute tutorial name
            {
                nameStream << "Tutorial " << result.size() << ": ";

                bool lastUpper = true;
                bool lastAlpha = true;

                for (const auto c : stem) {
                    const auto upper = std::isupper(c);
                    const auto alpha = std::isupper(c);

                    // Insert space between camel-case names
                    if (upper && !lastUpper) {
                        nameStream << " ";
                    }

                    nameStream << c;

                    lastUpper = upper;
                    lastAlpha = alpha;
                }
            }

            WorkGraph::WorkGraphTutorial tutorial = {};
            tutorial.name                         = nameStream.str();
            tutorial.shaderFileName               = std::filesystem::relative(path, shaderFolder).generic_string();

            const auto solutionFilename = path.parent_path() / (stem + "Solution.hlsl");

            if (std::filesystem::exists(solutionFilename)) {
                tutorial.solutionShaderFileName =
                    std::filesystem::relative(solutionFilename, shaderFolder).generic_string();
            }

            result.emplace_back(tutorial);
        }

        return result;
    };

    static std::vector<WorkGraph::WorkGraphTutorial> tutorials = LoadTutorials();

    return tutorials;
}

void Application::OnRender(ID3D12GraphicsCommandList10* commandList, const Swapchain::RenderTarget& renderTarget)
{
    // Clear shader resources (writable backbuffer & scratch buffer)
    ClearShaderResources(commandList);

    // Set root signature for parameters
    commandList->SetComputeRootSignature(workGraphRootSignature_.Get());

    struct RootConstants {
        unsigned width, height;
        float    mouseX, mouseY;
        unsigned inputState;
        float    time;
    };

    const auto& mousePos = ImGui::GetMousePos();

    RootConstants constants = {
        .width      = window_->GetWidth(),
        .height     = window_->GetHeight(),
        .mouseX     = mousePos.x,
        .mouseY     = mousePos.y,
        .inputState = 0,
        .time = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::high_resolution_clock::now() -
                                                                         startTime_)
                    .count(),
    };

    // Compute input state
    constants.inputState |= ImGui::IsMouseDown(ImGuiMouseButton_Left) << 0U;
    constants.inputState |= ImGui::IsMouseDown(ImGuiMouseButton_Middle) << 1U;
    constants.inputState |= ImGui::IsMouseDown(ImGuiMouseButton_Right) << 2U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_Space) << 3U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_UpArrow) << 4U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_LeftArrow) << 5U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_DownArrow) << 6U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_RightArrow) << 7U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_W) << 8U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_A) << 9U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_S) << 10U;
    constants.inputState |= ImGui::IsKeyDown(ImGuiKey_D) << 11U;

    // Set root constants
    commandList->SetComputeRoot32BitConstants(0, 6, &constants, 0);

    // Set font buffer
    commandList->SetComputeRootShaderResourceView(1, fontBuffer_->GetGPUVirtualAddress());

    // Set descriptor heap & table
    commandList->SetDescriptorHeaps(1, resourceDescriptorHeap_.GetAddressOf());
    commandList->SetComputeRootDescriptorTable(2, resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart());

    workGraph_->Dispatch(commandList);

    // Copy writable backbuffer to render target
    {
        std::array<D3D12_RESOURCE_BARRIER, 2> preBarriers = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                writableBackbuffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(
                renderTarget.colorResource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        commandList->ResourceBarrier(preBarriers.size(), preBarriers.data());

        const D3D12_TEXTURE_COPY_LOCATION sourceLocation = {
            .pResource        = writableBackbuffer_.Get(),
            .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };
        const D3D12_TEXTURE_COPY_LOCATION destLocation = {
            .pResource        = renderTarget.colorResource.Get(),
            .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };

        commandList->CopyTextureRegion(&destLocation, 0, 0, 0, &sourceLocation, nullptr);

        std::array<D3D12_RESOURCE_BARRIER, 2> postBarriers = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                writableBackbuffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(
                renderTarget.colorResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        commandList->ResourceBarrier(postBarriers.size(), postBarriers.data());
    }
}

void Application::OnRenderUserInterface(ID3D12GraphicsCommandList10*   commandList,
                                        const Swapchain::RenderTarget& renderTarget)
{
    const auto tutorials = GetTutorials();

    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
    ImGui::BeginMainMenuBar();

    if (ImGui::BeginMenu("Tutorials")) {
        for (std::uint32_t tutorialIndex = 0; tutorialIndex < tutorials.size(); ++tutorialIndex) {
            const auto& tutorial = tutorials[tutorialIndex];

            if (ImGui::MenuItem(tutorial.name.c_str(), nullptr, tutorialIndex == workGraphTutorialIndex_)) {
                workGraphTutorialIndex_     = tutorialIndex;
                // Reset sample solution
                workGraphUseSampleSolution_ = false;
            }
        }

        ImGui::EndMenu();
    }

    if (!tutorials[workGraphTutorialIndex_].solutionShaderFileName.empty()) {
        ImGui::Text("|");
        ImGui::Checkbox("Sample Solution", &workGraphUseSampleSolution_);
    }

    ImGui::Text("|");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5, 0, 1));
    ImGui::Text("Open tutorials/%s to start this tutorial.", tutorials[workGraphTutorialIndex_].shaderFileName.c_str());
    ImGui::PopStyleColor();

    // Print current FPS to menu bar
    {
        const auto& io                = ImGui::GetIO();
        const auto  frametimeTextSize = ImGui::CalcTextSize("Frametime: XXXXXms (XXXX FPS)");
        const auto  vsyncTextSize     = ImGui::CalcTextSize("V-Sync");
        const auto  checkboxWidth     = ImGui::GetFrameHeight();
        const auto  padding           = 20;

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             (frametimeTextSize.x + vsyncTextSize.x + checkboxWidth + padding));
        ImGui::Checkbox("V-Sync", &vsync_);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - frametimeTextSize.x);
        ImGui::Text("Frametime: %5.1fms (%4.0f FPS)", io.DeltaTime * 1000.f, io.Framerate);
    }

    ImGui::EndMainMenuBar();
    ImGui::PopStyleColor(2);

    // Compilation error message window
    if (errorMessageEndTime_ >= std::chrono::high_resolution_clock::now()) {
        ImGui::SetNextWindowPos(
            ImVec2(window_->GetWidth() / 2, window_->GetHeight() - 20), ImGuiCond_Always, ImVec2(0.5, 1));

        if (ImGui::Begin("error",
                         nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs))
        {
            ImGui::Text("Work Graph compilation failed. Check output for more details.");
        }

        ImGui::End();
    }

    // Info window
    {
        ImGui::SetNextWindowPos(ImVec2(0, window_->GetHeight()), ImGuiCond_Always, ImVec2(0, 1));

        if (ImGui::Begin("info",
                         nullptr,
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
            ImGui::Text("Adapter: %s", device_->GetAdapterDescription().c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }

    // Message window at bottom
    {
        ImGui::SetNextWindowPos(ImVec2(window_->GetWidth(), window_->GetHeight()), ImGuiCond_Always, ImVec2(1, 1));

        if (ImGui::Begin("bottom",
                         nullptr,
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
            ImGui::Text("Work Graph Playground by AMD & HS Coburg");
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }

    // Render to render target
    {
        // Set swapchain render target
        commandList->OMSetRenderTargets(
            1, &renderTarget.colorDescriptorHandle, false, &renderTarget.depthDescriptorHandle);

        // Bind UI descriptor heap
        commandList->SetDescriptorHeaps(1, uiDescriptorHeap_.GetAddressOf());

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
    }
}

void Application::OnResize(std::uint32_t width, std::uint32_t height)
{
    // Wait for all frames in flight
    device_->WaitForDevice();

    swapchain_->Resize(width, height);

    CreateWritableBackbuffer(width, height);
}

void Application::CreateImGuiContext()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Disbale ini and log files
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // Create descriptor heap for ImGui
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors             = 1;
        desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NodeMask                   = 1;
        ThrowIfFailed(device_->GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&uiDescriptorHeap_)));
    }

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(window_->GetHandle());
    ImGui_ImplDX12_Init(device_->GetDevice(),
                        Device::BufferedFramesCount,
                        Swapchain::ColorTargetFormat,
                        uiDescriptorHeap_.Get(),
                        uiDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
                        uiDescriptorHeap_->GetGPUDescriptorHandleForHeapStart());
}

void Application::DestroyImGuiContext()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void Application::CreateWorkGraphRootSignature()
{
    const auto descriptorRange = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);

    std::array<CD3DX12_ROOT_PARAMETER, 3> rootParameters;
    rootParameters[0].InitAsConstants(6, 0);
    rootParameters[1].InitAsShaderResourceView(0);
    rootParameters[2].InitAsDescriptorTable(1, &descriptorRange);

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(rootParameters.size(), rootParameters.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(device_->GetDevice()->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&workGraphRootSignature_)));
}

bool Application::CreateWorkGraph()
{
    // Wait for all frames in fight before deleting old resources
    device_->WaitForDevice();

    try {
        workGraph_ = std::make_unique<WorkGraph>(device_.get(),
                                                 shaderCompiler_,
                                                 workGraphRootSignature_.Get(),
                                                 workGraphTutorialIndex_,
                                                 workGraphUseSampleSolution_);
    } catch (const std::exception& e) {
        // Re-throw exception if no fallback work graph exists
        if (!workGraph_) {
            throw e;
        }

        std::cerr << "Failed to re-create work graph:\n" << e.what() << std::endl;

        return false;
    }

    return true;
}

void Application::CreateResourceDescriptorHeaps()
{
    // Create descriptor heap to clear shader resources
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors             = 3;
        desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask                   = 1;
        ThrowIfFailed(device_->GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&clearDescriptorHeap_)));
    }
    // Create resource descriptor heap for shader resources
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors             = 3;
        desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NodeMask                   = 1;
        ThrowIfFailed(device_->GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&resourceDescriptorHeap_)));
    }
}

void Application::CreateWritableBackbuffer(std::uint32_t width, std::uint32_t height)
{
    writableBackbuffer_.Reset();

    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC   resourceDescription = CD3DX12_RESOURCE_DESC::Tex2D(
        Swapchain::ColorTargetFormat, width, height, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(device_->GetDevice()->CreateCommittedResource(&heapProperties,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &resourceDescription,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                nullptr,
                                                                IID_PPV_ARGS(&writableBackbuffer_)));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension                    = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format                           = Swapchain::ColorTargetFormat;
    uavDesc.Texture2D.MipSlice               = 0;
    uavDesc.Texture2D.PlaneSlice             = 0;

    const auto descriptorSize =
        device_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto descriptorIndex = 0;

    device_->GetDevice()->CreateUnorderedAccessView(
        writableBackbuffer_.Get(),
        nullptr,
        &uavDesc,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize));
    device_->GetDevice()->CreateUnorderedAccessView(
        writableBackbuffer_.Get(),
        nullptr,
        &uavDesc,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(
            resourceDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize));
}

void Application::CreateScratchBuffer()
{
    scratchBuffer_.Reset();

    const auto elementCount = 100 * 1024;
    const auto elementSize  = sizeof(std::uint32_t);

    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC   resourceDescription =
        CD3DX12_RESOURCE_DESC::Buffer(elementCount * elementSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(device_->GetDevice()->CreateCommittedResource(&heapProperties,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &resourceDescription,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                nullptr,
                                                                IID_PPV_ARGS(&scratchBuffer_)));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension                    = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format                           = DXGI_FORMAT_R32_TYPELESS;
    uavDesc.Buffer.CounterOffsetInBytes      = 0;
    uavDesc.Buffer.FirstElement              = 0;
    uavDesc.Buffer.NumElements               = elementCount;
    uavDesc.Buffer.StructureByteStride       = 0;
    uavDesc.Buffer.Flags                     = D3D12_BUFFER_UAV_FLAG_RAW;

    const auto descriptorSize =
        device_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto descriptorIndex = 1;

    device_->GetDevice()->CreateUnorderedAccessView(
        scratchBuffer_.Get(),
        nullptr,
        &uavDesc,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize));
    device_->GetDevice()->CreateUnorderedAccessView(
        scratchBuffer_.Get(),
        nullptr,
        &uavDesc,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(
            resourceDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize));
}

void Application::CreatePersistentScratchBuffer()
{
    persistentScratchBuffer_.Reset();

    const auto elementCount = 100 * 1024 * 1024;
    const auto elementSize  = sizeof(std::uint32_t);

    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC   resourceDescription =
        CD3DX12_RESOURCE_DESC::Buffer(elementCount * elementSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(device_->GetDevice()->CreateCommittedResource(&heapProperties,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &resourceDescription,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                nullptr,
                                                                IID_PPV_ARGS(&persistentScratchBuffer_)));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension                    = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format                           = DXGI_FORMAT_R32_TYPELESS;
    uavDesc.Buffer.CounterOffsetInBytes      = 0;
    uavDesc.Buffer.FirstElement              = 0;
    uavDesc.Buffer.NumElements               = elementCount;
    uavDesc.Buffer.StructureByteStride       = 0;
    uavDesc.Buffer.Flags                     = D3D12_BUFFER_UAV_FLAG_RAW;

    const auto descriptorSize =
        device_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto descriptorIndex = 2;

    device_->GetDevice()->CreateUnorderedAccessView(
        persistentScratchBuffer_.Get(),
        nullptr,
        &uavDesc,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize));
    device_->GetDevice()->CreateUnorderedAccessView(
        persistentScratchBuffer_.Get(),
        nullptr,
        &uavDesc,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(
            resourceDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize));
}

void Application::ClearShaderResources(ID3D12GraphicsCommandList10* commandList)
{
    // Set descriptor heap for clear
    commandList->SetDescriptorHeaps(1, resourceDescriptorHeap_.GetAddressOf());

    const auto descriptorSize =
        device_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Clear writable backbuffer
    {
        const auto descriptorIndex     = 0;
        const auto gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
            resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize);
        const auto cpuDescriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize);

        float clearValue[4] = {1.f, 1.f, 1.f, 1.f};
        commandList->ClearUnorderedAccessViewFloat(
            gpuDescriptorHandle, cpuDescriptorHandle, writableBackbuffer_.Get(), clearValue, 0, nullptr);
    }

    // Clear scratch buffer
    {
        const auto descriptorIndex     = 1;
        const auto gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
            resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize);
        const auto cpuDescriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize);

        std::uint32_t clearValue[4] = {0, 0, 0, 0};
        commandList->ClearUnorderedAccessViewUint(
            gpuDescriptorHandle, cpuDescriptorHandle, scratchBuffer_.Get(), clearValue, 0, nullptr);
    }

    // Clear persistent scratch buffer
    if (clearPersistentScratchBuffer_) {
        const auto descriptorIndex     = 2;
        const auto gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
            resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize);
        const auto cpuDescriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, descriptorSize);

        std::uint32_t clearValue[4] = {0, 0, 0, 0};
        commandList->ClearUnorderedAccessViewUint(
            gpuDescriptorHandle, cpuDescriptorHandle, persistentScratchBuffer_.Get(), clearValue, 0, nullptr);

        // Reset clear
        clearPersistentScratchBuffer_ = false;
    }

    std::array<D3D12_RESOURCE_BARRIER, 3> uavBarriers = {
        CD3DX12_RESOURCE_BARRIER::UAV(writableBackbuffer_.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(scratchBuffer_.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(persistentScratchBuffer_.Get()),
    };

    // Barrier for clear operation
    commandList->ResourceBarrier(uavBarriers.size(), uavBarriers.data());
}

void Application::CreateFontBuffer()
{
    fontBuffer_.Reset();

    std::array<std::uint64_t, 128> fontData = {
        0x0000000000000000,  // nul
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  //
        0x0000000000000000,  // space
        0x183C3C1818001800,  // !
        0x3636000000000000,  // "
        0x36367F367F363600,  // #
        0x0C3E031E301F0C00,  // $
        0x006333180C666300,  // %
        0x1C361C6E3B336E00,  // &
        0x0606030000000000,  // '
        0x180C0606060C1800,  // (
        0x060C1818180C0600,  // )
        0x00663CFF3C660000,  // *
        0x000C0C3F0C0C0000,  // +
        0x00000000000C0C06,  // ,
        0x0000003F00000000,  // -
        0x00000000000C0C00,  // .
        0x6030180C06030100,  // /
        0x3E63737B6F673E00,  // 0
        0x0C0E0C0C0C0C3F00,  // 1
        0x1E33301C06333F00,  // 2
        0x1E33301C30331E00,  // 3
        0x383C36337F307800,  // 4
        0x3F031F3030331E00,  // 5
        0x1C06031F33331E00,  // 6
        0x3F3330180C0C0C00,  // 7
        0x1E33331E33331E00,  // 8
        0x1E33333E30180E00,  // 9
        0x000C0C00000C0C00,  // :
        0x000C0C00000C0C06,  // ;
        0x180C0603060C1800,  // <
        0x00003F00003F0000,  // =
        0x060C1830180C0600,  // >
        0x1E3330180C000C00,  // ?
        0x3E637B7B7B031E00,  // @
        0x0C1E33333F333300,  // A
        0x3F66663E66663F00,  // B
        0x3C66030303663C00,  // C
        0x1F36666666361F00,  // D
        0x7F46161E16467F00,  // E
        0x7F46161E16060F00,  // F
        0x3C66030373667C00,  // G
        0x3333333F33333300,  // H
        0x1E0C0C0C0C0C1E00,  // I
        0x7830303033331E00,  // J
        0x6766361E36666700,  // K
        0x0F06060646667F00,  // L
        0x63777F7F6B636300,  // M
        0x63676F7B73636300,  // N
        0x1C36636363361C00,  // O
        0x3F66663E06060F00,  // P
        0x1E3333333B1E3800,  // Q
        0x3F66663E36666700,  // R
        0x1E33070E38331E00,  // S
        0x3F2D0C0C0C0C1E00,  // T
        0x3333333333333F00,  // U
        0x33333333331E0C00,  // V
        0x6363636B7F776300,  // W
        0x6363361C1C366300,  // X
        0x3333331E0C0C1E00,  // Y
        0x7F6331184C667F00,  // Z
        0x1E06060606061E00,  // [
        0x03060C1830604000,  //
        0x1E18181818181E00,  // ]
        0x081C366300000000,  // ^
        0x00000000000000FF,  // _
        0x0C0C180000000000,  // `
        0x00001E303E336E00,  // a
        0x0706063E66663B00,  // b
        0x00001E3303331E00,  // c
        0x3830303e33336E00,  // d
        0x00001E333f031E00,  // e
        0x1C36060f06060F00,  // f
        0x00006E33333E301F,  // g
        0x0706366E66666700,  // h
        0x0C000E0C0C0C1E00,  // i
        0x300030303033331E,  // j
        0x070666361E366700,  // k
        0x0E0C0C0C0C0C1E00,  // l
        0x0000337F7F6B6300,  // m
        0x00001F3333333300,  // n
        0x00001E3333331E00,  // o
        0x00003B66663E060F,  // p
        0x00006E33333E3078,  // q
        0x00003B6E66060F00,  // r
        0x00003E031E301F00,  // s
        0x080C3E0C0C2C1800,  // t
        0x0000333333336E00,  // u
        0x00003333331E0C00,  // v
        0x0000636B7F7F3600,  // w
        0x000063361C366300,  // x
        0x00003333333E301F,  // y
        0x00003F190C263F00,  // z
        0x380C0C070C0C3800,  // {
        0x1818180018181800,  // |
        0x070C0C380C0C0700,  // }
        0x6E3B000000000000,  // ~
        0x0000000000000000,
    };

    // Storing this buffer in upload heap is not ideal, but does work for these small examples
    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   resourceDescription =
        CD3DX12_RESOURCE_DESC::Buffer(fontData.size() * sizeof(std::uint64_t), D3D12_RESOURCE_FLAG_NONE);
    ThrowIfFailed(device_->GetDevice()->CreateCommittedResource(&heapProperties,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &resourceDescription,
                                                                D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                nullptr,
                                                                IID_PPV_ARGS(&fontBuffer_)));

    void* mappedData;
    ThrowIfFailed(fontBuffer_->Map(0, nullptr, &mappedData));

    memcpy(mappedData, fontData.data(), fontData.size() * sizeof(std::uint64_t));

    fontBuffer_->Unmap(0, nullptr);
}
