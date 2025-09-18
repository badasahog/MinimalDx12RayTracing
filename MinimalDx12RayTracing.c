#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#undef _CRT_SECURE_NO_WARNINGS
#include <shellscalingapi.h>

#include <dxgi1_6.h>
#include <d3d12.h>

#ifdef _DEBUG
#include <dxgidebug.h>
#endif

#include <cglm/cglm.h>
#include <cglm/struct.h>
#include <cglm/call.h>
#include <cglm/cam.h>
#include <cglm/clipspace/persp_lh_zo.h>
#include <cglm/clipspace/view_lh.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdalign.h>

#include "RayTracingHlslCompat_native.h"

#pragma comment(linker, "/DEFAULTLIB:D3d12.lib")
#pragma comment(linker, "/DEFAULTLIB:Shcore.lib")
#pragma comment(linker, "/DEFAULTLIB:DXGI.lib")
#pragma comment(linker, "/DEFAULTLIB:dxguid.lib")

__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
__declspec(dllexport) UINT D3D12SDKVersion = 612;
__declspec(dllexport) char* D3D12SDKPath = ".\\D3D12\\";

HANDLE ConsoleHandle;
ID3D12Device10* Device;

inline void THROW_ON_FAIL_IMPL(HRESULT hr, int line)
{
	if (hr == 0x887A0005)//device removed
	{
		THROW_ON_FAIL_IMPL(ID3D12Device10_GetDeviceRemovedReason(Device), line);
	}

	if (FAILED(hr))
	{
		LPWSTR messageBuffer;
		DWORD formattedErrorLength = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			hr,
			MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			(LPWSTR)&messageBuffer,
			0,
			NULL
		);

		if (formattedErrorLength == 0)
			WriteConsoleA(ConsoleHandle, "an error occured, unable to retrieve error message\n", 51, NULL, NULL);
		else
		{
			WriteConsoleA(ConsoleHandle, "an error occured: ", 18, NULL, NULL);
			WriteConsoleW(ConsoleHandle, messageBuffer, formattedErrorLength, NULL, NULL);
			WriteConsoleA(ConsoleHandle, "\n", 1, NULL, NULL);
			LocalFree(messageBuffer);
		}

		char buffer[50];
		int stringlength = _snprintf_s(buffer, 50, _TRUNCATE, "error code: 0x%X\nlocation:line %i\n", hr, line);
		WriteConsoleA(ConsoleHandle, buffer, stringlength, NULL, NULL);

		RaiseException(0, EXCEPTION_NONCONTINUABLE, 0, NULL);
	}
}

#define THROW_ON_FAIL(x) THROW_ON_FAIL_IMPL(x, __LINE__)

#define THROW_ON_FALSE(x) if((x) == FALSE) THROW_ON_FAIL(HRESULT_FROM_WIN32(GetLastError()))

#define VALIDATE_HANDLE(x) if((x) == NULL || (x) == INVALID_HANDLE_VALUE) THROW_ON_FAIL(HRESULT_FROM_WIN32(GetLastError()))

#define OffsetPointer(x, Offset) ((typeof(x))((char*)x + (Offset)))

#define SizeOfInUint32(obj) ((sizeof(obj) - 1) / sizeof(UINT32) + 1)

inline UINT RoundToMultiple(UINT Size, UINT Alignment)
{
	return (Size + (Alignment - 1)) & ~(Alignment - 1);
}

static const bool bWarp = false;

static const wchar_t* const WindowClassName = L"DXSampleClass";

#define BUFFER_COUNT 3

#define TOTAL_PRIMITIVE_COUNT (ANALYTIC_PRIMITIVE_COUNT + VOLUMETRIC_PRIMITIVE_COUNT + SIGNED_DISTANCE_PRIMITIVE_COUNT)

static const DXGI_FORMAT RTV_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

enum ROOTSIG_SLOT
{
	ROOTSIG_SLOT_OUTPUTVIEW,
	ROOTSIG_SLOT_ACC_STRUCT,
	ROOTSIG_SLOT_SCENE_CB,
	ROOTSIG_SLOT_AABB_ATTRIBUTE_BUFFER,
	ROOTSIG_SLOT_VERTEX_BUFFERS,
	ROOTSIG_SLOT_COUNT
};

enum ROOTSIG_TYPE
{
	ROOTSIG_TYPE_TRIANGLE,
	ROOTSIG_TYPE_AABB,
	ROOTSIG_TYPE_COUNT
};

enum ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT
{
	ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_MATERIAL_CONSTANT,
	ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_COUNT
};

struct ROOTSIG_TRIANGLE_ROOT_ARGS
{
	struct PrimitiveConstantBuffer MaterialCB;
};

enum ROOTSIG_AABB_SLOT
{
	ROOTSIG_AABB_SLOT_MATERIALCONSTANT,
	ROOTSIG_AABB_SLOT_GEOMETRY_INDEX,
	ROOTSIG_AABB_SLOT_COUNT
};

struct ROOTSIG_AABB_ROOTARGS
{
	struct PrimitiveConstantBuffer MaterialCB;
	struct PrimitiveInstanceConstantBuffer AABB_CB;
};

static const UINT MAX_ROOT_ARG_SIZE = max(sizeof(struct ROOTSIG_TRIANGLE_ROOT_ARGS), sizeof(struct ROOTSIG_AABB_ROOTARGS));

enum GEOMETRY_TYPE
{
	GEOMETRY_TYPE_TRIANGLE,
	GEOMETRY_TYPE_AABB,
	GEOMETRY_TYPE_COUNT
};

enum GPU_TIMER
{
	GPU_TIMER_RAYTRACING,
	GPU_TIMER_COUNT
};

enum INTERSECTION_SHADER_TYPE
{
	INTERSECTION_SHADER_TYPE_ANALYTIC_PRIMITIVE,
	INTERSECTION_SHADER_TYPE_VOLUMETRIC_PRIMITIVE,
	INTERSECTION_SHADER_TYPE_SIGNED_DISTANCE_PRIMITIVE,
	INTERSECTION_SHADER_TYPE_COUNT
};

static const float AABB_WIDTH = 2;
static const float AABB_DISTANCE = 2;

static const UINT DESCRIPTOR_COUNT = 3;

static const wchar_t* const RaygenShaderName = L"MyRaygenShader";
static const wchar_t* const IntersectionShaderNames[INTERSECTION_SHADER_TYPE_COUNT] =
{
	L"MyIntersectionShader_AnalyticPrimitive",
	L"MyIntersectionShader_VolumetricPrimitive",
	L"MyIntersectionShader_SignedDistancePrimitive",
};

static const wchar_t* const ClosestHitShaderNames[GEOMETRY_TYPE_COUNT] =
{
	L"MyClosestHitShader_Triangle",
	L"MyClosestHitShader_AABB",
};

static const wchar_t* const MissShaderNames[RAY_TYPE_COUNT] =
{
	L"MyMissShader",
	L"MyMissShader_ShadowRay"
};

static const wchar_t* const HitGroupNames_TriangleGeometry[RAY_TYPE_COUNT] =
{
	L"MyHitGroup_Triangle",
	L"MyHitGroup_Triangle_ShadowRay"
};

static const wchar_t* const HitGroupNames_AABBGeometry[INTERSECTION_SHADER_TYPE_COUNT][RAY_TYPE_COUNT] =
{
	{ L"MyHitGroup_AABB_AnalyticPrimitive", L"MyHitGroup_AABB_AnalyticPrimitive_ShadowRay" },
	{ L"MyHitGroup_AABB_VolumetricPrimitive", L"MyHitGroup_AABB_VolumetricPrimitive_ShadowRay" },
	{ L"MyHitGroup_AABB_SignedDistancePrimitive", L"MyHitGroup_AABB_SignedDistancePrimitive_ShadowRay" },
};

#define WM_INIT (WM_USER + 1)

struct DxObjects
{
	UINT BackBufferIndex;

	ID3D12CommandQueue* CommandQueue;
	ID3D12GraphicsCommandList7* CommandList;
	ID3D12CommandAllocator* CommandAllocators[BUFFER_COUNT];

	IDXGISwapChain3* SwapChain;
	ID3D12Resource* RenderTargets[BUFFER_COUNT];

	ID3D12StateObject* DxrStateObject;

	ID3D12RootSignature* RaytracingGlobalRootSignature;

	ID3D12DescriptorHeap* MainDescriptorHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap_CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap_GpuHandle;

	D3D12_CPU_DESCRIPTOR_HANDLE RtvDescriptorTop;

	UINT DescriptorsAllocated;

	UINT CbvDescriptorSize;
	UINT RtvDescriptorSize;


	uint8_t* MappedConstantBufferData;
	D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferGPUAddress;
	UINT ConstantBufferAlignedInstanceSize;
	struct SceneConstantBuffer ConstantBufferStaging;


	D3D12_GPU_VIRTUAL_ADDRESS StructuredBufferGpuAddress;
	struct PrimitiveInstancePerFrameBuffer* MappedBufferPointers;
	struct PrimitiveInstancePerFrameBuffer StructuredBufferStaging[TOTAL_PRIMITIVE_COUNT];


	D3D12_RAYTRACING_AABB AABB_Array[TOTAL_PRIMITIVE_COUNT];

	D3D12_GPU_DESCRIPTOR_HANDLE IndexBufferGPUHandle;

	D3D12_GPU_VIRTUAL_ADDRESS TopLevelASGPUHandle;

	ID3D12Resource* RaytracingOutput;

	D3D12_GPU_VIRTUAL_ADDRESS RayGenShaderTableGPUAddress;
	UINT64 RayGenShaderTableWidth;

	D3D12_GPU_VIRTUAL_ADDRESS MissShaderTableGPUAddress;
	UINT64 MissShaderTableWidth;
	UINT64 MissShaderTableStrideInBytes;

	D3D12_GPU_VIRTUAL_ADDRESS HitGroupShaderTableGPUAddress;
	UINT64 HitGroupShaderTableWidth;
	UINT64 HitGroupShaderTableStrideInBytes;


	ID3D12Fence* Fence;
	UINT64 FenceValues[BUFFER_COUNT];
	HANDLE FenceEvent;
};

#define MAX_GPU_TIMERS 8
#define GPU_TIMER_SLOTS (MAX_GPU_TIMERS * 2)

struct GPUTimer
{
	ID3D12QueryHeap* TimerHeap;
	ID3D12Resource* TimerBuffer;
	double GPU_FreqInv;
	float Avg[MAX_GPU_TIMERS];
	UINT64 Timing[GPU_TIMER_SLOTS];
	UINT UsedQueries;
};

struct CameraVectors
{
	vec4 EyeVector;
	vec4 AtVector;
	vec4 UpVector;
};

struct WindowProcPayload
{
	struct DxObjects* DxObjects;
	struct GPUTimer (*GpuTimers)[GPU_TIMER_COUNT];
	struct CameraVectors* CameraVectors;
};

void UpdateCameraMatrices(struct DxObjects* DxObjects, float AspectRatio, vec4 EyeVector, vec4 AtVector, vec4 UpVector)
{
	glm_vec4_copy(EyeVector, DxObjects->ConstantBufferStaging.cameraPosition);

	mat4 ViewMatrix;
	glm_lookat_lh(EyeVector, AtVector, UpVector, ViewMatrix);

	mat4 ProjectionMatrix;
	static const float FOV_ANGLE_Y = 45.0f;
	glm_perspective_lh_zo(glm_rad(FOV_ANGLE_Y), AspectRatio, 0.01f, 125.0f, ProjectionMatrix);

	mat4 ViewProjMatrix;
	glm_mat4_mul(ProjectionMatrix, ViewMatrix, ViewProjMatrix);

	glm_mat4_inv(ViewProjMatrix, DxObjects->ConstantBufferStaging.projectionToWorld);
}

LRESULT CALLBACK PreInitProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK IdleProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam);

void WaitForGpu(struct DxObjects* DxObjects)
{
	if (SUCCEEDED(ID3D12CommandQueue_Signal(DxObjects->CommandQueue, DxObjects->Fence, DxObjects->FenceValues[DxObjects->BackBufferIndex])))
	{
		if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(DxObjects->Fence, DxObjects->FenceValues[DxObjects->BackBufferIndex], DxObjects->FenceEvent)))
		{
			THROW_ON_FALSE(WaitForSingleObject(DxObjects->FenceEvent, INFINITE) == WAIT_OBJECT_0);
			DxObjects->FenceValues[DxObjects->BackBufferIndex]++;
		}
	}
}

void SetAttributes(
	struct PrimitiveConstantBuffer AABB_MaterialCB[static TOTAL_PRIMITIVE_COUNT],
	UINT PrimitiveIndex,
	vec4 Albedo,
	float ReflectanceCoef,
	float DiffuseCoef,
	float SpecularCoef,
	float SpecularPower,
	float StepScale)
{
	glm_vec4_copy(Albedo, AABB_MaterialCB[PrimitiveIndex].albedo);
	AABB_MaterialCB[PrimitiveIndex].reflectanceCoef = ReflectanceCoef;
	AABB_MaterialCB[PrimitiveIndex].diffuseCoef = DiffuseCoef;
	AABB_MaterialCB[PrimitiveIndex].specularCoef = SpecularCoef;
	AABB_MaterialCB[PrimitiveIndex].specularPower = SpecularPower;
	AABB_MaterialCB[PrimitiveIndex].stepScale = StepScale;
}

D3D12_RAYTRACING_AABB InitializeAABB(vec3 OffsetIndex, vec3 Size)
{
	return (D3D12_RAYTRACING_AABB) {
		(-(4 * AABB_WIDTH + (4 - 1) * AABB_DISTANCE) / 2.0f) + OffsetIndex[0] * (AABB_WIDTH + AABB_DISTANCE),
		(-(1 * AABB_WIDTH + (1 - 1) * AABB_DISTANCE) / 2.0f) + OffsetIndex[1] * (AABB_WIDTH + AABB_DISTANCE),
		(-(4 * AABB_WIDTH + (4 - 1) * AABB_DISTANCE) / 2.0f) + OffsetIndex[2] * (AABB_WIDTH + AABB_DISTANCE),
		(-(4 * AABB_WIDTH + (4 - 1) * AABB_DISTANCE) / 2.0f) + OffsetIndex[0] * (AABB_WIDTH + AABB_DISTANCE) + Size[0],
		(-(1 * AABB_WIDTH + (1 - 1) * AABB_DISTANCE) / 2.0f) + OffsetIndex[1] * (AABB_WIDTH + AABB_DISTANCE) + Size[1],
		(-(4 * AABB_WIDTH + (4 - 1) * AABB_DISTANCE) / 2.0f) + OffsetIndex[2] * (AABB_WIDTH + AABB_DISTANCE) + Size[2],
	};
}

void SetTransformForAABB(struct DxObjects* DxObjects, UINT PrimitiveIndex, mat4 ScalingMatrix, mat4 RotationMatrix, D3D12_RAYTRACING_AABB AABB_Array[static TOTAL_PRIMITIVE_COUNT])
{
	vec3 TranslationVector;
	glm_vec3_add(&AABB_Array[PrimitiveIndex].MinX, &AABB_Array[PrimitiveIndex].MaxX, TranslationVector);
	glm_vec3_scale(TranslationVector, 0.5f, TranslationVector);

	mat4 TranslationMatrix;
	glm_translate_make(TranslationMatrix, TranslationVector);

	mat4 TransformationMatrix;
	glm_mat4_mul(TranslationMatrix, RotationMatrix, TransformationMatrix);
	glm_mat4_mul(TransformationMatrix, ScalingMatrix, TransformationMatrix);

	glm_mat4_copy(TransformationMatrix, DxObjects->StructuredBufferStaging[PrimitiveIndex].localSpaceToBottomLevelAS);
	glm_mat4_inv(TransformationMatrix, DxObjects->StructuredBufferStaging[PrimitiveIndex].bottomLevelASToLocalSpace);
}

int main()
{
	ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

	THROW_ON_FAIL(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE));

	HINSTANCE Instance = GetModuleHandleW(NULL);

	HICON Icon = LoadIconW(NULL, IDI_APPLICATION);
	HCURSOR Cursor = LoadCursorW(NULL, IDC_ARROW);

	WNDCLASSEXW WindowClass = { 0 };
	WindowClass.cbSize = sizeof(WNDCLASSEXW);
	WindowClass.style = CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc = PreInitProc;
	WindowClass.cbClsExtra = 0;
	WindowClass.cbWndExtra = 0;
	WindowClass.hInstance = Instance;
	WindowClass.hIcon = Icon;
	WindowClass.hCursor = Cursor;
	WindowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 2);
	WindowClass.lpszMenuName = NULL;
	WindowClass.lpszClassName = WindowClassName;
	WindowClass.hIconSm = Icon;

	ATOM WindowClassAtom = RegisterClassExW(&WindowClass);
	if (WindowClassAtom == 0)
		THROW_ON_FAIL(HRESULT_FROM_WIN32(GetLastError()));

	RECT WindowRect = { 
		0,
		0,
		1280,
		720
	};

	AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, FALSE);

	HWND Window = CreateWindowExW(
		0L,
		WindowClassName,
		L"Loading...",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		WindowRect.right - WindowRect.left,
		WindowRect.bottom - WindowRect.top,
		NULL,
		NULL,
		Instance,
		NULL);

	THROW_ON_FALSE(ShowWindow(Window, SW_SHOW));

#ifdef _DEBUG
	ID3D12Debug6* DebugController;

	{
		ID3D12Debug* DebugControllerV1;
		THROW_ON_FAIL(D3D12GetDebugInterface(&IID_ID3D12Debug, &DebugControllerV1));
		THROW_ON_FAIL(ID3D12Debug_QueryInterface(DebugControllerV1, &IID_ID3D12Debug6, &DebugController));
		ID3D12Debug_Release(DebugControllerV1);
	}

	ID3D12Debug6_EnableDebugLayer(DebugController);
	ID3D12Debug6_SetEnableSynchronizedCommandQueueValidation(DebugController, TRUE);
	ID3D12Debug6_SetGPUBasedValidationFlags(DebugController, D3D12_GPU_BASED_VALIDATION_FLAGS_DISABLE_STATE_TRACKING);
	ID3D12Debug6_SetEnableGPUBasedValidation(DebugController, TRUE);
#endif

	IDXGIFactory6* Factory;

#ifdef _DEBUG
	THROW_ON_FAIL(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, &IID_IDXGIFactory6, &Factory));
#else
	THROW_ON_FAIL(CreateDXGIFactory2(0, &IID_IDXGIFactory6, &Factory));
#endif

	IDXGIAdapter1* Adapter;

	if (bWarp)
	{
		IDXGIFactory6_EnumWarpAdapter(Factory, &IID_IDXGIAdapter1, &Adapter);
	}
	else
	{
		IDXGIFactory6_EnumAdapterByGpuPreference(Factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, &Adapter);
	}

	THROW_ON_FAIL(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_12_1, &IID_ID3D12Device10, &Device));

	THROW_ON_FAIL(IDXGIAdapter1_Release(Adapter));

	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 FeatureSupportData = { 0 };
		THROW_ON_FAIL(ID3D12Device10_CheckFeatureSupport(Device, D3D12_FEATURE_D3D12_OPTIONS5, &FeatureSupportData, sizeof(FeatureSupportData)));
		assert(FeatureSupportData.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED &&
			"ERROR: DirectX Raytracing is not supported by your OS, GPU and/or driver.\n");
	}

#ifdef _DEBUG
	ID3D12InfoQueue* InfoQueue;
	THROW_ON_FAIL(ID3D12Device10_QueryInterface(Device, &IID_ID3D12InfoQueue, &InfoQueue));

	THROW_ON_FAIL(ID3D12InfoQueue_SetBreakOnSeverity(InfoQueue, D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE));
	THROW_ON_FAIL(ID3D12InfoQueue_SetBreakOnSeverity(InfoQueue, D3D12_MESSAGE_SEVERITY_ERROR, TRUE));
	THROW_ON_FAIL(ID3D12InfoQueue_SetBreakOnSeverity(InfoQueue, D3D12_MESSAGE_SEVERITY_WARNING, TRUE));
#endif

	struct DxObjects DxObjects = { 0 };

	{
		D3D12_COMMAND_QUEUE_DESC CommandQueueDesc = { 0 };
		CommandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		CommandQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		CommandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		THROW_ON_FAIL(ID3D12Device10_CreateCommandQueue(Device, &CommandQueueDesc, &IID_ID3D12CommandQueue, &DxObjects.CommandQueue));
	}

	{
		DXGI_SWAP_CHAIN_DESC SwapChainDesc = { 0 };
		SwapChainDesc.BufferDesc.Width = 1;
		SwapChainDesc.BufferDesc.Height = 1;
		SwapChainDesc.BufferDesc.Format = RTV_FORMAT;
		SwapChainDesc.SampleDesc.Count = 1;
		SwapChainDesc.SampleDesc.Quality = 0;
		SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		SwapChainDesc.BufferCount = BUFFER_COUNT;
		SwapChainDesc.OutputWindow = Window;
		SwapChainDesc.Windowed = TRUE;
		SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		SwapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		IDXGISwapChain* SwapChain_temp;
		THROW_ON_FAIL(IDXGIFactory6_CreateSwapChain(Factory, DxObjects.CommandQueue, &SwapChainDesc, &SwapChain_temp));
		IDXGISwapChain_QueryInterface(SwapChain_temp, &IID_IDXGISwapChain3, &DxObjects.SwapChain);
		THROW_ON_FAIL(IDXGISwapChain_Release(SwapChain_temp));
	}

	THROW_ON_FAIL(IDXGIFactory6_MakeWindowAssociation(Factory, Window, DXGI_MWA_NO_ALT_ENTER));

	THROW_ON_FAIL(IDXGIFactory6_Release(Factory));

	ID3D12DescriptorHeap* RtvDescriptorHeap;

	{
		D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc = { 0 };
		DescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		DescriptorHeapDesc.NumDescriptors = BUFFER_COUNT;
		DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		THROW_ON_FAIL(ID3D12Device10_CreateDescriptorHeap(Device, &DescriptorHeapDesc, &IID_ID3D12DescriptorHeap, &RtvDescriptorHeap));
	}

	ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(RtvDescriptorHeap, &DxObjects.RtvDescriptorTop);

	
	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		THROW_ON_FAIL(ID3D12Device10_CreateCommandAllocator(Device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, &DxObjects.CommandAllocators[i]));
	}

	THROW_ON_FAIL(ID3D12Device10_CreateCommandList(Device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, DxObjects.CommandAllocators[0], NULL, &IID_ID3D12GraphicsCommandList7, &DxObjects.CommandList));
	THROW_ON_FAIL(ID3D12GraphicsCommandList7_Close(DxObjects.CommandList));

	DxObjects.BackBufferIndex = 0;
	THROW_ON_FAIL(ID3D12Device10_CreateFence(Device, DxObjects.FenceValues[DxObjects.BackBufferIndex], D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, &DxObjects.Fence));
	DxObjects.FenceValues[DxObjects.BackBufferIndex]++;

	DxObjects.FenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	VALIDATE_HANDLE(DxObjects.FenceEvent);

	struct PrimitiveConstantBuffer AABB_MaterialCB[TOTAL_PRIMITIVE_COUNT];

	{
		static const vec4 RED = { 1.0f, 0.5f, 0.5f, 1.0f };
		static const vec4 GREEN = { 0.1f, 1.0f, 0.5f, 1.0f };
		static const vec4 YELLOW = { 1.0f, 1.0f, 0.5f, 1.0f };

		UINT Offset = 0;

		// Analytic primitives.
		SetAttributes(AABB_MaterialCB, Offset + ANALYTIC_PRIMITIVE_AABB, RED, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f);
		SetAttributes(AABB_MaterialCB, Offset + ANALYTIC_PRIMITIVE_SPHERES, ChromiumReflectance, 1, 0.9f, 0.7f, 50.0f, 1.0f);
		Offset += ANALYTIC_PRIMITIVE_COUNT;

		// Volumetric primitives.
		SetAttributes(AABB_MaterialCB, Offset + VOLUMETRIC_PRIMITIVE_METABALLS, ChromiumReflectance, 1, 0.9f, 0.7f, 50.0f, 1.0f);
		Offset += VOLUMETRIC_PRIMITIVE_COUNT;

		// Signed distance primitives.
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_MINISPHERES, GREEN, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f);
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_INTERSECTED_ROUND_CUBE, GREEN, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f);
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_SQUARE_TORUS, ChromiumReflectance, 1, 0.9f, 0.7f, 50.0f, 1.0f);
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_TWISTED_TORUS, YELLOW, 0, 1.0f, 0.7f, 50, 0.5f);
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_COG, YELLOW, 0, 1.0f, 0.1f, 2, 1.0f);
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_CYLINDER, RED, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f);
		SetAttributes(AABB_MaterialCB, Offset + SIGNED_DISTANCE_PRIMITIVE_FRACTAL_PYRAMID, GREEN, 0, 1, 0.1f, 4, 0.8f);
	}

	struct CameraVectors CameraVectors = { 0 };

	{
		glm_vec4_copy((vec4) { 0.0f, 5.3f, -17.0f, 1.0f }, CameraVectors.EyeVector);
		glm_vec4_copy((vec4) { 0.0f, 0.0f, 0.0f, 1.0f }, CameraVectors.AtVector);

		vec4 CameraDirection;
		glm_vec4_sub(CameraVectors.AtVector, CameraVectors.EyeVector, CameraDirection);
		glm_vec4_normalize_to(CameraDirection, CameraDirection);

		vec4 CameraRight = { 1.0f, 0.0f, 0.0f, 0.0f };
		glm_cross(CameraDirection, CameraRight, CameraVectors.UpVector);

		mat4 IdentityMatrix;
		glm_mat4_identity(IdentityMatrix);

		mat4 CameraRotation;
		glm_rotate_y(IdentityMatrix, glm_rad(45.0f), CameraRotation);

		glm_mat4_mulv(CameraRotation, CameraVectors.EyeVector, CameraVectors.EyeVector);

		glm_mat4_mulv(CameraRotation, CameraVectors.UpVector, CameraVectors.UpVector);
	}

	glm_vec4_copy((vec4) { 0.0f, 18.0f, -20.0f, 0.0f }, DxObjects.ConstantBufferStaging.lightPosition);

	glm_vec4_copy((vec4) { 0.25f, 0.25f, 0.25f, 1.0f }, DxObjects.ConstantBufferStaging.lightAmbientColor);

	glm_vec4_copy((vec4) { 0.6f, 0.6f, 0.6f, 1.0f }, DxObjects.ConstantBufferStaging.lightDiffuseColor);

	struct GPUTimer GpuTimers[GPU_TIMER_COUNT] = { 0 };

	for (int i = 0; i < ARRAYSIZE(GpuTimers); i++)
	{
		GpuTimers[i].GPU_FreqInv = 1.f;

		{
			UINT64 GpuFrequency;
			THROW_ON_FAIL(ID3D12CommandQueue_GetTimestampFrequency(DxObjects.CommandQueue, &GpuFrequency));
			GpuTimers[i].GPU_FreqInv = 1000.0 / (double)GpuFrequency;
		}

		{
			D3D12_QUERY_HEAP_DESC QueryHeapDesc = { 0 };
			QueryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			QueryHeapDesc.Count = GPU_TIMER_SLOTS;
			THROW_ON_FAIL(ID3D12Device10_CreateQueryHeap(Device, &QueryHeapDesc, &IID_ID3D12QueryHeap, &GpuTimers[i].TimerHeap));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12QueryHeap_SetName(GpuTimers[i].TimerHeap, L"GPUTimerHeap"));
#endif

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_READBACK;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			// We allocate FrameCount + 1 instances as an instance is guaranteed to be written to if maxPresentFrameCount frames
			// have been submitted since. This is due to a fact that Present stalls when none of the FrameCount frames are done/available.

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = (BUFFER_COUNT + 1) * GPU_TIMER_SLOTS * sizeof(UINT64);
			ResourceDesc.Height = 1;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
				Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&GpuTimers[i].TimerBuffer));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(GpuTimers[i].TimerBuffer, L"GPU Timer Buffer"));
#endif
	}

	// This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		D3D12_DESCRIPTOR_RANGE Ranges[2] = { 0 }; // Perfomance TIP: Order from most frequent to least frequent.
		Ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; // 1 output texture
		Ranges[0].NumDescriptors = 1;
		Ranges[0].BaseShaderRegister = 0;
		Ranges[0].RegisterSpace = 0;
		Ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		Ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // 2 static index and vertex buffers.
		Ranges[1].NumDescriptors = 2;
		Ranges[1].BaseShaderRegister = 1;
		Ranges[1].RegisterSpace = 0;
		Ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER RootParameters[ROOTSIG_SLOT_COUNT] = { 0 };
		RootParameters[ROOTSIG_SLOT_OUTPUTVIEW].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		RootParameters[ROOTSIG_SLOT_OUTPUTVIEW].DescriptorTable.NumDescriptorRanges = 1;
		RootParameters[ROOTSIG_SLOT_OUTPUTVIEW].DescriptorTable.pDescriptorRanges = &Ranges[0];
		RootParameters[ROOTSIG_SLOT_OUTPUTVIEW].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
					
		RootParameters[ROOTSIG_SLOT_ACC_STRUCT].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		RootParameters[ROOTSIG_SLOT_ACC_STRUCT].Descriptor.ShaderRegister = 0;
		RootParameters[ROOTSIG_SLOT_ACC_STRUCT].Descriptor.RegisterSpace = 0;
		RootParameters[ROOTSIG_SLOT_ACC_STRUCT].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		RootParameters[ROOTSIG_SLOT_SCENE_CB].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		RootParameters[ROOTSIG_SLOT_SCENE_CB].Descriptor.ShaderRegister = 0;
		RootParameters[ROOTSIG_SLOT_SCENE_CB].Descriptor.RegisterSpace = 0;
		RootParameters[ROOTSIG_SLOT_SCENE_CB].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		RootParameters[ROOTSIG_SLOT_AABB_ATTRIBUTE_BUFFER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		RootParameters[ROOTSIG_SLOT_AABB_ATTRIBUTE_BUFFER].Descriptor.ShaderRegister = 3;
		RootParameters[ROOTSIG_SLOT_AABB_ATTRIBUTE_BUFFER].Descriptor.RegisterSpace = 0;
		RootParameters[ROOTSIG_SLOT_AABB_ATTRIBUTE_BUFFER].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		RootParameters[ROOTSIG_SLOT_VERTEX_BUFFERS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		RootParameters[ROOTSIG_SLOT_VERTEX_BUFFERS].DescriptorTable.NumDescriptorRanges = 1;
		RootParameters[ROOTSIG_SLOT_VERTEX_BUFFERS].DescriptorTable.pDescriptorRanges = &Ranges[1];
		RootParameters[ROOTSIG_SLOT_VERTEX_BUFFERS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc = { 0 };
		RootSignatureDesc.NumParameters = ARRAYSIZE(RootParameters);
		RootSignatureDesc.pParameters = RootParameters;
		RootSignatureDesc.NumStaticSamplers = 0;
		RootSignatureDesc.pStaticSamplers = NULL;
		RootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
			
		ID3D10Blob* Signature;
		THROW_ON_FAIL(D3D12SerializeRootSignature(&RootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Signature, NULL));
		THROW_ON_FAIL(ID3D12Device10_CreateRootSignature(Device, 1, ID3D10Blob_GetBufferPointer(Signature), ID3D10Blob_GetBufferSize(Signature), &IID_ID3D12RootSignature, &DxObjects.RaytracingGlobalRootSignature));
		ID3D10Blob_Release(Signature);
	}

	// Local Root Signature
	// This is a root signature that enables a shader to have unique arguments that come from shader tables.

	ID3D12RootSignature* RaytracingLocalRootSignature[ROOTSIG_TYPE_COUNT];

	{
		D3D12_ROOT_PARAMETER RootParameters[ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_COUNT] = { 0 };
		RootParameters[ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_MATERIAL_CONSTANT].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		RootParameters[ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_MATERIAL_CONSTANT].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		RootParameters[ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_MATERIAL_CONSTANT].Constants.ShaderRegister = 1;
		RootParameters[ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_MATERIAL_CONSTANT].Constants.RegisterSpace = 0;
		RootParameters[ROOTSIG_TRIANGLE_SLOT_MATERIAL_CONSTANT_MATERIAL_CONSTANT].Constants.Num32BitValues = SizeOfInUint32(struct PrimitiveConstantBuffer);

		D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc = { 0 };
		RootSignatureDesc.NumParameters = ARRAYSIZE(RootParameters);
		RootSignatureDesc.pParameters = RootParameters;
		RootSignatureDesc.NumStaticSamplers = 0;
		RootSignatureDesc.pStaticSamplers = NULL;
		RootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		ID3D10Blob* Signature;
		THROW_ON_FAIL(D3D12SerializeRootSignature(&RootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Signature, NULL));
		THROW_ON_FAIL(ID3D12Device10_CreateRootSignature(Device, 1, ID3D10Blob_GetBufferPointer(Signature), ID3D10Blob_GetBufferSize(Signature), &IID_ID3D12RootSignature, &RaytracingLocalRootSignature[ROOTSIG_TYPE_TRIANGLE]));
		ID3D10Blob_Release(Signature);
	}

	{
		D3D12_ROOT_PARAMETER RootParameters[ROOTSIG_AABB_SLOT_COUNT] = { 0 };
		RootParameters[ROOTSIG_AABB_SLOT_MATERIALCONSTANT].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		RootParameters[ROOTSIG_AABB_SLOT_MATERIALCONSTANT].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		RootParameters[ROOTSIG_AABB_SLOT_MATERIALCONSTANT].Constants.Num32BitValues = SizeOfInUint32(struct PrimitiveConstantBuffer);
		RootParameters[ROOTSIG_AABB_SLOT_MATERIALCONSTANT].Constants.ShaderRegister = 1;
		RootParameters[ROOTSIG_AABB_SLOT_MATERIALCONSTANT].Constants.RegisterSpace = 0;

		RootParameters[ROOTSIG_AABB_SLOT_GEOMETRY_INDEX].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		RootParameters[ROOTSIG_AABB_SLOT_GEOMETRY_INDEX].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		RootParameters[ROOTSIG_AABB_SLOT_GEOMETRY_INDEX].Constants.Num32BitValues = SizeOfInUint32(struct PrimitiveInstanceConstantBuffer);
		RootParameters[ROOTSIG_AABB_SLOT_GEOMETRY_INDEX].Constants.ShaderRegister = 2;
		RootParameters[ROOTSIG_AABB_SLOT_GEOMETRY_INDEX].Constants.RegisterSpace = 0;

		D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc = { 0 };
		RootSignatureDesc.NumParameters = ARRAYSIZE(RootParameters);
		RootSignatureDesc.pParameters = RootParameters;
		RootSignatureDesc.NumStaticSamplers = 0;
		RootSignatureDesc.pStaticSamplers = NULL;
		RootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		ID3D10Blob* Signature;
		THROW_ON_FAIL(D3D12SerializeRootSignature(&RootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Signature, NULL));
		THROW_ON_FAIL(ID3D12Device10_CreateRootSignature(Device, 1, ID3D10Blob_GetBufferPointer(Signature), ID3D10Blob_GetBufferSize(Signature), &IID_ID3D12RootSignature, &RaytracingLocalRootSignature[ROOTSIG_TYPE_AABB]));
		ID3D10Blob_Release(Signature);
	}

	{
		// Create a raytracing pipeline state object (RTPSO) which defines the binding of shaders, state and resources to be used during raytracing.
		// An RTPSO represents a full set of shaders reachable by a DispatchRays() call,
		// with all configuration options resolved, such as local signatures and other state.
		// Create 18 subobjects that combine into a RTPSO:
		// Subobjects need to be associated with DXIL exports (i.e. shaders) either by way of default or explicit associations.
		// Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
		// This simple sample utilizes default shader association except for local root signature subobject
		// which has an explicit association specified purely for demonstration purposes.
		// 1 - DXIL library
		// 8 - Hit group types - 4 geometries (1 triangle, 3 aabb) x 2 ray types (ray, shadowRay)
		// 1 - Shader config
		// 6 - 3 x Local root signature and association
		// 1 - Global root signature
		// 1 - Pipeline config

		D3D12_STATE_SUBOBJECT StateObjectVector[16] = { 0 };
		size_t StateObjectVectorSize = 0;

		HANDLE RTShaderFile = CreateFileW(L"Raytracing.cso", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		VALIDATE_HANDLE(RTShaderFile);

		LONGLONG RTShaderSize;
		THROW_ON_FALSE(GetFileSizeEx(RTShaderFile, &RTShaderSize));

		HANDLE RTShaderFileMap = CreateFileMappingW(RTShaderFile, NULL, PAGE_READONLY, 0, 0, NULL);
		VALIDATE_HANDLE(RTShaderFileMap);

		const void* RTShaderBytecode = MapViewOfFile(RTShaderFileMap, FILE_MAP_READ, 0, 0, 0);

		// DXIL library
		// This contains the shaders and their entrypoints for the state object.
		// Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
		D3D12_DXIL_LIBRARY_DESC DxilLibDesc = { 0 };
		DxilLibDesc.DXILLibrary.pShaderBytecode = RTShaderBytecode;
		DxilLibDesc.DXILLibrary.BytecodeLength = RTShaderSize;
		DxilLibDesc.NumExports = 0;
		DxilLibDesc.pExports = NULL;

		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		StateObjectVector[StateObjectVectorSize].pDesc = &DxilLibDesc;
		StateObjectVectorSize++;

		// Use default shader exports for a DXIL library/collection subobject ~ surface all shaders.
		// Hit groups
		// A hit group specifies closest hit, any hit and intersection shaders 
		// to be executed when a ray intersects the geometry.
		// Triangle geometry hit groups
		D3D12_HIT_GROUP_DESC HitGroupDesc[RAY_TYPE_COUNT] = { 0 };
		for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
		{
			if (RayType == RAY_TYPE_RADIANCE)
			{
				HitGroupDesc[RayType].ClosestHitShaderImport = ClosestHitShaderNames[GEOMETRY_TYPE_TRIANGLE];
			}

			HitGroupDesc[RayType].HitGroupExport = HitGroupNames_TriangleGeometry[RayType];
			HitGroupDesc[RayType].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;

			StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
			StateObjectVector[StateObjectVectorSize].pDesc = &HitGroupDesc[RayType];
			StateObjectVectorSize++;
		}

		// AABB geometry hit groups
		// Create hit groups for each intersection shader.
		D3D12_HIT_GROUP_DESC HitGroupDesc2[INTERSECTION_SHADER_TYPE_COUNT][RAY_TYPE_COUNT] = { 0 };
		for (enum INTERSECTION_SHADER_TYPE IntersectionShaderType = 0; IntersectionShaderType < INTERSECTION_SHADER_TYPE_COUNT; IntersectionShaderType++)
		{
			for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
			{
				HitGroupDesc2[IntersectionShaderType][RayType].IntersectionShaderImport = IntersectionShaderNames[IntersectionShaderType];
				if (RayType == RAY_TYPE_RADIANCE)
				{
					HitGroupDesc2[IntersectionShaderType][RayType].ClosestHitShaderImport = ClosestHitShaderNames[GEOMETRY_TYPE_AABB];
				}
				HitGroupDesc2[IntersectionShaderType][RayType].HitGroupExport = HitGroupNames_AABBGeometry[IntersectionShaderType][RayType];
				HitGroupDesc2[IntersectionShaderType][RayType].Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;

				StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
				StateObjectVector[StateObjectVectorSize].pDesc = &HitGroupDesc2[IntersectionShaderType][RayType];
				StateObjectVectorSize++;
			}
		}

		// Shader config
		// Defines the maximum sizes in bytes for the ray rayPayload and attribute structure.
		D3D12_RAYTRACING_SHADER_CONFIG ShaderConfig = { 0 };
		ShaderConfig.MaxPayloadSizeInBytes = max(sizeof(struct RayPayload), sizeof(struct ShadowRayPayload));
		ShaderConfig.MaxAttributeSizeInBytes = sizeof(struct ProceduralPrimitiveAttributes);

		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
		StateObjectVector[StateObjectVectorSize].pDesc = &ShaderConfig;
		StateObjectVectorSize++;

		// Local root signature and shader association
		// This is a root signature that enables a shader to have unique arguments that come from shader tables.
		D3D12_LOCAL_ROOT_SIGNATURE LocalRootSignature = { 0 };
		LocalRootSignature.pLocalRootSignature = RaytracingLocalRootSignature[ROOTSIG_TYPE_TRIANGLE];
		
		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		StateObjectVector[StateObjectVectorSize].pDesc = &LocalRootSignature;
		StateObjectVectorSize++;

		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION ToExportsAsociation = { 0 };
		ToExportsAsociation.pSubobjectToAssociate = &StateObjectVector[StateObjectVectorSize - 1];
		ToExportsAsociation.NumExports = ARRAYSIZE(HitGroupNames_TriangleGeometry);
		ToExportsAsociation.pExports = HitGroupNames_TriangleGeometry;

		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		StateObjectVector[StateObjectVectorSize].pDesc = &ToExportsAsociation;
		StateObjectVectorSize++;

		D3D12_LOCAL_ROOT_SIGNATURE LocalRootSignature2 = { 0 };
		LocalRootSignature2.pLocalRootSignature = RaytracingLocalRootSignature[ROOTSIG_TYPE_AABB];
			
		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		StateObjectVector[StateObjectVectorSize].pDesc = &LocalRootSignature2;
		StateObjectVectorSize++;

		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION ToExportsAsociation2 = { 0 };
		ToExportsAsociation2.pSubobjectToAssociate = &StateObjectVector[StateObjectVectorSize - 1];
		ToExportsAsociation2.NumExports = sizeof(HitGroupNames_AABBGeometry)/sizeof(**HitGroupNames_AABBGeometry);
		ToExportsAsociation2.pExports = HitGroupNames_AABBGeometry;

		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		StateObjectVector[StateObjectVectorSize].pDesc = &ToExportsAsociation2;
		StateObjectVectorSize++;

		// This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
		D3D12_GLOBAL_ROOT_SIGNATURE GlobalRootSignature = { 0 };
		GlobalRootSignature.pGlobalRootSignature = DxObjects.RaytracingGlobalRootSignature;

		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
		StateObjectVector[StateObjectVectorSize].pDesc = &GlobalRootSignature;
		StateObjectVectorSize++;

		// Defines the maximum TraceRay() recursion depth.
		D3D12_RAYTRACING_PIPELINE_CONFIG RaytracingPipelineConfig = { 0 };
		RaytracingPipelineConfig.MaxTraceRecursionDepth = MAX_RAY_RECURSION_DEPTH;

		StateObjectVector[StateObjectVectorSize].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
		StateObjectVector[StateObjectVectorSize].pDesc = &RaytracingPipelineConfig;
		StateObjectVectorSize++;
		
		assert(StateObjectVectorSize == ARRAYSIZE(StateObjectVector));

		D3D12_STATE_OBJECT_DESC RaytracingPipelineDesc = { 0 };
		RaytracingPipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		RaytracingPipelineDesc.NumSubobjects = ARRAYSIZE(StateObjectVector);
		RaytracingPipelineDesc.pSubobjects = StateObjectVector;

		THROW_ON_FAIL(ID3D12Device10_CreateStateObject(Device, &RaytracingPipelineDesc, &IID_ID3D12StateObject, &DxObjects.DxrStateObject));

		THROW_ON_FALSE(UnmapViewOfFile(RTShaderBytecode));
		THROW_ON_FALSE(CloseHandle(RTShaderFileMap));
		THROW_ON_FALSE(CloseHandle(RTShaderFile));
	}

	{
		// Allocate a heap for 6 descriptors:
		// 2 - vertex and index  buffer SRVs
		// 1 - raytracing output texture SRV
		D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc = { 0 };
		DescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		DescriptorHeapDesc.NumDescriptors = DESCRIPTOR_COUNT;
		DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		THROW_ON_FAIL(ID3D12Device10_CreateDescriptorHeap(Device, &DescriptorHeapDesc, &IID_ID3D12DescriptorHeap, &DxObjects.MainDescriptorHeap));
	}

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12DescriptorHeap_SetName(DxObjects.MainDescriptorHeap, L"main descriptor heap"));
#endif
	
	ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects.MainDescriptorHeap, &DxObjects.DescriptorHeap_CpuHandle);
	ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(DxObjects.MainDescriptorHeap, &DxObjects.DescriptorHeap_GpuHandle);

	DxObjects.CbvDescriptorSize = ID3D12Device10_GetDescriptorHandleIncrementSize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	DxObjects.RtvDescriptorSize = ID3D12Device10_GetDescriptorHandleIncrementSize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	{
		UINT Offset = 0;

		// Analytic primitives.
		DxObjects.AABB_Array[Offset + ANALYTIC_PRIMITIVE_AABB] = InitializeAABB((vec3) { 3, 0, 0 }, (vec3) { 2, 3, 2 });
		DxObjects.AABB_Array[Offset + ANALYTIC_PRIMITIVE_SPHERES] = InitializeAABB((vec3) { 2.25f, 0, 0.75f }, (vec3) { 3, 3, 3 });
		Offset += ANALYTIC_PRIMITIVE_COUNT;

		// Volumetric primitives.
		DxObjects.AABB_Array[Offset + VOLUMETRIC_PRIMITIVE_METABALLS] = InitializeAABB((vec3) { 0, 0, 0 }, (vec3) { 3, 3, 3 });
		Offset += VOLUMETRIC_PRIMITIVE_COUNT;

		// Signed distance primitives.
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_MINISPHERES] = InitializeAABB((vec3) { 2, 0, 0 }, (vec3) { 2, 2, 2 });
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_TWISTED_TORUS] = InitializeAABB((vec3) { 0, 0, 1 }, (vec3) { 2, 2, 2 });
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_INTERSECTED_ROUND_CUBE] = InitializeAABB((vec3) { 0, 0, 2 }, (vec3) { 2, 2, 2 });
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_SQUARE_TORUS] = InitializeAABB((vec3) { 0.75f, -0.1f, 2.25f }, (vec3) { 3, 3, 3 });
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_COG] = InitializeAABB((vec3) { 1, 0, 0 }, (vec3) { 2, 2, 2 });
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_CYLINDER] = InitializeAABB((vec3) { 0, 0, 3 }, (vec3) { 2, 3, 2 });
		DxObjects.AABB_Array[Offset + SIGNED_DISTANCE_PRIMITIVE_FRACTAL_PYRAMID] = InitializeAABB((vec3) { 2, 0, 2 }, (vec3) { 6, 6, 6 });
	}

	ID3D12Resource* AABB_Buffer;
	
	{
		D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
		HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ResourceDesc.Alignment = 0;
		ResourceDesc.Width = sizeof(DxObjects.AABB_Array);
		ResourceDesc.Height = 1;
		ResourceDesc.DepthOrArraySize = 1;
		ResourceDesc.MipLevels = 1;
		ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		ResourceDesc.SampleDesc.Count = 1;
		ResourceDesc.SampleDesc.Quality = 0;
		ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
			Device,
			&HeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_BARRIER_LAYOUT_UNDEFINED,
			NULL,
			NULL,
			0,
			NULL,
			&IID_ID3D12Resource,
			&AABB_Buffer));
	}

	{
		void* MappedData;
		ID3D12Resource_Map(AABB_Buffer, 0, NULL, &MappedData);
		memcpy(MappedData, DxObjects.AABB_Array, sizeof(DxObjects.AABB_Array));
		ID3D12Resource_Unmap(AABB_Buffer, 0, NULL);
	}

	// Plane Indices.
	UINT16 Indices[] =
	{
		3, 1, 0,
		2, 1, 3
	};

	ID3D12Resource* IndexBufferResource;
	D3D12_CPU_DESCRIPTOR_HANDLE IndexBufferCPUHandle;

	{
		D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
		HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ResourceDesc.Alignment = 0;
		ResourceDesc.Width = sizeof(Indices);
		ResourceDesc.Height = 1;
		ResourceDesc.DepthOrArraySize = 1;
		ResourceDesc.MipLevels = 1;
		ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		ResourceDesc.SampleDesc.Count = 1;
		ResourceDesc.SampleDesc.Quality = 0;
		ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
			Device,
			&HeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_BARRIER_LAYOUT_UNDEFINED,
			NULL,
			NULL,
			0,
			NULL,
			&IID_ID3D12Resource,
			&IndexBufferResource));
	}
		
	{
		void* MappedData;
		ID3D12Resource_Map(IndexBufferResource, 0, NULL, &MappedData);
		memcpy(MappedData, Indices, sizeof(Indices));
		ID3D12Resource_Unmap(IndexBufferResource, 0, NULL);
	}

	// Cube Vertices positions and corresponding triangle normals.
	struct Vertex Vertices[] =
	{
		{{ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }},
		{{ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }},
		{{ 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }},
		{{ 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }}
	};

	ID3D12Resource* VertexBufferResource;

	{
		D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
		HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ResourceDesc.Alignment = 0;
		ResourceDesc.Width = sizeof(Vertices);
		ResourceDesc.Height = 1;
		ResourceDesc.DepthOrArraySize = 1;
		ResourceDesc.MipLevels = 1;
		ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		ResourceDesc.SampleDesc.Count = 1;
		ResourceDesc.SampleDesc.Quality = 0;
		ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
			Device,
			&HeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_BARRIER_LAYOUT_UNDEFINED,
			NULL,
			NULL,
			0,
			NULL,
			&IID_ID3D12Resource,
			&VertexBufferResource));
	}
		
	{
		void* MappedData;
		ID3D12Resource_Map(VertexBufferResource, 0, NULL, &MappedData);
		memcpy(MappedData, Vertices, sizeof(Vertices));
		ID3D12Resource_Unmap(VertexBufferResource, 0, NULL);
	}

	// Vertex buffer is passed to the shader along with index buffer as a descriptor range.
	DxObjects.DescriptorsAllocated = 0;
	
	{
		const UINT IndexBufferDescriptorIndex = DxObjects.DescriptorsAllocated++;
		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects.MainDescriptorHeap, &IndexBufferCPUHandle);
		IndexBufferCPUHandle.ptr += IndexBufferDescriptorIndex * DxObjects.CbvDescriptorSize;

		const UINT VertexBufferDescriptorIndex = DxObjects.DescriptorsAllocated++;

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC ResourceViewDesc = { 0 };
			ResourceViewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			ResourceViewDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			ResourceViewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			ResourceViewDesc.Buffer.NumElements = sizeof(Indices) / 4;
			ResourceViewDesc.Buffer.StructureByteStride = 0;
			ResourceViewDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			ID3D12Device10_CreateShaderResourceView(Device, IndexBufferResource, &ResourceViewDesc, IndexBufferCPUHandle);
		}

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC ResourceViewDesc = { 0 };
			ResourceViewDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceViewDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			ResourceViewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			ResourceViewDesc.Buffer.NumElements = ARRAYSIZE(Vertices);
			ResourceViewDesc.Buffer.StructureByteStride = sizeof(Vertices[0]);
			ResourceViewDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

			D3D12_CPU_DESCRIPTOR_HANDLE VertexBufferCPUHandle;
			ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects.MainDescriptorHeap, &VertexBufferCPUHandle);
			VertexBufferCPUHandle.ptr += VertexBufferDescriptorIndex * DxObjects.CbvDescriptorSize;

			ID3D12Device10_CreateShaderResourceView(Device, VertexBufferResource, &ResourceViewDesc, VertexBufferCPUHandle);
		}

		DxObjects.IndexBufferGPUHandle.ptr = DxObjects.DescriptorHeap_GpuHandle.ptr + IndexBufferDescriptorIndex * DxObjects.CbvDescriptorSize;
			
		assert(VertexBufferDescriptorIndex == IndexBufferDescriptorIndex + 1 && L"Vertex Buffer descriptor index must follow that of Index Buffer descriptor index");
	}

	// Build raytracing acceleration structures from the generated geometry.
	// Build acceleration structure needed for raytracing.
	// Reset the command list for the acceleration structure construction.
	ID3D12GraphicsCommandList7_Reset(DxObjects.CommandList, DxObjects.CommandAllocators[DxObjects.BackBufferIndex], NULL);
	
	struct AccelerationStructureBuffers
	{
		ID3D12Resource* Scratch;
		ID3D12Resource* AccelerationStructure;
		ID3D12Resource* InstanceDesc;
		UINT64 ResultDataMaxSizeInBytes;
	};

	struct AccelerationStructureBuffers BottomLevelAS[GEOMETRY_TYPE_COUNT] = { 0 };

	struct AccelerationStructureBuffers TopLevelAS = { 0 };
	ID3D12Resource* TopLevelASResource;

	{
		struct PointerWithSize
		{
			D3D12_RAYTRACING_GEOMETRY_DESC* Ptr;
			size_t Size;
		};

		struct PointerWithSize GeometryDescs[GEOMETRY_TYPE_COUNT] = { 0 };
		
		D3D12_RAYTRACING_GEOMETRY_DESC TriangleRaytracingGeometryDesc[1] = { 0 };
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr = TriangleRaytracingGeometryDesc;
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Size = ARRAYSIZE(TriangleRaytracingGeometryDesc);

		D3D12_RAYTRACING_GEOMETRY_DESC AABBRaytracingGeometryDesc[TOTAL_PRIMITIVE_COUNT] = { 0 };
		GeometryDescs[GEOMETRY_TYPE_AABB].Ptr = AABBRaytracingGeometryDesc;
		GeometryDescs[GEOMETRY_TYPE_AABB].Size = ARRAYSIZE(AABBRaytracingGeometryDesc);

		// Mark the geometry as opaque. 
		// PERFORMANCE TIP: mark geometry as opaque whenever applicable as it can enable important ray processing optimizations.
		// Note: When rays encounter opaque geometry an any hit shader will not be executed whether it is present or not.

		// Triangle geometry desc
		// Triangle bottom-level AS contains a single plane geometry.
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.IndexBuffer = ID3D12Resource_GetGPUVirtualAddress(IndexBufferResource);
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.IndexCount = ARRAYSIZE(Indices);
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.VertexCount = ARRAYSIZE(Vertices);
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.VertexBuffer.StartAddress = ID3D12Resource_GetGPUVirtualAddress(VertexBufferResource);
		GeometryDescs[GEOMETRY_TYPE_TRIANGLE].Ptr[0].Triangles.VertexBuffer.StrideInBytes = sizeof(struct Vertex);

		// AABB geometry desc
		{
			D3D12_RAYTRACING_GEOMETRY_DESC RaytracingGeometryDesc = { 0 };
			RaytracingGeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
			RaytracingGeometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			RaytracingGeometryDesc.AABBs.AABBCount = 1;
			RaytracingGeometryDesc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);

			// Create AABB geometries. 
			// Having separate geometries allows of separate shader record binding per geometry.
			// In this sample, this lets us specify custom hit groups per AABB geometry.
			for (int i = 0; i < TOTAL_PRIMITIVE_COUNT; i++)
			{
				memcpy(&GeometryDescs[GEOMETRY_TYPE_AABB].Ptr[i], &RaytracingGeometryDesc, sizeof(D3D12_RAYTRACING_GEOMETRY_DESC));
				GeometryDescs[GEOMETRY_TYPE_AABB].Ptr[i].AABBs.AABBs.StartAddress = ID3D12Resource_GetGPUVirtualAddress(AABB_Buffer) + i * sizeof(D3D12_RAYTRACING_AABB);
			}
		}

		// Build all bottom-level AS.
		for (enum GEOMETRY_TYPE GeometryType = 0; GeometryType < GEOMETRY_TYPE_COUNT; GeometryType++)
		{
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BottomLevelBuildDesc = { 0 };
			BottomLevelBuildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			BottomLevelBuildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			BottomLevelBuildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			BottomLevelBuildDesc.Inputs.NumDescs = GeometryDescs[GeometryType].Size;
			BottomLevelBuildDesc.Inputs.pGeometryDescs = GeometryDescs[GeometryType].Ptr;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO BottomLevelPrebuildInfo = { 0 };
			ID3D12Device10_GetRaytracingAccelerationStructurePrebuildInfo(Device, &BottomLevelBuildDesc.Inputs, &BottomLevelPrebuildInfo);
			assert(BottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

			{
				D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
				HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
				HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
				ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				ResourceDesc.Alignment = 0;
				ResourceDesc.Width = BottomLevelPrebuildInfo.ScratchDataSizeInBytes;
				ResourceDesc.Height = 1;
				ResourceDesc.DepthOrArraySize = 1;
				ResourceDesc.MipLevels = 1;
				ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
				ResourceDesc.SampleDesc.Count = 1;
				ResourceDesc.SampleDesc.Quality = 0;
				ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

				THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
					Device,
					&HeapProperties,
					D3D12_HEAP_FLAG_NONE,
					&ResourceDesc,
					D3D12_BARRIER_LAYOUT_UNDEFINED,
					NULL,
					NULL,
					0,
					NULL,
					&IID_ID3D12Resource,
					&BottomLevelAS[GeometryType].Scratch));
			}

#ifdef _DEBUG
			THROW_ON_FAIL(ID3D12Resource_SetName(BottomLevelAS[GeometryType].Scratch, L"ScratchResource"));
#endif

			// Allocate resources for acceleration structures.
			// Acceleration structures can only be placed in resources that are created in the default heap (or custom heap equivalent). 
			// Default heap is OK since the application doesn't need CPU read/write access to them. 
			// The resources that will contain acceleration structures must be created in the state D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
			// and must have resource flag D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. The ALLOW_UNORDERED_ACCESS requirement simply acknowledges both: 
			//  - the system will be doing this type of access in its implementation of acceleration structure builds behind the scenes.
			//  - from the app point of view, synchronization of writes/reads to acceleration structures is accomplished using UAV barriers.
			{
				D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
				HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
				HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
				ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				ResourceDesc.Alignment = 0;
				ResourceDesc.Width = BottomLevelPrebuildInfo.ResultDataMaxSizeInBytes;
				ResourceDesc.Height = 1;
				ResourceDesc.DepthOrArraySize = 1;
				ResourceDesc.MipLevels = 1;
				ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
				ResourceDesc.SampleDesc.Count = 1;
				ResourceDesc.SampleDesc.Quality = 0;
				ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

				THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
					Device,
					&HeapProperties,
					D3D12_HEAP_FLAG_NONE,
					&ResourceDesc,
					D3D12_BARRIER_LAYOUT_UNDEFINED,
					NULL,
					NULL,
					0,
					NULL,
					&IID_ID3D12Resource,
					&BottomLevelAS[GeometryType].AccelerationStructure));
			}

#ifdef _DEBUG
			THROW_ON_FAIL(ID3D12Resource_SetName(BottomLevelAS[GeometryType].AccelerationStructure, L"BottomLevelAccelerationStructure"));
#endif

			BottomLevelBuildDesc.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(BottomLevelAS[GeometryType].AccelerationStructure);
			BottomLevelBuildDesc.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(BottomLevelAS[GeometryType].Scratch);

			ID3D12GraphicsCommandList7_BuildRaytracingAccelerationStructure(DxObjects.CommandList, &BottomLevelBuildDesc, 0, NULL);

			BottomLevelAS[GeometryType].ResultDataMaxSizeInBytes = BottomLevelPrebuildInfo.ResultDataMaxSizeInBytes;
		}
	}

	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TopLevelBuildDesc = { 0 };
		TopLevelBuildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		TopLevelBuildDesc.Inputs.NumDescs = GEOMETRY_TYPE_COUNT;
		TopLevelBuildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		TopLevelBuildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TopLevelPrebuildInfo = { 0 };
		ID3D12Device10_GetRaytracingAccelerationStructurePrebuildInfo(Device, &TopLevelBuildDesc.Inputs, &TopLevelPrebuildInfo);

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = TopLevelPrebuildInfo.ScratchDataSizeInBytes;
			ResourceDesc.Height = 1;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
				Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&TopLevelAS.Scratch));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(TopLevelAS.Scratch, L"Top Level Scratch Resource"));
#endif

		// Allocate resources for acceleration structures.
		// Acceleration structures can only be placed in resources that are created in the default heap (or custom heap equivalent). 
		// Default heap is OK since the application doesn't need CPU read/write access to them. 
		// The resources that will contain acceleration structures must be created in the state D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
		// and must have resource flag D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. The ALLOW_UNORDERED_ACCESS requirement simply acknowledges both: 
		//  - the system will be doing this type of access in its implementation of acceleration structure builds behind the scenes.
		//  - from the app point of view, synchronization of writes/reads to acceleration structures is accomplished using UAV barriers.

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = TopLevelPrebuildInfo.ResultDataMaxSizeInBytes;
			ResourceDesc.Height = 1;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
				Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&TopLevelASResource));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(TopLevelASResource, L"TopLevelAccelerationStructure"));
#endif

		DxObjects.TopLevelASGPUHandle = ID3D12Resource_GetGPUVirtualAddress(TopLevelASResource);

		// Create instance descs for the bottom-level acceleration structures.
		{
			D3D12_GPU_VIRTUAL_ADDRESS BottomLevelASaddresses[GEOMETRY_TYPE_COUNT] =
			{
				ID3D12Resource_GetGPUVirtualAddress(BottomLevelAS[0].AccelerationStructure),
				ID3D12Resource_GetGPUVirtualAddress(BottomLevelAS[1].AccelerationStructure)
			};

			D3D12_RAYTRACING_INSTANCE_DESC InstanceDescs[GEOMETRY_TYPE_COUNT] = { 0 };

			// Bottom-level AS with a single plane.
			{
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].InstanceMask = 1;
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].InstanceContributionToHitGroupIndex = 0;
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].AccelerationStructure = BottomLevelASaddresses[GEOMETRY_TYPE_TRIANGLE];

				// Width of a bottom-level AS geometry.
				// Make the plane a little larger than the actual number of primitives in each dimension.
				vec4 BottomLevelASWidth = {
					700 * AABB_WIDTH + (700 - 1) * AABB_DISTANCE,
					AABB_WIDTH,
					700 * AABB_WIDTH + (700 - 1) * AABB_DISTANCE,
					0
				};

				vec4 BasePositionVector;
				glm_vec4_mul(BottomLevelASWidth, (vec4) { -0.35f, 0.0f, -0.35f, 0 }, BasePositionVector);

				mat4 ScalingMatrix;
				glm_scale_make(ScalingMatrix, BottomLevelASWidth);

				mat4 TranslationMatrix;
				glm_translate_make(TranslationMatrix, BasePositionVector);

				mat4 TransformationMatrix;
				glm_mat4_mul(TranslationMatrix, ScalingMatrix, TransformationMatrix);

				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[0][0] = TransformationMatrix[0][0];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[0][1] = TransformationMatrix[1][0];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[0][2] = TransformationMatrix[2][0];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[0][3] = TransformationMatrix[3][0];

				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[1][0] = TransformationMatrix[0][1];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[1][1] = TransformationMatrix[1][1];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[1][2] = TransformationMatrix[2][1];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[1][3] = TransformationMatrix[3][1];

				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[2][0] = TransformationMatrix[0][2];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[2][1] = TransformationMatrix[1][2];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[2][2] = TransformationMatrix[2][2];
				InstanceDescs[GEOMETRY_TYPE_TRIANGLE].Transform[2][3] = TransformationMatrix[3][2];
			}

			// Create instanced bottom-level AS with procedural geometry AABBs.
			// Instances share all the data, except for a transform.
			{
				InstanceDescs[GEOMETRY_TYPE_AABB].InstanceMask = 1;

				// Set hit group offset to beyond the shader records for the triangle AABB.
				InstanceDescs[GEOMETRY_TYPE_AABB].InstanceContributionToHitGroupIndex = GEOMETRY_TYPE_AABB * RAY_TYPE_COUNT;
				InstanceDescs[GEOMETRY_TYPE_AABB].AccelerationStructure = BottomLevelASaddresses[GEOMETRY_TYPE_AABB];

				mat4 TranslationMatrix;
				glm_translate_make(TranslationMatrix, (vec4) { 0, AABB_WIDTH / 2, 0 });

				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[0][0] = TranslationMatrix[0][0];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[0][1] = TranslationMatrix[1][0];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[0][2] = TranslationMatrix[2][0];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[0][3] = TranslationMatrix[3][0];

				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[1][0] = TranslationMatrix[0][1];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[1][1] = TranslationMatrix[1][1];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[1][2] = TranslationMatrix[2][1];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[1][3] = TranslationMatrix[3][1];

				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[2][0] = TranslationMatrix[0][2];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[2][1] = TranslationMatrix[1][2];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[2][2] = TranslationMatrix[2][2];
				InstanceDescs[GEOMETRY_TYPE_AABB].Transform[2][3] = TranslationMatrix[3][2];
			}

			{
				D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
				HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
				HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
				ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				ResourceDesc.Alignment = 0;
				ResourceDesc.Width = sizeof(InstanceDescs);
				ResourceDesc.Height = 1;
				ResourceDesc.DepthOrArraySize = 1;
				ResourceDesc.MipLevels = 1;
				ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
				ResourceDesc.SampleDesc.Count = 1;
				ResourceDesc.SampleDesc.Quality = 0;
				ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

				THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
					Device,
					&HeapProperties,
					D3D12_HEAP_FLAG_NONE,
					&ResourceDesc,
					D3D12_BARRIER_LAYOUT_UNDEFINED,
					NULL,
					NULL,
					0,
					NULL,
					&IID_ID3D12Resource,
					&TopLevelAS.InstanceDesc));
			}

			{
				void* MappedData;
				ID3D12Resource_Map(TopLevelAS.InstanceDesc, 0, NULL, &MappedData);
				memcpy(MappedData, InstanceDescs, sizeof(InstanceDescs));
				ID3D12Resource_Unmap(TopLevelAS.InstanceDesc, 0, NULL);
			}
		}

		TopLevelBuildDesc.DestAccelerationStructureData = DxObjects.TopLevelASGPUHandle;
		TopLevelBuildDesc.Inputs.InstanceDescs = ID3D12Resource_GetGPUVirtualAddress(TopLevelAS.InstanceDesc);
		TopLevelBuildDesc.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(TopLevelAS.Scratch);

		ID3D12GraphicsCommandList7_BuildRaytracingAccelerationStructure(DxObjects.CommandList, &TopLevelBuildDesc, 0, NULL);

		TopLevelAS.ResultDataMaxSizeInBytes = TopLevelPrebuildInfo.ResultDataMaxSizeInBytes;
	}

	THROW_ON_FAIL(ID3D12GraphicsCommandList7_Close(DxObjects.CommandList));

	ID3D12CommandQueue_ExecuteCommandLists(DxObjects.CommandQueue, 1, &DxObjects.CommandList);

	WaitForGpu(&DxObjects);

	for (enum GEOMETRY_TYPE GeometryType = 0; GeometryType < GEOMETRY_TYPE_COUNT; GeometryType++)
	{
		THROW_ON_FAIL(ID3D12Resource_Release(BottomLevelAS[GeometryType].Scratch));
	}

	THROW_ON_FAIL(ID3D12Resource_Release(TopLevelAS.Scratch));
	THROW_ON_FAIL(ID3D12Resource_Release(TopLevelAS.InstanceDesc));

	DxObjects.ConstantBufferAlignedInstanceSize = RoundToMultiple(sizeof(struct SceneConstantBuffer), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	ID3D12Resource* ConstantBufferResource;

	{
		D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
		HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ResourceDesc.Alignment = 0;
		ResourceDesc.Width = BUFFER_COUNT * DxObjects.ConstantBufferAlignedInstanceSize;
		ResourceDesc.Height = 1;
		ResourceDesc.DepthOrArraySize = 1;
		ResourceDesc.MipLevels = 1;
		ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		ResourceDesc.SampleDesc.Count = 1;
		ResourceDesc.SampleDesc.Quality = 0;
		ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
			Device,
			&HeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_BARRIER_LAYOUT_UNDEFINED,
			NULL,
			NULL,
			0,
			NULL,
			&IID_ID3D12Resource,
			&ConstantBufferResource));
	}
	
	DxObjects.ConstantBufferGPUAddress = ID3D12Resource_GetGPUVirtualAddress(ConstantBufferResource);

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12Resource_SetName(ConstantBufferResource, L"Scene Constant Buffer"));
#endif

	THROW_ON_FAIL(ID3D12Resource_Map(ConstantBufferResource, 0, NULL, &DxObjects.MappedConstantBufferData));

	ID3D12Resource* StructuredBufferResource;

	{
		D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
		HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ResourceDesc.Alignment = 0;
		ResourceDesc.Width = BUFFER_COUNT * sizeof(DxObjects.StructuredBufferStaging);
		ResourceDesc.Height = 1;
		ResourceDesc.DepthOrArraySize = 1;
		ResourceDesc.MipLevels = 1;
		ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		ResourceDesc.SampleDesc.Count = 1;
		ResourceDesc.SampleDesc.Quality = 0;
		ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
			Device,
			&HeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_BARRIER_LAYOUT_UNDEFINED,
			NULL,
			NULL,
			0,
			NULL,
			&IID_ID3D12Resource,
			&StructuredBufferResource));
	}

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12Resource_SetName(StructuredBufferResource, L"AABB primitive attributes"));
#endif

	DxObjects.StructuredBufferGpuAddress = ID3D12Resource_GetGPUVirtualAddress(StructuredBufferResource);

	THROW_ON_FAIL(ID3D12Resource_Map(StructuredBufferResource, 0, NULL, &DxObjects.MappedBufferPointers));

	ID3D12Resource* RayGenShaderTable;
	ID3D12Resource* MissShaderTable;
	ID3D12Resource* HitGroupShaderTable;
	ID3D12StateObjectProperties* StateObjectProperties;

	{
		void* RayGenShaderID;
		void* MissShaderIDs[RAY_TYPE_COUNT];
		void* HitGroupShaderIDs_TriangleGeometry[RAY_TYPE_COUNT];
		void* HitGroupShaderIDs_AABBGeometry[INTERSECTION_SHADER_TYPE_COUNT][RAY_TYPE_COUNT];

		THROW_ON_FAIL(ID3D12StateObject_QueryInterface(DxObjects.DxrStateObject, &IID_ID3D12StateObjectProperties, &StateObjectProperties));

		RayGenShaderID = ID3D12StateObjectProperties_GetShaderIdentifier(StateObjectProperties, RaygenShaderName);

		for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
		{
			MissShaderIDs[RayType] = ID3D12StateObjectProperties_GetShaderIdentifier(StateObjectProperties, MissShaderNames[RayType]);
		}

		for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
		{
			HitGroupShaderIDs_TriangleGeometry[RayType] = ID3D12StateObjectProperties_GetShaderIdentifier(StateObjectProperties, HitGroupNames_TriangleGeometry[RayType]);
		}

		for (enum INTERSECTION_SHADER_TYPE i = 0; i < INTERSECTION_SHADER_TYPE_COUNT; i++)
		{
			for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
			{
				HitGroupShaderIDs_AABBGeometry[i][RayType] = ID3D12StateObjectProperties_GetShaderIdentifier(StateObjectProperties, HitGroupNames_AABBGeometry[i][RayType]);
			}
		}

		/*************--------- Shader table layout -------*******************
		| --------------------------------------------------------------------
		| Shader table - HitGroupShaderTable:
		| [0] : MyHitGroup_Triangle
		| [1] : MyHitGroup_Triangle_ShadowRay
		| [2] : MyHitGroup_AABB_AnalyticPrimitive
		| [3] : MyHitGroup_AABB_AnalyticPrimitive_ShadowRay
		| ...
		| [6] : MyHitGroup_AABB_VolumetricPrimitive
		| [7] : MyHitGroup_AABB_VolumetricPrimitive_ShadowRay
		| [8] : MyHitGroup_AABB_SignedDistancePrimitive
		| [9] : MyHitGroup_AABB_SignedDistancePrimitive_ShadowRay,
		| ...
		| [20] : MyHitGroup_AABB_SignedDistancePrimitive
		| [21] : MyHitGroup_AABB_SignedDistancePrimitive_ShadowRay
		| --------------------------------------------------------------------
		**********************************************************************/

		DxObjects.RayGenShaderTableWidth = RoundToMultiple(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = DxObjects.RayGenShaderTableWidth;
			ResourceDesc.Height = 1;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
				Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&RayGenShaderTable));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(RayGenShaderTable, L"RayGenShaderTable"));
#endif

		DxObjects.RayGenShaderTableGPUAddress = ID3D12Resource_GetGPUVirtualAddress(RayGenShaderTable);

		{
			void* MappedData;
			THROW_ON_FAIL(ID3D12Resource_Map(RayGenShaderTable, 0, NULL, &MappedData));
			memcpy(MappedData, RayGenShaderID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			ID3D12Resource_Unmap(RayGenShaderTable, 0, NULL);
		}

		DxObjects.MissShaderTableStrideInBytes = RoundToMultiple(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
		DxObjects.MissShaderTableWidth = RAY_TYPE_COUNT * DxObjects.MissShaderTableStrideInBytes;

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = DxObjects.MissShaderTableWidth;
			ResourceDesc.Height = 1;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
				Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&MissShaderTable));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(MissShaderTable, L"MissShaderTable"));
#endif

		DxObjects.MissShaderTableGPUAddress = ID3D12Resource_GetGPUVirtualAddress(MissShaderTable);

		{
			void* MappedData;
			THROW_ON_FAIL(ID3D12Resource_Map(MissShaderTable, 0, NULL, &MappedData));

			for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
			{
				memcpy(MappedData, MissShaderIDs[RayType], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				MappedData = OffsetPointer(MappedData, DxObjects.MissShaderTableStrideInBytes);
			}

			ID3D12Resource_Unmap(MissShaderTable, 0, NULL);
		}

		// Hit group shader table.
		DxObjects.HitGroupShaderTableStrideInBytes = RoundToMultiple(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + MAX_ROOT_ARG_SIZE, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
		DxObjects.HitGroupShaderTableWidth = (RAY_TYPE_COUNT + TOTAL_PRIMITIVE_COUNT * RAY_TYPE_COUNT) * DxObjects.HitGroupShaderTableStrideInBytes;

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = DxObjects.HitGroupShaderTableWidth;
			ResourceDesc.Height = 1;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(
				Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&HitGroupShaderTable));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(HitGroupShaderTable, L"HitGroupShaderTable"));
#endif

		DxObjects.HitGroupShaderTableGPUAddress = ID3D12Resource_GetGPUVirtualAddress(HitGroupShaderTable);

		{
			void* HitGroupShaderTableData;
			THROW_ON_FAIL(ID3D12Resource_Map(HitGroupShaderTable, 0, NULL, &HitGroupShaderTableData));

			// Triangle geometry hit groups.
			for (int i = 0; i < ARRAYSIZE(HitGroupShaderIDs_TriangleGeometry); i++)
			{
				memcpy(HitGroupShaderTableData, HitGroupShaderIDs_TriangleGeometry[i], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

				struct PrimitiveConstantBuffer PlaneMaterialCB = { { 0.9f, 0.9f, 0.9f, 1.0f }, 0.25f, 1, 0.4f, 50, 1 };
				memcpy(OffsetPointer(HitGroupShaderTableData, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES), &PlaneMaterialCB, sizeof(struct PrimitiveConstantBuffer));

				HitGroupShaderTableData = OffsetPointer(HitGroupShaderTableData, DxObjects.HitGroupShaderTableStrideInBytes);
			}
			
			// AABB geometry hit groups.
			// Create a shader record for each primitive.
			for (enum INTERSECTION_SHADER_TYPE IntersectionShaderType = 0, instanceIndex = 0; IntersectionShaderType < INTERSECTION_SHADER_TYPE_COUNT; IntersectionShaderType++)
			{
				int NumPrimitiveTypes;

				switch (IntersectionShaderType)
				{
				case INTERSECTION_SHADER_TYPE_ANALYTIC_PRIMITIVE: 
				{
					NumPrimitiveTypes = ANALYTIC_PRIMITIVE_COUNT;
					break;
				}
				case INTERSECTION_SHADER_TYPE_VOLUMETRIC_PRIMITIVE: 
				{
					NumPrimitiveTypes = VOLUMETRIC_PRIMITIVE_COUNT;
					break;
				}
				case INTERSECTION_SHADER_TYPE_SIGNED_DISTANCE_PRIMITIVE:
				{
					NumPrimitiveTypes = SIGNED_DISTANCE_PRIMITIVE_COUNT;
					break;
				}
				default:
				{
					NumPrimitiveTypes = 0;
					break;
				}
				}

				// Primitives for each intersection shader.
				for (int PrimitiveIndex = 0; PrimitiveIndex < NumPrimitiveTypes; PrimitiveIndex++, instanceIndex++)
				{
					struct ROOTSIG_AABB_ROOTARGS RootArgs = { 0 };
					RootArgs.MaterialCB = AABB_MaterialCB[instanceIndex];
					RootArgs.AABB_CB.instanceIndex = instanceIndex;
					RootArgs.AABB_CB.primitiveType = PrimitiveIndex;

					// Ray types.
					for (enum RAY_TYPE RayType = 0; RayType < RAY_TYPE_COUNT; RayType++)
					{
						memcpy(HitGroupShaderTableData, HitGroupShaderIDs_AABBGeometry[IntersectionShaderType][RayType], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
						memcpy(OffsetPointer(HitGroupShaderTableData, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES), &RootArgs, sizeof(struct ROOTSIG_AABB_ROOTARGS));

						HitGroupShaderTableData = OffsetPointer(HitGroupShaderTableData, DxObjects.HitGroupShaderTableStrideInBytes);
					}
				}
			}
		}
	}

	ID3D12Resource_Unmap(HitGroupShaderTable, 0, NULL);

	THROW_ON_FAIL(ID3D12StateObjectProperties_Release(StateObjectProperties));

	THROW_ON_FALSE(SetWindowLongPtrW(Window, GWLP_WNDPROC, (LONG_PTR)WindowProc) != 0);

	DispatchMessageW(&(MSG) {
		.hwnd = Window,
		.message = WM_INIT,
		.wParam = (WPARAM)&(struct WindowProcPayload)
		{
			.DxObjects = &DxObjects,
			.GpuTimers = &GpuTimers,
			.CameraVectors = &CameraVectors
		},
		.lParam = 0
	});

	DispatchMessageW(&(MSG) {
		.hwnd = Window,
		.message = WM_SIZE,
		.wParam = SIZE_RESTORED,
		.lParam = MAKELONG(WindowRect.right - WindowRect.left, WindowRect.bottom - WindowRect.top)
	});

	MSG Message = { 0 };
	while (Message.message != WM_QUIT)
	{
		if (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&Message);
			DispatchMessageW(&Message);
		}
	}

	WaitForGpu(&DxObjects);
	
	THROW_ON_FAIL(ID3D12Resource_Release(DxObjects.RaytracingOutput));

	for (int i = 0; i < ARRAYSIZE(GpuTimers); i++)
	{
		THROW_ON_FAIL(ID3D12QueryHeap_Release(GpuTimers[i].TimerHeap));
		THROW_ON_FAIL(ID3D12Resource_Release(GpuTimers[i].TimerBuffer));
	}

	THROW_ON_FAIL(ID3D12RootSignature_Release(DxObjects.RaytracingGlobalRootSignature));

	for (int i = 0; i < ARRAYSIZE(RaytracingLocalRootSignature); i++)
		THROW_ON_FAIL(ID3D12RootSignature_Release(RaytracingLocalRootSignature[i]));

	THROW_ON_FAIL(ID3D12GraphicsCommandList7_Release(DxObjects.CommandList));

	THROW_ON_FAIL(ID3D12StateObject_Release(DxObjects.DxrStateObject));

	THROW_ON_FAIL(ID3D12DescriptorHeap_Release(DxObjects.MainDescriptorHeap));
	THROW_ON_FAIL(ID3D12Resource_Release(ConstantBufferResource));
	THROW_ON_FAIL(ID3D12Resource_Release(StructuredBufferResource));
	THROW_ON_FAIL(ID3D12Resource_Release(IndexBufferResource));
	THROW_ON_FAIL(ID3D12Resource_Release(VertexBufferResource));
	THROW_ON_FAIL(ID3D12Resource_Release(AABB_Buffer));

	THROW_ON_FAIL(ID3D12Resource_Release(TopLevelASResource));

	THROW_ON_FAIL(ID3D12Resource_Release(RayGenShaderTable));
	THROW_ON_FAIL(ID3D12Resource_Release(MissShaderTable));
	THROW_ON_FAIL(ID3D12Resource_Release(HitGroupShaderTable));

	for (enum GEOMETRY_TYPE GeometryType = 0; GeometryType < GEOMETRY_TYPE_COUNT; GeometryType++)
	{
		THROW_ON_FAIL(ID3D12Resource_Release(BottomLevelAS[GeometryType].AccelerationStructure));
	}

	for(int i = 0; i < ARRAYSIZE(DxObjects.CommandAllocators); i++)
		THROW_ON_FAIL(ID3D12CommandAllocator_Release(DxObjects.CommandAllocators[i]));

	THROW_ON_FAIL(ID3D12Fence_Release(DxObjects.Fence));

	THROW_ON_FAIL(ID3D12DescriptorHeap_Release(RtvDescriptorHeap));

	for(int i = 0; i < ARRAYSIZE(DxObjects.RenderTargets); i++)
		THROW_ON_FAIL(ID3D12Resource_Release(DxObjects.RenderTargets[i]));

	THROW_ON_FAIL(IDXGISwapChain3_Release(DxObjects.SwapChain));

	THROW_ON_FAIL(ID3D12CommandQueue_Release(DxObjects.CommandQueue));

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12InfoQueue_Release(InfoQueue));
#endif

	THROW_ON_FAIL(ID3D12Device10_Release(Device));

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12Debug6_Release(DebugController));
#endif

	THROW_ON_FALSE(CloseHandle(DxObjects.FenceEvent));

	THROW_ON_FALSE(UnregisterClassW(WindowClassName, Instance));

	THROW_ON_FALSE(DestroyCursor(Cursor));
	THROW_ON_FALSE(DestroyIcon(Icon));

#ifdef _DEBUG
	{
		IDXGIDebug1* DxgiDebug;
		THROW_ON_FAIL(DXGIGetDebugInterface1(0, &IID_IDXGIDebug1, &DxgiDebug));
		THROW_ON_FAIL(IDXGIDebug1_ReportLiveObjects(DxgiDebug, DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
	}
#endif
	return Message.wParam;
}

LRESULT CALLBACK PreInitProc(HWND Window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProcW(Window, message, wParam, lParam);
	}
	return 0;
}

LRESULT CALLBACK IdleProc(HWND Window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_PAINT:
		Sleep(25);
		break;
	case WM_SIZE:
		if (wParam == SIZE_RESTORED)
			THROW_ON_FALSE(SetWindowLongPtrW(Window, GWLP_WNDPROC, (LONG_PTR)WindowProc) != 0);
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProcW(Window, message, wParam, lParam);
	}
	return 0;
}

LRESULT CALLBACK WindowProc(HWND Window, UINT message, WPARAM wParam, LPARAM lParam)
{
	static const UINT64 TICKS_PER_SECOND = 10000000;

	static LARGE_INTEGER QPC_Frequency;
	static LARGE_INTEGER QPC_LastTime;
	static UINT64 QPC_MaxDelta;

	static UINT64 ElapsedTicks = 0;
	static UINT64 TotalTicks = 0;

	static UINT64 QPC_SecondCounter = 0;

	static UINT WindowWidth = 0;
	static UINT WindowHeight = 0;

	static D3D12_VIEWPORT Viewport;
	static D3D12_RECT ScissorRect;
	
	static bool bFullScreen = false;
	static bool bVsync = false;

	static float AnimateGeometryTime = 0.0f;

	static bool bAnimateGeometry = true;
	static bool bAnimateCamera = false;
	static bool bAnimateLight = false;

	static D3D12_GPU_DESCRIPTOR_HANDLE RaytracingOutputResourceUAVGpuDescriptor;
	static UINT RaytracingOutputResourceUAVDescriptorHeapIndex = UINT_MAX;

	static float AspectRatio = 0.0f;
	static struct DxObjects* restrict DxObjects = NULL;
	static struct GPUTimer (*restrict GpuTimers)[GPU_TIMER_COUNT] = NULL;
	static struct CameraVectors* restrict CameraVectors = NULL;

	switch (message)
	{
	case WM_INIT:
		QueryPerformanceFrequency(&QPC_Frequency);
		QueryPerformanceCounter(&QPC_LastTime);

		Viewport.TopLeftX = 0.f;
		Viewport.TopLeftY = 0.f;
		Viewport.MinDepth = D3D12_MIN_DEPTH;
		Viewport.MaxDepth = D3D12_MAX_DEPTH;

		ScissorRect.left = 0;
		ScissorRect.top = 0;

		// Initialize max delta to 1/10 of a second.
		QPC_MaxDelta = QPC_Frequency.QuadPart / 10;

		DxObjects = ((struct WindowProcPayload*)wParam)->DxObjects;
		GpuTimers = ((struct WindowProcPayload*)wParam)->GpuTimers;
		CameraVectors = ((struct WindowProcPayload*)wParam)->CameraVectors;
		break;
	case WM_KEYDOWN:
		switch (wParam)
		{
		case 'C':
			bAnimateCamera = !bAnimateCamera;
			break;
		case 'G':
			bAnimateGeometry = !bAnimateGeometry;
			break;
		case 'L':
			bAnimateLight = !bAnimateLight;
			break;
		case 'V':
			bVsync = !bVsync;
			break;
		}
		break;

	case WM_SYSKEYDOWN:
		if ((wParam == VK_RETURN) && (lParam & (1 << 29)))
		{
			bFullScreen = !bFullScreen;

			if (bFullScreen)
			{
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_EXSTYLE, WS_EX_TOPMOST) != 0);
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_STYLE, 0) != 0);
			}
			else
			{
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_STYLE, WS_OVERLAPPEDWINDOW) != 0);
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_EXSTYLE, 0) != 0);
			}
		
			THROW_ON_FALSE(ShowWindow(Window, SW_SHOWMAXIMIZED));
		}
		break;

	case WM_PAINT:
		{
			LARGE_INTEGER CurrentTime;
			QueryPerformanceCounter(&CurrentTime);

			UINT64 TimeDelta = CurrentTime.QuadPart - QPC_LastTime.QuadPart;

			QPC_LastTime = CurrentTime;
			QPC_SecondCounter += TimeDelta;

			if (TimeDelta > QPC_MaxDelta)
			{
				TimeDelta = QPC_MaxDelta;
			}

			TimeDelta *= TICKS_PER_SECOND;
			TimeDelta /= QPC_Frequency.QuadPart;

			ElapsedTicks = TimeDelta;
			TotalTicks += TimeDelta;
		}
			
		if (QPC_SecondCounter >= (UINT64)QPC_Frequency.QuadPart)
		{
			QPC_SecondCounter %= QPC_Frequency.QuadPart;
		}
				
		{
			static int CyclicTimerFrameCount = 0;
			static double PrevTime = 0.0f;
			double TotalTime = (double)TotalTicks / TICKS_PER_SECOND;
			CyclicTimerFrameCount++;

			if ((TotalTime - PrevTime) >= 1.0f)
			{
				float Diff = (float)(TotalTime - PrevTime);
				float Fps = (float)CyclicTimerFrameCount / Diff;

				CyclicTimerFrameCount = 0;
				PrevTime = TotalTime;
				float RaytracingTime = (*GpuTimers)[GPU_TIMER_RAYTRACING].Avg[0];

				float MRaysPerSecond;

				{
					float ResolutionMRays = WindowWidth * WindowHeight;
					float RaytracingTimeInSeconds = 0.001f * RaytracingTime;
					MRaysPerSecond = ResolutionMRays / (RaytracingTimeInSeconds * 1e+6f);
				}

				wchar_t WindowText[256];

				_snwprintf_s(WindowText, ARRAYSIZE(WindowText), _TRUNCATE,
					L"D3D12 Raytracing - Procedural Geometry:     fps: %.2f    DispatchRays(): %.2fms     ~Million Primary Rays/s: %.2f",
					Fps, RaytracingTime, MRaysPerSecond);

				SetWindowTextW(Window, WindowText);
			}
		}

		{
			float ElapsedSeconds = (double)ElapsedTicks / TICKS_PER_SECOND;

			// Transform the procedural geometry.
			if (bAnimateGeometry)
			{
				AnimateGeometryTime += ElapsedSeconds;
			}

			// Rotate the camera around Y axis.
			if (bAnimateCamera)
			{
				static const float SECONDS_TO_FULL_ROTATION = 48.0f;
				const float AngleToRotateBy = 360.0f * (ElapsedSeconds / SECONDS_TO_FULL_ROTATION);

				mat4 IdentityMatrix;
				glm_mat4_identity(IdentityMatrix);

				mat4 RotationMatrix;
				glm_rotate_y(IdentityMatrix, glm_rad(AngleToRotateBy), RotationMatrix);

				glm_mat4_mulv(RotationMatrix, CameraVectors->EyeVector, CameraVectors->EyeVector);

				glm_mat4_mulv(RotationMatrix, CameraVectors->UpVector, CameraVectors->UpVector);

				glm_mat4_mulv(RotationMatrix, CameraVectors->AtVector, CameraVectors->AtVector);

				UpdateCameraMatrices(DxObjects, AspectRatio, CameraVectors->EyeVector, CameraVectors->AtVector, CameraVectors->UpVector);
			}

			// Rotate the second light around Y axis.
			if (bAnimateLight)
			{
				static const float SECONDS_PER_LIGHT_REVOLUTION = 8.0f;
				float AngleToRotateBy = -360.0f * (ElapsedSeconds / SECONDS_PER_LIGHT_REVOLUTION);

				mat4 IdentityMatrix;
				glm_mat4_identity(IdentityMatrix);

				mat4 RotationMatrix;
				glm_rotate_y(IdentityMatrix, glm_rad(AngleToRotateBy), RotationMatrix);

				glm_mat4_mulv(RotationMatrix, DxObjects->ConstantBufferStaging.lightPosition, DxObjects->ConstantBufferStaging.lightPosition);
			}
		}

		{
			mat4 IdentityMatrix;
			glm_mat4_identity(IdentityMatrix);

			mat4 ScaleMatrix_15Y;
			glm_scale_make(ScaleMatrix_15Y, (vec3) { 1, 1.5, 1 });

			mat4 ScaleMatrix_15;
			glm_scale_make(ScaleMatrix_15, (vec3) { 1.5, 1.5, 1.5 });

			mat4 ScaleMatrix_3;
			glm_scale_make(ScaleMatrix_3, (vec3) { 3, 3, 3 });

			mat4 RotationMatrix;
			glm_rotate_y(IdentityMatrix, -2 * AnimateGeometryTime, RotationMatrix);

			// Apply scale, rotation and translation transforms.
			// The intersection shader tests in this sample work with local space, so here
			// we apply the BLAS object space translation that was passed to geometry descs.

			UINT Offset = 0;
			// Analytic primitives.
			SetTransformForAABB(DxObjects, Offset + ANALYTIC_PRIMITIVE_AABB, ScaleMatrix_15Y, IdentityMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + ANALYTIC_PRIMITIVE_SPHERES, ScaleMatrix_15, RotationMatrix, DxObjects->AABB_Array);
			Offset += ANALYTIC_PRIMITIVE_COUNT;

			// Volumetric primitives.
			SetTransformForAABB(DxObjects, Offset + VOLUMETRIC_PRIMITIVE_METABALLS, ScaleMatrix_15, RotationMatrix, DxObjects->AABB_Array);
			Offset += VOLUMETRIC_PRIMITIVE_COUNT;

			// Signed distance primitives.
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_MINISPHERES, IdentityMatrix, IdentityMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_INTERSECTED_ROUND_CUBE, IdentityMatrix, IdentityMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_SQUARE_TORUS, ScaleMatrix_15, IdentityMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_TWISTED_TORUS, IdentityMatrix, RotationMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_COG, IdentityMatrix, RotationMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_CYLINDER, ScaleMatrix_15Y, IdentityMatrix, DxObjects->AABB_Array);
			SetTransformForAABB(DxObjects, Offset + SIGNED_DISTANCE_PRIMITIVE_FRACTAL_PYRAMID, ScaleMatrix_3, IdentityMatrix, DxObjects->AABB_Array);
		}

		DxObjects->ConstantBufferStaging.elapsedTime = AnimateGeometryTime;

		// Begin frame.
		// Reset command list and allocator.
		THROW_ON_FAIL(ID3D12CommandAllocator_Reset(DxObjects->CommandAllocators[DxObjects->BackBufferIndex]));
		THROW_ON_FAIL(ID3D12GraphicsCommandList7_Reset(DxObjects->CommandList, DxObjects->CommandAllocators[DxObjects->BackBufferIndex], NULL));

		{
			D3D12_TEXTURE_BARRIER TextureBarrier = { 0 };
			TextureBarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
			TextureBarrier.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
			TextureBarrier.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
			TextureBarrier.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
			TextureBarrier.LayoutBefore = D3D12_BARRIER_LAYOUT_PRESENT;
			TextureBarrier.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
			TextureBarrier.pResource = DxObjects->RenderTargets[DxObjects->BackBufferIndex];
			TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			D3D12_BARRIER_GROUP ResourceBarrier = { 0 };
			ResourceBarrier.Type = D3D12_BARRIER_TYPE_TEXTURE;
			ResourceBarrier.NumBarriers = 1;
			ResourceBarrier.pTextureBarriers = &TextureBarrier;
			ID3D12GraphicsCommandList7_Barrier(DxObjects->CommandList, 1, &ResourceBarrier);
		}

		for (int i = 0; i < ARRAYSIZE(*GpuTimers); i++)
		{
			(*GpuTimers)[i].UsedQueries = 0;
		}

		ID3D12GraphicsCommandList7_SetComputeRootSignature(DxObjects->CommandList, DxObjects->RaytracingGlobalRootSignature);

		memcpy(DxObjects->MappedConstantBufferData + DxObjects->BackBufferIndex * DxObjects->ConstantBufferAlignedInstanceSize, &DxObjects->ConstantBufferStaging, sizeof(struct SceneConstantBuffer));
		ID3D12GraphicsCommandList7_SetComputeRootConstantBufferView(
			DxObjects->CommandList,
			ROOTSIG_SLOT_SCENE_CB,
			DxObjects->ConstantBufferGPUAddress + DxObjects->BackBufferIndex * DxObjects->ConstantBufferAlignedInstanceSize
		);

		memcpy(
			DxObjects->MappedBufferPointers + DxObjects->BackBufferIndex * ARRAYSIZE(DxObjects->StructuredBufferStaging),
			&DxObjects->StructuredBufferStaging[0],
			sizeof(DxObjects->StructuredBufferStaging));
		
		ID3D12GraphicsCommandList7_SetComputeRootShaderResourceView(
			DxObjects->CommandList,
			ROOTSIG_SLOT_AABB_ATTRIBUTE_BUFFER, 
			DxObjects->StructuredBufferGpuAddress + DxObjects->BackBufferIndex * sizeof(DxObjects->StructuredBufferStaging));

		ID3D12GraphicsCommandList7_SetDescriptorHeaps(DxObjects->CommandList, 1, &DxObjects->MainDescriptorHeap);
		ID3D12GraphicsCommandList7_SetComputeRootDescriptorTable(DxObjects->CommandList, ROOTSIG_SLOT_VERTEX_BUFFERS, DxObjects->IndexBufferGPUHandle);
		ID3D12GraphicsCommandList7_SetComputeRootDescriptorTable(DxObjects->CommandList, ROOTSIG_SLOT_OUTPUTVIEW, RaytracingOutputResourceUAVGpuDescriptor);

		ID3D12GraphicsCommandList7_SetComputeRootShaderResourceView(DxObjects->CommandList, ROOTSIG_SLOT_ACC_STRUCT, DxObjects->TopLevelASGPUHandle);

		ID3D12GraphicsCommandList7_RSSetViewports(DxObjects->CommandList, 1, &Viewport);
		ID3D12GraphicsCommandList7_RSSetScissorRects(DxObjects->CommandList, 1, &ScissorRect);

		ID3D12GraphicsCommandList7_SetPipelineState1(DxObjects->CommandList, DxObjects->DxrStateObject);

		assert((*GpuTimers)[GPU_TIMER_RAYTRACING].UsedQueries == 0 && "Timer ID sequence break");
		assert(0 < MAX_GPU_TIMERS && "Timer ID out of range");
		ID3D12GraphicsCommandList7_EndQuery(
			DxObjects->CommandList,
			(*GpuTimers)[GPU_TIMER_RAYTRACING].TimerHeap,
			D3D12_QUERY_TYPE_TIMESTAMP,
			(*GpuTimers)[GPU_TIMER_RAYTRACING].UsedQueries++);

		{
			D3D12_DISPATCH_RAYS_DESC DispatchDesc = { 0 };
			DispatchDesc.RayGenerationShaderRecord.StartAddress = DxObjects->RayGenShaderTableGPUAddress;
			DispatchDesc.RayGenerationShaderRecord.SizeInBytes = DxObjects->RayGenShaderTableWidth;
			DispatchDesc.MissShaderTable.StartAddress = DxObjects->MissShaderTableGPUAddress;
			DispatchDesc.MissShaderTable.SizeInBytes = DxObjects->MissShaderTableWidth;
			DispatchDesc.MissShaderTable.StrideInBytes = DxObjects->MissShaderTableStrideInBytes;
			DispatchDesc.HitGroupTable.StartAddress = DxObjects->HitGroupShaderTableGPUAddress;
			DispatchDesc.HitGroupTable.SizeInBytes = DxObjects->HitGroupShaderTableWidth;
			DispatchDesc.HitGroupTable.StrideInBytes = DxObjects->HitGroupShaderTableStrideInBytes;
			DispatchDesc.Width = WindowWidth;
			DispatchDesc.Height = WindowHeight;
			DispatchDesc.Depth = 1;
			ID3D12GraphicsCommandList7_DispatchRays(DxObjects->CommandList, &DispatchDesc);
		}

		ID3D12GraphicsCommandList7_EndQuery(
			DxObjects->CommandList,
			(*GpuTimers)[GPU_TIMER_RAYTRACING].TimerHeap,
			D3D12_QUERY_TYPE_TIMESTAMP,
			(*GpuTimers)[GPU_TIMER_RAYTRACING].UsedQueries++);

		{
			D3D12_TEXTURE_BARRIER PreCopyBarriers[2] = { 0 };
			PreCopyBarriers[0].SyncBefore = D3D12_BARRIER_SYNC_ALL;
			PreCopyBarriers[0].SyncAfter = D3D12_BARRIER_SYNC_COPY;
			PreCopyBarriers[0].AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
			PreCopyBarriers[0].AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST;
			PreCopyBarriers[0].LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
			PreCopyBarriers[0].LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_DEST;
			PreCopyBarriers[0].pResource = DxObjects->RenderTargets[DxObjects->BackBufferIndex];
			PreCopyBarriers[0].Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			PreCopyBarriers[1].SyncBefore = D3D12_BARRIER_SYNC_ALL;
			PreCopyBarriers[1].SyncAfter = D3D12_BARRIER_SYNC_COPY;
			PreCopyBarriers[1].AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
			PreCopyBarriers[1].AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
			PreCopyBarriers[1].LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
			PreCopyBarriers[1].LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
			PreCopyBarriers[1].pResource = DxObjects->RaytracingOutput;
			PreCopyBarriers[1].Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			D3D12_BARRIER_GROUP ResourceBarrier = { 0 };
			ResourceBarrier.Type = D3D12_BARRIER_TYPE_TEXTURE;
			ResourceBarrier.NumBarriers = ARRAYSIZE(PreCopyBarriers);
			ResourceBarrier.pTextureBarriers = PreCopyBarriers;
			ID3D12GraphicsCommandList7_Barrier(DxObjects->CommandList, 1, &ResourceBarrier);
		}

		ID3D12GraphicsCommandList7_CopyResource(DxObjects->CommandList, DxObjects->RenderTargets[DxObjects->BackBufferIndex], DxObjects->RaytracingOutput);

		{
			D3D12_TEXTURE_BARRIER PreCopyBarriers[2] = { 0 };
			PreCopyBarriers[0].SyncBefore = D3D12_BARRIER_SYNC_COPY;
			PreCopyBarriers[0].SyncAfter = D3D12_BARRIER_SYNC_ALL;
			PreCopyBarriers[0].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
			PreCopyBarriers[0].AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
			PreCopyBarriers[0].LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
			PreCopyBarriers[0].LayoutAfter = D3D12_BARRIER_LAYOUT_PRESENT;
			PreCopyBarriers[0].pResource = DxObjects->RenderTargets[DxObjects->BackBufferIndex];
			PreCopyBarriers[0].Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			PreCopyBarriers[1].SyncBefore = D3D12_BARRIER_SYNC_COPY;
			PreCopyBarriers[1].SyncAfter = D3D12_BARRIER_SYNC_RAYTRACING;
			PreCopyBarriers[1].AccessBefore = D3D12_BARRIER_ACCESS_COPY_SOURCE;
			PreCopyBarriers[1].AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
			PreCopyBarriers[1].LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
			PreCopyBarriers[1].LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
			PreCopyBarriers[1].pResource = DxObjects->RaytracingOutput;
			PreCopyBarriers[1].Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			D3D12_BARRIER_GROUP ResourceBarrier = { 0 };
			ResourceBarrier.Type = D3D12_BARRIER_TYPE_TEXTURE;
			ResourceBarrier.NumBarriers = ARRAYSIZE(PreCopyBarriers);
			ResourceBarrier.pTextureBarriers = PreCopyBarriers;
			ID3D12GraphicsCommandList7_Barrier(DxObjects->CommandList, 1, &ResourceBarrier);
		}

		for (int i = 0; i < ARRAYSIZE(*GpuTimers); i++)
		{
			static UINT ResolveToFrameID = 0;
			UINT64 ResolveToBaseAddress = ResolveToFrameID * GPU_TIMER_SLOTS * sizeof(UINT64);
			ID3D12GraphicsCommandList7_ResolveQueryData(DxObjects->CommandList, (*GpuTimers)[i].TimerHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, (*GpuTimers)[i].UsedQueries, (*GpuTimers)[i].TimerBuffer, ResolveToBaseAddress);

			UINT ReadBackFrameID = (ResolveToFrameID + 1) % (BUFFER_COUNT + 1);
			SIZE_T ReadBackBaseOffset = ReadBackFrameID * GPU_TIMER_SLOTS * sizeof(UINT64);
			D3D12_RANGE DataRange =
			{
				ReadBackBaseOffset,
				ReadBackBaseOffset + (*GpuTimers)[i].UsedQueries * sizeof(UINT64),
			};

			{
				UINT64* TimingData;
				THROW_ON_FAIL(ID3D12Resource_Map((*GpuTimers)[i].TimerBuffer, 0, &DataRange, &TimingData));
				memcpy((*GpuTimers)[i].Timing, TimingData, sizeof(UINT64) * (*GpuTimers)[i].UsedQueries);
				ID3D12Resource_Unmap((*GpuTimers)[i].TimerBuffer, 0, NULL);
			}

			for (int j = 0; j < (*GpuTimers)[i].UsedQueries / 2; j++)
			{
				const UINT64 start = (*GpuTimers)[i].Timing[j * 2];
				const UINT64 end = (*GpuTimers)[i].Timing[j * 2 + 1];
				float value = (float)((double)(end - start) * (*GpuTimers)[i].GPU_FreqInv);
				(*GpuTimers)[i].Avg[j] = ((1.f - 0.95f) * value + 0.95f * (*GpuTimers)[i].Avg[j]);
			}

			ResolveToFrameID = ReadBackFrameID;
		}

		THROW_ON_FAIL(ID3D12GraphicsCommandList7_Close(DxObjects->CommandList));

		ID3D12CommandQueue_ExecuteCommandLists(DxObjects->CommandQueue, 1, &DxObjects->CommandList);

		THROW_ON_FAIL(IDXGISwapChain3_Present(DxObjects->SwapChain, bVsync ? 1 : 0, bVsync ? 0 : DXGI_PRESENT_ALLOW_TEARING));

		{
			// Schedule a Signal command in the queue.
			const UINT64 CurrentFenceValue = DxObjects->FenceValues[DxObjects->BackBufferIndex];
			THROW_ON_FAIL(ID3D12CommandQueue_Signal(DxObjects->CommandQueue, DxObjects->Fence, CurrentFenceValue));

			// Update the back buffer index.
			DxObjects->BackBufferIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(DxObjects->SwapChain);

			// If the next frame is not ready to be rendered yet, wait until it is ready.
			if (ID3D12Fence_GetCompletedValue(DxObjects->Fence) < DxObjects->FenceValues[DxObjects->BackBufferIndex])
			{
				THROW_ON_FAIL(ID3D12Fence_SetEventOnCompletion(DxObjects->Fence, DxObjects->FenceValues[DxObjects->BackBufferIndex], DxObjects->FenceEvent));
				THROW_ON_FALSE(WaitForSingleObject(DxObjects->FenceEvent, INFINITE) == WAIT_OBJECT_0);
			}

			// Set the fence value for the next frame.
			DxObjects->FenceValues[DxObjects->BackBufferIndex] = CurrentFenceValue + 1;
		}
		break;
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
		{
			THROW_ON_FALSE(SetWindowLongPtrW(Window, GWLP_WNDPROC, (LONG_PTR)IdleProc) != 0);
			break;
		}

		if (WindowWidth == LOWORD(lParam) && WindowHeight == HIWORD(lParam))
			break;

		WindowWidth = LOWORD(lParam);
		WindowHeight = HIWORD(lParam);

		AspectRatio = (float)WindowWidth / (float)WindowHeight;

		Viewport.Width = WindowWidth;
		Viewport.Height = WindowHeight;

		ScissorRect.right = WindowWidth;
		ScissorRect.bottom = WindowHeight;

		WaitForGpu(DxObjects);

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			if(DxObjects->RenderTargets[i])
				THROW_ON_FAIL(ID3D12Resource_Release(DxObjects->RenderTargets[i]));
			DxObjects->FenceValues[i] = DxObjects->FenceValues[DxObjects->BackBufferIndex];
		}

		THROW_ON_FAIL(IDXGISwapChain3_ResizeBuffers(
			DxObjects->SwapChain,
			BUFFER_COUNT,
			WindowWidth,
			WindowHeight,
			RTV_FORMAT,
			DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));
				
		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			THROW_ON_FAIL(IDXGISwapChain3_GetBuffer(DxObjects->SwapChain, i, &IID_ID3D12Resource, &DxObjects->RenderTargets[i]));

#ifdef _DEBUG
			wchar_t NameBuff[25] = { 0 };
			_snwprintf_s(NameBuff, ARRAYSIZE(NameBuff), _TRUNCATE, L"Render target %i", i);
			THROW_ON_FAIL(ID3D12Resource_SetName(DxObjects->RenderTargets[i], NameBuff));
#endif

			D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = { 0 };
			RtvDesc.Format = RTV_FORMAT;
			RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
						
			D3D12_CPU_DESCRIPTOR_HANDLE RtvDescriptor;
			RtvDescriptor.ptr = DxObjects->RtvDescriptorTop.ptr + i * DxObjects->RtvDescriptorSize;

			ID3D12Device10_CreateRenderTargetView(Device, DxObjects->RenderTargets[i], &RtvDesc, RtvDescriptor);
		}

		DxObjects->BackBufferIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(DxObjects->SwapChain);

		if(DxObjects->RaytracingOutput)
			THROW_ON_FAIL(ID3D12Resource_Release(DxObjects->RaytracingOutput));

		{
			D3D12_HEAP_PROPERTIES HeapProperties = { 0 };
			HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
			HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC1 ResourceDesc = { 0 };
			ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			ResourceDesc.Alignment = 0;
			ResourceDesc.Width = WindowWidth;
			ResourceDesc.Height = WindowHeight;
			ResourceDesc.DepthOrArraySize = 1;
			ResourceDesc.MipLevels = 1;
			ResourceDesc.Format = RTV_FORMAT;
			ResourceDesc.SampleDesc.Count = 1;
			ResourceDesc.SampleDesc.Quality = 0;
			ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			THROW_ON_FAIL(ID3D12Device10_CreateCommittedResource3(Device,
				&HeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&ResourceDesc,
				D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
				NULL,
				NULL,
				0,
				NULL,
				&IID_ID3D12Resource,
				&DxObjects->RaytracingOutput));
		}

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(DxObjects->RaytracingOutput, L"RaytracingOutput"));
#endif

		if (RaytracingOutputResourceUAVDescriptorHeapIndex >= DESCRIPTOR_COUNT)
		{
			assert(DxObjects->DescriptorsAllocated < DESCRIPTOR_COUNT && L"Ran out of descriptors on the heap!");
			RaytracingOutputResourceUAVDescriptorHeapIndex = DxObjects->DescriptorsAllocated++;
		}

		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = { 0 };
			UavDesc.Format = DXGI_FORMAT_UNKNOWN;
			UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

			D3D12_CPU_DESCRIPTOR_HANDLE UavDescriptorHandle;
			UavDescriptorHandle.ptr = DxObjects->DescriptorHeap_CpuHandle.ptr + RaytracingOutputResourceUAVDescriptorHeapIndex * DxObjects->CbvDescriptorSize;
				
			ID3D12Device10_CreateUnorderedAccessView(Device, DxObjects->RaytracingOutput, NULL, &UavDesc, UavDescriptorHandle);
		}

		RaytracingOutputResourceUAVGpuDescriptor.ptr = DxObjects->DescriptorHeap_GpuHandle.ptr + RaytracingOutputResourceUAVDescriptorHeapIndex * DxObjects->CbvDescriptorSize;

		UpdateCameraMatrices(DxObjects, AspectRatio, CameraVectors->EyeVector, CameraVectors->AtVector, CameraVectors->UpVector);
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProcW(Window, message, wParam, lParam);
	}

	return 0;
}
